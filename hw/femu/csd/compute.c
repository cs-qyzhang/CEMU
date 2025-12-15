#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include "compute.h"
#include "memory.h"
#include "qemu/atomic.h"
#include "hw/femu/backend/backend.h"
#include "hw/femu/inc/slab.h"
#include "hw/femu/nvme-def.h"
#include "hw/femu/nvme.h"
#include "hw/femu/param.h"
#include "sched.h"
#include "sysemu/cpus.h"
#include "sysemu/runstate.h"
#include "sysemu/cpu-timers.h"

#define MAX_PIND    1024
#define MAX_RSID    1024

static inline ComputeNamespace *compute_ns(NvmeNamespace *ns)
{
    return (ComputeNamespace *)ns->private;
}

struct csd_thread_arg {
    NvmeNamespace *ns;
    int id;
};

static void *compute_thread(void *arg);

struct ProgramInitArgs {
    uint32_t nsid;
    uint16_t pind;
};

static void init_program_slab(void *elem, void *arg)
{
    struct ProgramInitArgs *args = (struct ProgramInitArgs *)arg;
    Program *p = (Program *)elem;
    p->nsid = args->nsid;
    p->pind = args->pind++;
    p->state = PROGRAM_STATE_INVALID;
    pthread_mutex_init(&p->lock, NULL);
}

static void init_mrs_slab(void *elem, void *arg)
{
    int *rsid = (int *)arg;
    MemoryRangeSet *mrs = (MemoryRangeSet *)elem;
    mrs->rsid = (*rsid)++;
    mrs->mr = NULL;
    mrs->numr = 0;
    pthread_spin_init(&mrs->lock, 0);
}

static void compute_init(NvmeNamespace *ns, Error **errp)
{
    ComputeParams *param = ns->params;
    ComputeNamespace *cns = malloc(sizeof(ComputeNamespace));
    ns->private = cns;
    cns->ns = ns;
    cns->params = param;

    // program slab init
    struct ProgramInitArgs init_arg;
    init_arg.nsid = ns->id;
    init_arg.pind = 1;
    slab_init(&cns->programs, sizeof(Program), MAX_PIND, init_program_slab, &init_arg);

    // rsid slab init
    int rsid = 1;
    slab_init(&cns->mrs, sizeof(MemoryRangeSet), MAX_RSID, init_mrs_slab, &rsid);

    cns->to_csd = g_malloc0(sizeof(struct rte_ring *) * param->nr_thread);
    cns->compute_threads = g_malloc0(sizeof(QemuThread) * param->nr_thread);
    struct csd_thread_arg* args = g_malloc0(sizeof(struct csd_thread_arg) * param->nr_thread);
    for (int i = 0; i < param->nr_thread; i++) {
        cns->to_csd[i] = femu_ring_create(FEMU_RING_TYPE_SP_SC, FEMU_MAX_INF_REQS);
        char name[32];
        sprintf(name, "FEMU-CSD-Thread-%d", i);
        args[i].ns = ns;
        args[i].id = i;
        qemu_thread_create(&cns->compute_threads[i], name, compute_thread, &args[i],
                           QEMU_THREAD_JOINABLE);
    }

    // init scheduler
    sched_init(ns);
}

static uint16_t memory_range_set_management(NvmeNamespace *ns, NvmeCmd *cmd, NvmeCqe *cqe)
{
    FemuCtrl *n = ns->ctrl;
    NvmeMemoryRangeSetManageCmd *manage = (NvmeMemoryRangeSetManageCmd *)cmd;
    uint16_t rsid = le16_to_cpu(manage->rsid);
    uint32_t sel = manage->sel;
    uint32_t numr = manage->numr;
    uint64_t prp1 = le64_to_cpu(manage->prp1);
    uint64_t prp2 = le64_to_cpu(manage->prp2);

    femu_debug("memory_range_set_management: sel %u, rsid %u, numr %u, prp1 %lx, prp2 %lx\n", sel, rsid, numr, prp1, prp2);

    if (sel == 0) {
        // add memory range set
        // read memory range descriptors
        if (rsid != 0) {
            femu_err("memory_range_set_management: rsid %u != 0 when sel == 0!\n", rsid);
            return NVME_INVALID_FIELD;
        }
        if (numr == 0 || numr >= 128) {
            femu_err("memory_range_set_management: numr %u not supported!\n", numr);
            return NVME_INVALID_FIELD;
        }

        NvmeMemoryRange mr[128];
        if (dma_write_prp(n, (void *)mr, numr * sizeof(NvmeMemoryRange), prp1, prp2)) {
            femu_err("memory_range_set_management: dma_write_prp error\n");
            return NVME_DNR;
        }

        MemoryRangeSet *mrs = slab_alloc(memory_range_set_slab(ns), 1);
        if (mrs == NULL) {
            return NVME_MR_SET_EXCEEDED;
        }
        mrs->in_use = false;
        mrs->numr = numr;
        mrs->mr = g_malloc0(sizeof(MemoryRange) * numr);
        mrs->mr_addr = g_malloc0(sizeof(void *) * numr);
        mrs->mr_len = g_malloc0(sizeof(long long *) * numr);
        for (int i = 0; i < numr; i++) {
            uint32_t nsid = le32_to_cpu(mr[i].nsid);
            uint32_t len = le32_to_cpu(mr[i].len);
            uint64_t sb = le64_to_cpu(mr[i].sb);
            NvmeNamespace *mns = nvme_find_namespace(n, nsid);
            if (mns == NULL) {
                free(mrs->mr);
                slab_free(memory_range_set_slab(ns), mrs, 1);
                return NVME_INVALID_MEMORY_NS;
            }
            mrs->mr[i].addr = backend_addr(mns->backend, sb);
            mrs->mr[i].nsid = nsid;
            mrs->mr[i].len = len;
            mrs->mr[i].sb = sb;
            mrs->mr_addr[i] = mrs->mr[i].addr;
            mrs->mr_len[i] = mrs->mr[i].len;
        }

        femu_log("create mrs rsid %d, mrs %p, numr %d\n", mrs->rsid, mrs, mrs->numr);
        cqe->n.result = mrs->rsid;
    } else if (sel == 1) {
        // remove memory range set
        if (rsid == 0 || rsid > MAX_RSID) {
            femu_err("memory_range_set_management: rsid %u invalid!\n", rsid);
            return NVME_INVALID_MR_SET_ID;
        }
        pthread_spin_lock(&memory_range_set_slab(ns)->lock);
        MemoryRangeSet *mrs = memory_range_set_get(ns, rsid);
        femu_log("free mrs rsid %d, mrs %p, numr %d\n", rsid, mrs, mrs->numr);
        if (mrs == NULL || mrs->numr == 0) {
            pthread_spin_unlock(&memory_range_set_slab(ns)->lock);
            femu_err("memory_range_set_management: rsid %u not found!\n", rsid);
            return NVME_INVALID_FIELD;
        }
        if (mrs->in_use) {
            pthread_spin_unlock(&memory_range_set_slab(ns)->lock);
            return NVME_MR_SET_IN_USE;
        }
        mrs->numr = 0;
        pthread_spin_unlock(&memory_range_set_slab(ns)->lock);
        free(mrs->mr);
        free(mrs->mr_addr);
        free(mrs->mr_len);
        slab_free(memory_range_set_slab(ns), mrs, 1);
        cqe->n.result = 0;
    } else {
        femu_err("memory_range_set_management: sel %u not supported!\n", sel);
        return NVME_INVALID_FIELD;
    }

    return NVME_SUCCESS;
}


static uint16_t parse_program(Program *program, char **csf_path, char **csf_name)
{
    int csf_name_len = strlen(program->code);
    if (csf_name_len >= MIN(4096, program->size)) {
        femu_err("load_shared_lib: shared library file name exceeds 4096!\n");
        return NVME_INVALID_PROGRAM_DATA;
    }

    int program_name_len = strlen(program->code + csf_name_len + 1);
    if (program_name_len >= 4096) {
        femu_err("load_shared_lib: shared library program function name exceeds 4096!\n");
        return NVME_INVALID_PROGRAM_DATA;
    }
    if (program_name_len + csf_name_len + 2 > program->size) {
        femu_err("load_shared_lib: shared library file name and program function name format error!\n");
        return NVME_INVALID_PROGRAM_DATA;
    }

    char *path = program->code;
    if (path[0] == '.') {
        // path relative to CEMU/tests/cemu
        g_autofree char *file_dir = g_path_get_dirname(__FILE__);
        g_autofree char *current_dir = realpath(file_dir, NULL);
        g_autofree char *src_dir = g_build_filename(current_dir, "../../../tests/cemu", NULL);
        g_autofree char *tmp_path = g_build_filename(src_dir, path, NULL);
        path = g_strdup(tmp_path);
    }

    *csf_path = path;
    *csf_name = program->code + csf_name_len + 1;
    femu_debug("parse_program: csf_path %s, csf_name %s\n", *csf_path, *csf_name);

    return NVME_SUCCESS;
}

/*
 * user data format for shared library:
 * | so_name | csf_name |
 */
static uint16_t load_shared_lib(Program *program)
{
    program->shared_lib.jit_fn = NULL;

    char *so_name;
    char *program_name;

    uint16_t ret = parse_program(program, &so_name, &program_name);
    if (ret != NVME_SUCCESS) {
        return ret;
    }

    void *dl_handle = NULL;
    femu_debug("load .so: %s\n", so_name);
    dl_handle = dlopen(so_name, RTLD_NOW);
    if (dl_handle == NULL) {
        femu_err("load_shared_lib: dlopen(%s) error: %s\n", so_name, dlerror());
        return NVME_INVALID_PROGRAM_DATA;
    }

    program->shared_lib.jit_fn = dlsym(dl_handle, program_name);
    if (program->shared_lib.jit_fn == NULL) {
        femu_err("load_shared_lib: dlsym(%s) error: %s\n", program_name, dlerror());
        return NVME_INVALID_PROGRAM_DATA;
    }
    program->shared_lib.so_handle = dl_handle;

    femu_debug("load_shared_lib: lib %s, program %s\n", so_name, program_name);

    return NVME_SUCCESS;
}

// user data is eBPF bytecode
static uint16_t load_ubpf(Program *program, int jit)
{
    char *ebpf_path;
    char *ebpf_name;
    ssize_t ret = parse_program(program, &ebpf_path, &ebpf_name);
    if (ret != NVME_SUCCESS) {
        return ret;
    }

    // read eBPF code from file
    int ebpf_fd = open(ebpf_path, O_RDONLY);
    if (ebpf_fd == -1) {
        perror("open");
        exit(1);
    }
    long program_size = lseek(ebpf_fd, 0, SEEK_END);
    lseek(ebpf_fd, 0, SEEK_SET);
    char *ebpf_code = aligned_alloc(4096, program_size);
    ret = read(ebpf_fd, ebpf_code, program_size);
    if (ret != program_size) {
        perror("read");
        exit(1);
    }
    close(ebpf_fd);

    char *errmsg = NULL;
    program->ebpf.jit_fn = NULL;

    program->ebpf.vm = ubpf_create();
    if (program->ebpf.vm == NULL) {
        femu_err("load_ubpf: ubpf_create error\n");
        return NVME_INVALID_PROGRAM_DATA;
    }

    if (ubpf_load_elf(program->ebpf.vm, ebpf_code, program_size, &errmsg)) {
        femu_err("load_ubpf: ubpf_load_elf error: %s\n", errmsg);
        return NVME_INVALID_PROGRAM_DATA;
    }

    if (jit) {
        program->ebpf.jit_fn = ubpf_compile(program->ebpf.vm, &errmsg);
        if (program->ebpf.jit_fn == NULL) {
            femu_err("load_ubpf: ubpf_compile error: %s\n", errmsg);
            return NVME_INVALID_PROGRAM_DATA;
        }
    }

    femu_debug("load_ubpf: downloaded ubpf success\n");
    return NVME_SUCCESS;
}

static uint16_t unload_shared_lib(Program *program)
{
    if (program->shared_lib.so_handle == NULL) {
        femu_err("unload_shared_lib: shared_lib already unloaded!\n");
        return NVME_SUCCESS;
    }
    int ret = dlclose(program->shared_lib.so_handle);
    if (ret != 0) {
        femu_err("unload_shared_lib: dlclose error: %s\n", dlerror());
        return NVME_DNR;
    }
    program->shared_lib.jit_fn = NULL;
    program->shared_lib.so_handle = NULL;
    femu_debug("unload_shared_lib: shared_lib unloaded!\n");
    return NVME_SUCCESS;
}

static uint16_t unload_ubpf(Program *program)
{
    if (program->ebpf.vm == NULL) {
        femu_err("unload_ubpf: ebpf already unloaded!\n");
        return NVME_SUCCESS;
    }
    ubpf_destroy(program->ebpf.vm);
    program->ebpf.vm = NULL;
    program->ebpf.jit_fn = NULL;
    femu_debug("unload_ubpf: ebpf unloaded!\n");
    return NVME_SUCCESS;
}

static uint16_t load_program(NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    FemuCtrl *n = ns->ctrl;
    NvmeLoadProgramCmd *load = (NvmeLoadProgramCmd *)cmd;
    int ptype = load->ptype;
    int sel = load->sel;
    int pit = load->pit;
    int jit = load->jit;
    int indirect = load->indirect;
    uint16_t runtime_scale = le16_to_cpu(load->runtime_scale);
    uint32_t runtime = le32_to_cpu(load->runtime);
    uint16_t pind = le16_to_cpu(load->pind);
    uint32_t psize = le32_to_cpu(load->psize);
    uint32_t numb = le32_to_cpu(load->numb);
    uint32_t loff = le32_to_cpu(load->loff);    // load offset
    uint64_t pid = le64_to_cpu(load->pid);
    uint64_t prp1 = le64_to_cpu(load->prp1);
    uint64_t prp2 = le64_to_cpu(load->prp2);

    femu_log("load_program: sel %d, indirect %d, runtime %u, runtime_scale %f, ptype %d, pind %d, psize %d, numb %d, loff %d, pid %ld, prp1 %lx, prp2 %lx\n",
               sel, indirect, runtime, runtime_scale / 10.0, ptype, pind, psize, numb, loff, pid, prp1, prp2);

    // get program
    if (pind == 0 || pind > MAX_PIND) {
        femu_err("load_program: pind %u invalid!\n", pind);
        return NVME_INVALID_PIND;
    }
    Program *program = program_get(ns, pind);

    pthread_mutex_lock(&program->lock);
    if (sel == 0) {
        // load program
        if (qatomic_load_acquire(&program->jobs_running) != 0) {
            pthread_mutex_unlock(&program->lock);
            femu_err("load_program: program %u is running (jobs_running %d)!\n", pind, qatomic_load_acquire(&program->jobs_running));
            return NVME_PROGRAM_IN_USE;
        }

        if (program->state == PROGRAM_STATE_ACTIVATED ||
            program->state == PROGRAM_STATE_LOADED) {
            femu_debug("load_program: program %u already activated!\n", pind);
        }

        if (loff == 0) {
            // first time load
            if (pit == 1) {
                program->pid = pid;
            }
            if (program->type >= PROGRAM_TYPE_INVALID) {
                pthread_mutex_unlock(&program->lock);
                femu_err("load_program: program %u type %u not supported!\n", pind, program->type);
                return NVME_INVALID_PTYPE;
            }
            program->size = psize;
            program->type = ptype;
            qatomic_set(&program->jobs_running, 0);
            program->code = malloc(psize);
            program->state = PROGRAM_STATE_LOADING;
            program->runtime = runtime;
            program->runtime_scale = runtime_scale / 10.0;
            program->is_indirect = indirect;
        } else {
            // loff != 0
            if (psize != program->size) {
                pthread_mutex_unlock(&program->lock);
                femu_err("load_program: program %u size mismatch!\n", pind);
                return NVME_INVALID_FIELD;
            }
            if (pit == 1 && pid != program->pid) {
                pthread_mutex_unlock(&program->lock);
                femu_err("load_program: program %u pid mismatch!\n", pind);
                return NVME_INVALID_FIELD;
            }
            if (ptype != program->type) {
                pthread_mutex_unlock(&program->lock);
                femu_err("load_program: program %u type mismatch!\n", pind);
                return NVME_INVALID_PTYPE;
            }
        }

        // read program code
        if (dma_write_prp(n, program->code + loff, numb, prp1, prp2)) {
            pthread_mutex_unlock(&program->lock);
            femu_err("load_program: dma_write_prp error\n");
            return NVME_DNR;
        }
        program->load_size += numb;

        int ret = 0;
        if (program->load_size == program->size) {
            // program has transferred, start loading
            switch (program->type) {
            case PROGRAM_TYPE_SHARED_LIB:
                ret = load_shared_lib(program);
                break;
            case PROGRAM_TYPE_EBPF:
                ret = load_ubpf(program, jit);
                break;
            default:
                pthread_mutex_unlock(&program->lock);
                femu_err("load_program: program %u type %u not supported!\n", pind, program->type);
                return NVME_INVALID_PTYPE;
            }
            if (ret) {
                pthread_mutex_unlock(&program->lock);
                femu_err("load_program: load program error\n");
                return ret;
            }
            // TODO: error handle, delete program
            program->state = PROGRAM_STATE_LOADED;
        }
    } else {
        // unload program
        // TODO add jobs_running check
        if (program->state == PROGRAM_STATE_ACTIVATED) {
            femu_err("unload program: program %u is activated!\n", pind);
        }
        program->state = PROGRAM_STATE_INVALID;
        free(program->code);
        if (program->type == PROGRAM_TYPE_SHARED_LIB) {
            unload_shared_lib(program);
        } else if (program->type == PROGRAM_TYPE_EBPF) {
            unload_ubpf(program);
        }
        femu_debug("unload program: program %u unloaded!\n", pind);
    }
    pthread_mutex_unlock(&program->lock);
    return NVME_SUCCESS;
}

static uint16_t program_activation(NvmeNamespace *ns, NvmeCmd *cmd, NvmeCqe *cqe)
{
    NvmeProgramActivationCmd *activation = (NvmeProgramActivationCmd *)cmd;
    uint16_t pind = le16_to_cpu(activation->pind);
    int sel = activation->sel;

    femu_debug("program_activation: sel %d, pind %d\n", sel, pind);

    // get program
    if (pind == 0 || pind > MAX_PIND) {
        femu_err("program_activation: pind %u invalid!\n", pind);
        return NVME_INVALID_PIND;
    }
    Program *program = program_get(ns, pind);

    pthread_mutex_lock(&program->lock);
    if (sel == 0) {
        // deactivate program
        // TODO fix jobs_running when cu>1
        // if (qatomic_load_acquire(&program->jobs_running) > 0) {
        //     pthread_mutex_unlock(&program->lock);
        //     femu_err("program deactivation: program %u is running (%d)!\n", pind, qatomic_load_acquire(&program->jobs_running));
        //     return NVME_PROGRAM_IN_USE;
        // }
        program->state = PROGRAM_STATE_LOADED;
        femu_debug("program deactivation: program %u deactivated!\n", pind);
    } else if (sel == 1) {
        // activate program
        if (program->state != PROGRAM_STATE_LOADED) {
            pthread_mutex_unlock(&program->lock);
            femu_err("program_activation: program %u not loaded!\n", pind);
            return NVME_INVALID_PROGRAM_DATA;
        }
        program->state = PROGRAM_STATE_ACTIVATED;
    } else {
        pthread_mutex_unlock(&program->lock);
        femu_err("program_activation: sel %u not supported!\n", sel);
        return NVME_INVALID_FIELD;
    }
    pthread_mutex_unlock(&program->lock);
    return NVME_SUCCESS;
}

// Do parameter check and preparation for program execution, the actual
// execution is done in run_on_host()
static uint16_t program_execute(NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    FemuCtrl *n = ns->ctrl;
    NvmeProgramExecuteCmd *exec = (NvmeProgramExecuteCmd *)cmd;
    uint16_t pind = le16_to_cpu(exec->pind);
    uint16_t rsid = le16_to_cpu(exec->rsid);
    uint32_t numr = le32_to_cpu(exec->numr);
    uint32_t dlen = le32_to_cpu(exec->dlen);
    uint64_t cparam1 = le64_to_cpu(exec->cparam1);
    uint64_t cparam2 = le64_to_cpu(exec->cparam2);
    uint64_t prp1 = le64_to_cpu(exec->prp1);
    uint64_t prp2 = le64_to_cpu(exec->prp2);
    uint32_t chunk_nlb = le32_to_cpu(exec->chunk_nlb) + 1;
    uint32_t runtime = le32_to_cpu(exec->runtime);

    femu_debug("program_execute: stime %lu, pind %u, rsid %u, numr %u, dlen %u, "
               "cparam1 %lx, cparam2 %lx, chunk_nlb %d, group %d, prp1 %lx, prp2 %lx\n",
               req->stat.stime, pind, rsid, numr, dlen, cparam1, cparam2, chunk_nlb,
               le32_to_cpu(exec->group), prp1, prp2);

    // get program
    if (pind == 0 || pind > MAX_PIND) {
        femu_err("program_execute: pind %u invalid!\n", pind);
        return NVME_INVALID_PIND;
    }
    if (numr && dlen < sizeof(NvmeMemoryRange) * numr) {
        femu_err("program_execute: dlen %u less than 32 * numr(%d)!\n", dlen, numr);
        return NVME_INVALID_FIELD;
    }

    Program *program = program_get(ns, pind);
    if (program->state != PROGRAM_STATE_ACTIVATED) {
        femu_err("program_execute: program %u not activated!\n", pind);
        return NVME_PROGRAM_NOT_ACTIVATED;
    }
    if (program->is_indirect && numr == 0) {
        femu_err("program_execute: indirect program %u doesn't provide lba list!\n", pind);
        return NVME_INVALID_FIELD;
    }
    if (program->is_indirect && rsid == 0) {
        femu_err("program_execute: indirect program %u doesn't provide rsid!\n", pind);
        return NVME_INVALID_FIELD;
    }
    // if (program->is_indirect && numr * sizeof(NvmeCopyFormat) < dlen) {
    //     femu_err("program_execute: indirect program %u incorrect dlen or numr!\n", pind);
    //     return NVME_INVALID_FIELD;
    // }
    if (!program->is_indirect && numr == 0 && rsid == 0) {
        femu_err("program_execute: numr %u and rsid %u is zero!\n", numr, rsid);
        return NVME_INVALID_FIELD;
    }
    if (program->is_indirect && dlen < 16) {
        femu_err("program_execute: indirect program %u dlen %u less than 14!\n", pind, dlen);
        return NVME_INVALID_FIELD;
    }

    // read data buffer
    char *data_buffer = NULL;
    if (dlen) {
        data_buffer = malloc(dlen); // free in sched.c:job_finish()
        if (dma_write_prp(n, (void *)data_buffer, dlen, prp1, prp2)) {
            femu_err("program_execute: dma_write_prp error, dlen %u\n", dlen);
            return NVME_DNR;
        }
    }
    req->data_buffer = data_buffer;

    // get all memory range
    void **mr_addr = NULL;
    long long *mr_len = NULL;
    if (program->is_indirect || numr == 0) {
        // memory range set is in rsid
        if (rsid == 0 || rsid > MAX_RSID) {
            femu_err("program_execute: rsid %d invalid!\n", rsid);
            return NVME_INVALID_MR_SET_ID;
        }
        MemoryRangeSet *mrs = memory_range_set_get(ns, rsid);
        mr_addr = mrs->mr_addr;
        mr_len = mrs->mr_len;

        if (program->is_indirect) {
            req->mem_ctrl = n;
            req->nr_sres = 0;
            req->sres = NULL;

            femu_log("program_execute: data_buffer %p, dlen %u\n", data_buffer, dlen);
            // parse indirect task from data buffer
            IndirectTask *task = &req->indirect_task;
            task->raw_data_buffer = data_buffer;
            int *raw_task = (int*)data_buffer;
            task->nr_concurrent_chunks = raw_task[0];
            task->destination = raw_task[1];
            task->nr_total_input_cf2 = raw_task[2];
            task->nr_total_output_cf2 = raw_task[3];
            task->chunk_nlb = chunk_nlb;
            femu_log("program_execute: task->nr_concurrent_chunks %d, task->destination %d, task->chunk_nlb %d, task->nr_total_input_cf2 %d, task->nr_total_output_cf2 %d\n", task->nr_concurrent_chunks, task->destination, task->chunk_nlb, task->nr_total_input_cf2, task->nr_total_output_cf2);

            // check parameters
            if (task->nr_concurrent_chunks == 0) {
                femu_err("program_execute: indirect program %u nr_concurrent_chunks is 0!\n", pind);
                return NVME_INVALID_FIELD;
            }
            if (task->destination != 0 && task->destination != 1) {
                femu_err("program_execute: indirect program %u destination %d not supported!\n", pind, task->destination);
                return NVME_INVALID_FIELD;
            }
            if (task->nr_total_input_cf2 == 0) {
                femu_err("program_execute: indirect program %u nr_total_input_cf2 is 0!\n", pind);
                return NVME_INVALID_FIELD;
            }
            if (task->destination == 1 && task->nr_total_output_cf2 == 0) {
                femu_err("program_execute: indirect program %u destination is 1 but nr_total_output_cf2 is 0!\n", pind);
                return NVME_INVALID_FIELD;
            }
            if (task->chunk_nlb == 0) {
                femu_err("program_execute: indirect program %u chunk_nlb is 0!\n", pind);
                return NVME_INVALID_FIELD;
            }
            if (mrs->numr < task->nr_concurrent_chunks || mrs->numr % task->nr_concurrent_chunks != 0) {
                femu_err("program_execute: indirect program %u memory range set numr %u not divisible by nr_concurrent_chunks %d!\n", pind, mrs->numr, task->nr_concurrent_chunks);
                return NVME_INVALID_FIELD;
            }
            int required_dlen = 16 + task->nr_concurrent_chunks * sizeof(int) + task->nr_total_input_cf2 * sizeof(NvmeCopyFormat);
            if (task->destination == 1) {
                required_dlen += task->nr_concurrent_chunks * sizeof(int) + task->nr_total_output_cf2 * sizeof(NvmeCopyFormat);
            }
            if (dlen < required_dlen) {
                femu_err("program_execute: indirect program %u data buffer (dlen %u) too short, required %d!\n", pind, dlen, required_dlen);
                return NVME_INVALID_FIELD;
            }

            task->nr_input_cf2 = raw_task + 4;
            for (int i = 0; i < task->nr_concurrent_chunks; i++) {
                femu_log("program_execute: task->nr_input_cf2[%d] %d\n", i, task->nr_input_cf2[i]);
            }
            if (task->destination == 1) {
                // output in NVM
                task->nr_output_cf2 = task->nr_input_cf2 + task->nr_concurrent_chunks;
                data_buffer = (void*)(task->nr_output_cf2 + task->nr_concurrent_chunks);
                for (int i = 0; i < task->nr_concurrent_chunks; i++) {
                    femu_log("program_execute: task->nr_output_cf2[%d] %d\n", i, task->nr_output_cf2[i]);
                }
            } else {
                // output in FDM
                task->nr_output_cf2 = NULL;
                data_buffer = (void*)(task->nr_input_cf2 + task->nr_concurrent_chunks);
            }
            femu_log("program_execute: data_buffer %p, dlen %u\n", data_buffer, dlen);
            int total_input_cf2 = 0;
            int total_output_cf2 = 0;
            task->nr_total_nlb = 0;
            task->nr_total_output_nlb = 0;
            task->nr_output_nlb = malloc(sizeof(int) * task->nr_concurrent_chunks);
            task->nr_finished_nlb = malloc(sizeof(int) * task->nr_concurrent_chunks);
            task->nr_finished_output_nlb = malloc(sizeof(int) * task->nr_concurrent_chunks);
            task->iter_finished = malloc(sizeof(int) * task->nr_concurrent_chunks);
            for (int i = 0; i < task->nr_concurrent_chunks; i++) {
                total_input_cf2 += task->nr_input_cf2[i];
                task->nr_total_nlb += task->chunk_nlb;
                task->nr_output_nlb[i] = 0;
                task->nr_finished_nlb[i] = 0;
                task->nr_finished_output_nlb[i] = 0;
                task->iter_finished[i] = 0;
                if (task->destination == 1) {
                    total_output_cf2 += task->nr_output_cf2[i];
                }
            }

            if (total_input_cf2 != task->nr_total_input_cf2) {
                femu_err("program_execute: indirect program %u nr_total_input_cf2 mismatch, got %d, expected %d!\n", pind, total_input_cf2, task->nr_total_input_cf2);
                return NVME_INVALID_FIELD;
            }
            if (task->destination == 1 && total_output_cf2 != task->nr_total_output_cf2) {
                femu_err("program_execute: indirect program %u nr_total_output_cf2 mismatch, got %d, expected %d!\n", pind, total_output_cf2, task->nr_total_output_cf2);
                return NVME_INVALID_FIELD;
            }

            // generate per-chunk sre_iter
            task->iter_in = malloc(sizeof(struct sre_iter) * task->nr_concurrent_chunks);
            task->iter_out = malloc(sizeof(struct sre_iter) * task->nr_concurrent_chunks);
            memset(task->iter_in, 0, sizeof(struct sre_iter) * task->nr_concurrent_chunks);
            memset(task->iter_out, 0, sizeof(struct sre_iter) * task->nr_concurrent_chunks);
            NvmeCopyFormat2 *sres = (NvmeCopyFormat2 *)data_buffer;
            NvmeCopyFormat2 *sres_out = sres + task->nr_total_input_cf2;
            int max_sres_per_chunk = 0;
            for (int i = 0; i < task->nr_concurrent_chunks; i++) {
                task->iter_in[i].sres = sres;
                sres += task->nr_input_cf2[i];
                task->iter_in[i].nr_sres = task->nr_input_cf2[i];
                task->iter_in[i].nlb = task->chunk_nlb;
                max_sres_per_chunk = MAX(max_sres_per_chunk, task->nr_input_cf2[i]);
                if (task->destination == 1) {
                    task->iter_out[i].sres = sres_out;
                    sres_out += task->nr_output_cf2[i];
                    task->iter_out[i].nr_sres = task->nr_output_cf2[i];
                    task->iter_out[i].nlb = task->chunk_nlb;
                    max_sres_per_chunk = MAX(max_sres_per_chunk, task->nr_output_cf2[i]);
                }
            }
            femu_log("program_execute: data_buffer %p, dlen %u\n", data_buffer, dlen);
            data_buffer += (task->nr_total_input_cf2 + task->nr_total_output_cf2) * sizeof(NvmeCopyFormat);
            dlen -= (data_buffer - task->raw_data_buffer);
            req->data_buffer = data_buffer;

            if (mrs->numr < 2) {
                femu_err("program_execute: indirect program %u memory range set numr %u less than 2!\n", pind, mrs->numr);
                return NVME_INVALID_FIELD;
            }

            // mr[1] is input buffer
            if (chunk_nlb * 512 > mr_len[1]) {
                femu_err("program_execute: indirect program %u input buffer size %lld less than chunk_nlb %d!\n", pind, mr_len[1], chunk_nlb);
                return NVME_INVALID_FIELD;
            }
            req->is_write = 0;
        }
        numr = mrs->numr;
    } else {
        // memory range set is in data buffer
        if (rsid != 0) {
            femu_err("program_execute: rsid %d != 0 when numr !=0!\n", rsid);
            return NVME_INVALID_FIELD;
        }
        if (data_buffer == NULL) {
            return NVME_INVALID_FIELD;
        }
        NvmeMemoryRange *mrs = (NvmeMemoryRange *)data_buffer;
        mr_addr = malloc(sizeof(void *) * numr);
        mr_len = malloc(sizeof(long long) * numr);
        for (int i = 0; i < numr; i++) {
            uint32_t nsid = le32_to_cpu(mrs[i].nsid);
            uint32_t len = le32_to_cpu(mrs[i].len);
            uint64_t sb = le64_to_cpu(mrs[i].sb);
            NvmeNamespace *mns = nvme_find_namespace(n, nsid);
            void *addr = backend_addr(mns->backend, sb);
            mr_addr[i] = addr;
            mr_len[i] = len;
        }
        data_buffer += sizeof(NvmeMemoryRange) * numr;
        dlen -= sizeof(NvmeMemoryRange) * numr;
    }

    sched_alloc_job(compute_ns(req->ns), req);
    ComputeJob *job = req->job;
    job->user_runtime = runtime ? runtime : program->runtime;
    job->program = program;
    job->args.numr = numr;
    job->args.mr_addr = mr_addr;
    job->args.mr_len = mr_len;
    job->args.cparam1 = cparam1;
    job->args.cparam2 = cparam2;
    job->args.data_buffer = dlen ? data_buffer : NULL;
    job->args.buffer_len = dlen;
    if (!program->is_indirect) {
        sched_enqueue_job(compute_ns(req->ns), req);
    }
    int rc = femu_ring_enqueue(compute_ns(req->ns)->to_csd[req->stat.exec.thread], (void *)&req, 1);
    if (rc != 1) {
        femu_err("enqueue failed, ret=%d\n", rc);
    }

    if (!program->is_indirect) {
        qatomic_inc(&program->jobs_running);
    }

    return NVME_SUCCESS;
}

static QemuMutex compute_mutex;  // 计算锁
static QemuCond compute_cond;
static int compute_active = 0;
static int64_t compute_owner = -1;
static void __attribute__((constructor)) init_sync_objects(void) {
    qemu_mutex_init(&compute_mutex);
    qemu_cond_init(&compute_cond);
}
static void __attribute__((destructor)) cleanup_sync_objects(void) {
    qemu_mutex_destroy(&compute_mutex);
    qemu_cond_destroy(&compute_cond);
}
// static void enter_compute_section(void) {
//     qemu_mutex_lock_iothread();
//     cpu_disable_ticks();
//     pause_all_vcpus();
// }
// static void leave_compute_section(void) {
//     cpu_enable_ticks();
//     resume_all_vcpus();
//     qemu_mutex_unlock_iothread();
// }

static void enter_compute_section(void) {
    qemu_mutex_lock(&compute_mutex);
    compute_active++;
    if (compute_active == 1 && compute_owner == -1) {
        qemu_mutex_lock_iothread();  // 只有第一个计算线程加锁
        compute_owner = qemu_get_thread_id();
        cpu_disable_ticks();
        pause_all_vcpus();
    }
    qemu_mutex_unlock(&compute_mutex);
}
static void leave_compute_section(void) {
    qemu_mutex_lock(&compute_mutex);
    compute_active--;
    if (qemu_get_thread_id() == compute_owner) {
        while (compute_active != 0) {
            qemu_cond_wait(&compute_cond, &compute_mutex);
        }
        cpu_enable_ticks();
        resume_all_vcpus();
        qemu_mutex_unlock_iothread();
        compute_owner = -1;
    }
    else if(compute_active == 0)
    {
        qemu_cond_broadcast(&compute_cond);
    }
    qemu_mutex_unlock(&compute_mutex);
}

// static void enter_compute_section(void) {
//     qemu_mutex_lock(&compute_mutex);
//     if (compute_owner == -1) {
//         qemu_mutex_lock_iothread();  // 只有第一个计算线程加锁
//         compute_owner = qemu_get_thread_id();
//         cpu_disable_ticks();
//         pause_all_vcpus();
//     }
//     qemu_mutex_unlock(&compute_mutex);
// }

// static void leave_compute_section(void) {
//     qemu_mutex_lock(&compute_mutex);
//     if (qemu_get_thread_id() == compute_owner) {
//         cpu_enable_ticks();
//         resume_all_vcpus();
//         qemu_mutex_unlock_iothread();
//         compute_owner = -1;
//     }
//     qemu_mutex_unlock(&compute_mutex);
// }

static uint64_t run_functional_modeling(ComputeJob *job)
{
    Program *program = job->program;
    uint64_t res = 0;
    uint64_t realtime;
    uint64_t runtime = job->user_runtime;

    femu_debug("run_functional_modeling: program %u, runtime %lu, size %llu\n", program->pind, runtime, job->args.mr_len[0]);

    // execute
    // if (!runtime) {
        struct timespec ts,te;
        struct timespec cs,ce;
        clock_gettime(CLOCK_MONOTONIC, &cs);
        enter_compute_section();
        clock_gettime(CLOCK_MONOTONIC, &ce);
        uint64_t time_cost = (ce.tv_sec-cs.tv_sec)* 1000000000LL + (ce.tv_nsec-cs.tv_nsec);
        if(runtime < time_cost)
            runtime = 0;
        else
            runtime -= time_cost;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        switch (program->type) {
        case PROGRAM_TYPE_SHARED_LIB:
            res = program->shared_lib.jit_fn(&job->args);
            break;
        case PROGRAM_TYPE_EBPF:
            if (program->ebpf.jit_fn == NULL) {
                femu_debug("running non-jit ebpf...\n");
                if (ubpf_exec(program->ebpf.vm, &job->args, &res) < 0) {
                    res = -1;
                    femu_err("running ebpf: ubpf_exec error\n");
                }
            } else {
                femu_debug("running jit ebpf...\n");
                res = program->ebpf.jit_fn(&job->args);
            }
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &te);
        realtime = (te.tv_sec-ts.tv_sec)* 1000000000LL + (te.tv_nsec-ts.tv_nsec);
        if(!runtime)
        {
            if (program->runtime_scale)
                runtime = realtime * program->runtime_scale;
        }
        leave_compute_section();
    // }

    femu_debug("run_on_host: program %u, runtime %lu, realtime: %lu, size %llu\n", program->pind, runtime, realtime,job->args.mr_len[0]);

    set_sched_runtime(job, runtime);
    return res;
}

static int compute_io_cmd(NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_COMPUTE_EXEC:
        req->status = program_execute(ns, cmd, req);
        return req->status != NVME_SUCCESS;
    case NVME_CMD_COMPUTE_LOAD:
        req->status = load_program(ns, cmd, req);
        break;
    default:
        femu_err("compute_io_cmd: opcode %u not supported!\n", cmd->opcode);
        req->status = NVME_INVALID_OPCODE;
        break;
    }
    return 1;
}

static uint16_t compute_admin_cmd(NvmeNamespace *ns, NvmeCmd *cmd, NvmeCqe *cqe)
{
    switch (cmd->opcode) {
    case NVME_CMD_COMPUTE_MRS_MGMT:
        return memory_range_set_management(ns, cmd, cqe);
    case NVME_CMD_COMPUTE_ACTIVATE:
        return program_activation(ns, cmd, cqe);
    default:
        femu_err("compute_io_cmd: opcode %u not supported!\n", cmd->opcode);
        return NVME_INVALID_OPCODE;
    }
}

static inline int next_sres(struct sre_iter *iter, NvmeCopyFormat *out_sres)
{
    int nlb = iter->nlb;
    int nr_sres = 0;
    int sre_done = iter->done;
    int pos = iter->pos;
    if (iter->pos >= iter->nr_sres) {
        return 0;
    }
    while (nlb && pos < iter->nr_sres) {
        int n = MIN(nlb, iter->sres[pos].nlb + 1 - sre_done);
        out_sres[nr_sres].cf2.slba = iter->sres[pos].slba + sre_done;
        out_sres[nr_sres].cf2.nlb = n - 1;
        out_sres[nr_sres].cf2.snsid = 1;
        sre_done += n;
        nlb -= n;
        nr_sres++;
        if (sre_done == iter->sres[pos].nlb + 1) {
            pos++;
            sre_done = 0;
        }
    }
    iter->done = sre_done;
    iter->pos = pos;
    return nr_sres;
}

static NvmeRequest *req_dup(NvmeRequest *req)
{
    NvmeRequest *new_req = malloc(sizeof(NvmeRequest));
    memcpy(new_req, req, sizeof(NvmeRequest));
    sched_alloc_job(compute_ns(new_req->ns), new_req);
    memcpy(new_req->job, req->job, sizeof(ComputeJob));
    new_req->job->req = new_req;
    sched_enqueue_job(compute_ns(new_req->ns), new_req);
    return new_req;
}

static int cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static pqueue_pri_t get_pri(void *a)
{
    return ((NvmeRequest *)a)->stat.expire_time;
}

static void set_pri(void *a, pqueue_pri_t pri)
{
    ((NvmeRequest *)a)->stat.expire_time = pri;
}

static size_t get_pos(void *a)
{
    return ((NvmeRequest *)a)->pos;
}

static void set_pos(void *a, size_t pos)
{
    ((NvmeRequest *)a)->pos = pos;
}

static void *indirect_main(void *arg)
{
    NvmeRequest *indirect_req = arg;
    NvmeProgramExecuteCmd *exec = (NvmeProgramExecuteCmd *)&indirect_req->cmd;
    NvmeNamespace *ns = indirect_req->ns;
    ComputeNamespace *cns = compute_ns(indirect_req->ns);
    FemuCtrl *n = ns->ctrl;
    IndirectTask *task = &indirect_req->indirect_task;
    uint64_t stime = indirect_req->stat.stime;
    bool dest_in_nvm = task->destination == 1;
    struct sre_iter *iter = NULL;

    uint64_t input_time = 0;
    uint64_t output_time = 0;
    uint64_t compute_time = 0;
    uint64_t start = clock_ns();

    int chunk_nlb = task->chunk_nlb;
    int parallel_chunks = task->nr_concurrent_chunks;
    int left_chunks = 0;

    struct rte_ring *ring = femu_ring_create(FEMU_RING_TYPE_MP_SC, 128);
    if (ring == NULL) {
        femu_err("indirect_main: create ring failed!\n");
        abort();
    }

    void **mr_addr = indirect_req->job->args.mr_addr;
    long long *mr_len = indirect_req->job->args.mr_len;
    int numr_per_chunk = indirect_req->job->args.numr / parallel_chunks;

    femu_log("indirect main thread start: parallel_chunks %d, chunk_nlb %d\n", parallel_chunks, chunk_nlb);
    femu_log("indirect main thread mem_ctrl %p\n", indirect_req->mem_ctrl);

    NvmeRequest **reqs = malloc(sizeof(NvmeRequest *) * parallel_chunks);
    for (int i = 0; i < parallel_chunks; i++) {
        reqs[i] = req_dup(indirect_req);
        reqs[i]->indirect_task.chunk_id = i;

        reqs[i]->job->args.mr_addr = mr_addr + numr_per_chunk * i;
        reqs[i]->job->args.mr_len = mr_len + numr_per_chunk * i;
        reqs[i]->job->args.numr = numr_per_chunk;
        reqs[i]->indirect_task.ring = ring;
        reqs[i]->indirect_task.stage = 0;

        iter = &task->iter_in[i];
        if (iter->nr_sres == 0) {
            reqs[i]->sres = NULL;
            continue;
        }

        // additional sres for chunk memory copy command
        reqs[i]->sres = malloc(sizeof(NvmeCopyFormat2) * iter->nr_sres);
        // generate next io request
        reqs[i]->nr_sres = next_sres(iter, reqs[i]->sres);
        if (reqs[i]->nr_sres == 0) {
            femu_err("indirect main thread: no more input sres!\n");
            left_chunks--;
        }
        reqs[i]->sdaddr = reqs[i]->job->args.mr_addr[1];
        reqs[i]->mem_ctrl = n;
        reqs[i]->indirect_task.stage = 0;
        reqs[i]->is_write = 0;
        femu_ring_enqueue(n->to_ftl[1], (void *)&reqs[i], 1);
        left_chunks++;
        qatomic_inc(&reqs[i]->job->program->jobs_running);
    }

    pqueue_t *pq = pqueue_init(FEMU_MAX_INF_REQS, cmp_pri, get_pri, set_pri,
                               get_pos, set_pos);
    femu_log("indirect finished init pqueue\n");

    while (true) {
        // wait io
        NvmeRequest *req;
        uint64_t now = clock_ns();
        if (!femu_ring_empty(ring)) {
            // req has finished ftl
            femu_ring_dequeue(ring, (void *)&req, 1);
            if (req->indirect_task.stage == 0) {
                // input finish
                femu_debug("indirect input io finish, chunk %d, iter %d, stime %lu, expire_time %lu, lat %u\n", req->indirect_task.chunk_id, req->indirect_task.iter_finished[req->indirect_task.chunk_id], req->stat.stime, req->stat.expire_time, req->stat.reqlat);
                input_time += req->stat.reqlat;
                req->indirect_task.stage = 1;
                pqueue_insert(pq, req);
            } else if (req->indirect_task.stage == 1) {
                // compute finish, prepare next io
                femu_debug("indirect compute finish, chunk %d, iter %d, stime %lu, expire_time %lu, lat %u, left_chunks %d\n", req->indirect_task.chunk_id, req->indirect_task.iter_finished[req->indirect_task.chunk_id], req->stat.stime, req->stat.expire_time, req->stat.reqlat, left_chunks);
                compute_time += req->stat.reqlat;
                if (dest_in_nvm && req->cqe.res64 > 0) {
                    // to stage 2
                    req->indirect_task.stage = 2;

                    iter = &req->indirect_task.iter_out[req->indirect_task.chunk_id];
                    iter->nlb = req->cqe.res64;
                    req->nr_sres = next_sres(iter, req->sres);
                    if (req->nr_sres == 0) {
                        // error! no more output sres!
                        left_chunks--;
                        femu_err("indirect main thread: no more output sres!\n");
                    } else {
                        req->sdaddr = req->job->args.mr_addr[0]; // output in mr[0]
                        pqueue_insert(pq, req);
                    }
                } else {
                    // to stage 0
                    iter = &req->indirect_task.iter_in[req->indirect_task.chunk_id];
                    req->nr_sres = next_sres(iter, req->sres);
                    req->indirect_task.stage = 0;

                    if (req->nr_sres == 0) {
                        // no more input, chunk finished
                        left_chunks--;
                    } else {
                        req->sdaddr = req->job->args.mr_addr[1]; // output in mr[0]
                        pqueue_insert(pq, req);
                    }
                    req->indirect_task.iter_finished[req->indirect_task.chunk_id]++;
                }
            } else if (req->indirect_task.stage == 2) {
                // output finish, prepare next io
                femu_debug("indirect output io finish, chunk %d, iter %d, stime %lu, expire_time %lu, lat %u, left_chunks %d\n", req->indirect_task.chunk_id, req->indirect_task.iter_finished[req->indirect_task.chunk_id], req->stat.stime, req->stat.expire_time, req->stat.reqlat, left_chunks);
                struct sre_iter *iter = &req->indirect_task.iter_in[req->indirect_task.chunk_id];
                req->nr_sres = next_sres(iter, req->sres);
                if (req->nr_sres == 0) {
                    // no more input, chunk finished
                    left_chunks--;
                } else {
                    req->sdaddr = req->job->args.mr_addr[1]; // input in mr[1]
                    req->indirect_task.stage = 0;
                    pqueue_insert(pq, req);
                }
                req->indirect_task.iter_finished[req->indirect_task.chunk_id]++;
            }
        }

        // femu_log("indirect pqueue size %zu\n", pqueue_size(pq));
        while ((req = pqueue_peek(pq))) {
            now = clock_ns();
            if (now >= req->stat.expire_time) {
                // io finish
                pqueue_pop(pq);
                if (req->indirect_task.stage == 0) {
                    // input
                    femu_debug("indirect start io, expire_time %lu, now %lu, diff %lu\n", req->stat.expire_time, now, now - req->stat.expire_time);
                    output_time += now - req->stat.stime;
                    req->stat.stime = now;
                    req->stat.expire_time = now;
                    req->stat.reqlat = 0;
                    req->stat.pcie_lat = 0;
                    req->is_write = 0;
                    femu_ring_enqueue(n->to_ftl[1], (void *)&req, 1);
                } else if (req->indirect_task.stage == 1) {
                    // compute
                    sched_enqueue_indirect(cns, req->job);
                    req->stat.stime = now;
                    req->stat.expire_time = now;
                    req->stat.reqlat = 0;
                    req->stat.pcie_lat = 0;
                    uint64_t res = run_functional_modeling(req->job);
                    req->cqe.res64 = res;
                    femu_debug("indirect start compute, expire_time %lu, now %lu, diff %lu, res %lu\n", req->stat.expire_time, now, now - req->stat.expire_time, res);
                } else if (req->indirect_task.stage == 2) {
                    // output
                    femu_debug("indirect start output io, expire_time %lu, now %lu, diff %lu\n", req->stat.expire_time, now, now - req->stat.expire_time);
                    output_time += now - req->stat.stime;
                    req->stat.stime = now;
                    req->stat.expire_time = now;
                    req->stat.reqlat = 0;
                    req->stat.pcie_lat = 0;
                    req->is_write = 1;
                    femu_ring_enqueue(n->to_ftl[1], (void *)&req, 1);
                }
            } else {
                break;
            }
        }

        // femu_log("indirect left_chunks %d\n", left_chunks);
        if (left_chunks == 0)
            break;
    }

    femu_log("indirect finish\n");
    for (int i = 0; i < parallel_chunks; i++) {
        if (reqs[i]->sres) {
            sched_job_finish_indirect(reqs[i]->job);
            free(reqs[i]->sres);
        }
        free(reqs[i]);
    }
    free(task->raw_data_buffer);
    free(task->nr_output_nlb);
    free(task->nr_finished_nlb);
    free(task->nr_finished_output_nlb);
    free(task->iter_finished);
    free(task->iter_in);
    free(task->iter_out);

    pqueue_free(pq);
    femu_ring_free(ring);
    free(reqs);

    exec->opcode = INDIRECT_THREAD_FINISH;
    indirect_req->stat.stime = stime;
    femu_ring_enqueue(cns->to_csd[0], (void *)&indirect_req, 1);

    uint64_t end = clock_ns();
    femu_log("indirect: total time %zu, input time %zu, compute time %zu, output time %zu\n", end - start, input_time, compute_time, output_time);

    return NULL;
}

static void *compute_thread(void *arg)
{
    NvmeNamespace *ns = ((struct csd_thread_arg *)arg)->ns;
    ComputeNamespace *cns = compute_ns(ns);
    FemuCtrl *n = ns->ctrl;
    int id = ((struct csd_thread_arg *)arg)->id;

    static Slab indirect_thread_pool;
    if (id == 0)
        slab_init(&indirect_thread_pool, sizeof(QemuThread),
                MAX_INDIRECT_JOBS, NULL, NULL);

    while (!ns->ctrl->dataplane_started) {
        usleep(100000);
    }

    struct rte_ring *to_csd = cns->to_csd[id];

    femu_log("csd_thread %d start\n", id);

    // int next_poller = 1;

    while (1) {
        if (femu_ring_empty(to_csd))
            continue;

        NvmeRequest *req = NULL;
        size_t rc = femu_ring_dequeue(to_csd, (void *)&req, 1);
        if (rc != 1) {
            printf("FEMU: FTL to_ftl dequeue failed\n");
        }

        ComputeJob *job = req->job;
        NvmeCmd *cmd = &req->cmd;

        uint16_t status = 0;
        switch (cmd->opcode) {
        case NVME_CMD_COMPUTE_EXEC:
            if (job_is_indirect(job)) {
                req->indirect_thread = slab_alloc(&indirect_thread_pool, 1);
                if (req->indirect_thread == NULL) {
                    femu_err("compute_thread: slab_alloc indirect_thread_pool failed\n");
                    break;
                }
                qemu_thread_create(req->indirect_thread, "indirect_scheduler",
                        indirect_main, req, QEMU_THREAD_JOINABLE);
            } else {
                req->cqe.res64 = run_functional_modeling(req->job);
            }
            break;
        case INDIRECT_THREAD_FINISH:
            qemu_thread_join(req->indirect_thread);
            slab_free(&indirect_thread_pool, req->indirect_thread, 1);
            cmd->opcode = NVME_CMD_COMPUTE_EXEC;
            femu_ring_enqueue(n->to_poller[1], (void *)&req, 1);
            break;
        default:
            femu_err("compute_thread: UNKNOWN OPCODE %d\n", cmd->opcode);
            abort();
        }

        req->status = status;
        if (status != NVME_SUCCESS) {
            femu_err("Error IO processed! opcode %d\n", cmd->opcode);
        }

        // femu_debug("csd_thread %d: opcode %d, status %d\n", id, cmd->opcode, status);
    }

    return NULL;
}

static int compute_decode_params(const cJSON *json, NvmeNamespace *ns)
{
    ComputeParams *params = malloc(sizeof(ComputeParams));
    ns->params = params;
    char *algo = NULL;

    DECODE_PARAM(json, "nr_cu", Number, params->nr_cu, 4);
    DECODE_PARAM(json, "nr_thread", Number, params->nr_thread, 4);
    DECODE_PARAM(json, "time_slice", Number, params->csf_sched_option.time_slice, 200000);
    DECODE_PARAM(json, "context_switch_time", Number, params->csf_sched_option.context_switch_time, 200);
    DECODE_PARAM(json, "csf_runtime_scale", Number, params->csf_runtime_scale, 3);
    DECODE_PARAM(json, "csf_sched_option", String, algo, (char *)"rr");
    params->csf_sched_option.algo = strdup(algo);
    DECODE_PARAM(json, "grouped_csf", Number, params->csf_sched_option.take_care_of_group, 0);
    DECODE_PARAM(json, "grouped_csf_prio", Number,
          params->csf_sched_option.take_care_of_group_prio, 0);
    return 0;
}

static int compute_encode_params(cJSON *json, NvmeNamespace *ns)
{
    ComputeParams *params = ns->params;

    ENCODE_PARAM(json, "nr_cu", Number, params->nr_cu);
    ENCODE_PARAM(json, "nr_thread", Number, params->nr_thread);
    ENCODE_PARAM(json, "time_slice", Number, params->csf_sched_option.time_slice);
    ENCODE_PARAM(json, "context_switch_time", Number, params->csf_sched_option.context_switch_time);
    ENCODE_PARAM(json, "csf_runtime_scale", Number, params->csf_runtime_scale);
    ENCODE_PARAM(json, "csf_sched_option", String, params->csf_sched_option.algo);
    ENCODE_PARAM(json, "grouped_csf", Number, params->csf_sched_option.take_care_of_group);
    ENCODE_PARAM(json, "grouped_csf_prio", Number,
          params->csf_sched_option.take_care_of_group_prio);
    return 0;
}

static CommandSetOps compute_ns_ops = {
    .state            = NULL,
    .init             = compute_init,
    .exit             = NULL,
    .rw_check_req     = NULL,
    .admin_cmd        = compute_admin_cmd,
    .io_cmd           = compute_io_cmd,
    .get_log          = NULL,
    .encode_params    = compute_encode_params,
    .decode_params    = compute_decode_params,
};

nvme_register(NVME_CSI_COMPUTE, &compute_ns_ops);
