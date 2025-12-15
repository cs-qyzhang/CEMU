#include "hw/femu/backend/backend.h"
#include "hw/femu/inc/rte_ring.h"
#include "hw/femu/nvme-def.h"
#include "hw/femu/nvme.h"
#include "memory.h"

static inline uint16_t check_param(NvmeNamespace *ns, uint64_t sb, uint32_t len, const char *func)
{
    if (sb % 4 || len % 4) {
        femu_err("%s: sb %lu, len %u not dword-aligned!\n", func, sb, len);
        return NVME_INVALID_FIELD;
    }
    if (sb + len > ns->backend->size) {
        femu_err("%s: sb (%lu) + fl (%u) > backend size (%lu)!\n", func, sb, len, ns->backend->size);
        return NVME_INVALID_FIELD;
    }
    return NVME_SUCCESS;
}

static uint16_t memory_read(NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    FemuCtrl *n = ns->ctrl;
    NvmeMemoryReadCmd *read = (NvmeMemoryReadCmd *)cmd;
    uint64_t prp1 = le64_to_cpu(read->prp1);
    uint64_t prp2 = le64_to_cpu(read->prp2);
    uint64_t sb = le64_to_cpu(read->sb);
    uint32_t rl = le32_to_cpu(read->rl);

    femu_debug("memory_read: sb %zu, rl %u, prp1 %zu, prp2 %zu\n", sb, rl, prp1, prp2);

    uint16_t ret = check_param(ns, sb, rl, "memory_read");
    if (ret) {
        return ret;
    }

    if (dma_read_prp(n, backend_addr(ns->backend, sb), rl, prp1, prp2)) {
        femu_err("memory_read: dma_read_prp error\n");
        return NVME_DNR;
    }
    req->stat.pcie_lat = 0;
    req->stat.data_size = rl;
    req->stat.reqlat += rl / 2;
    req->stat.expire_time += rl / 2;
    req->cqe.n.result = rl;
    femu_debug("memory_read lat: %lu, cmd time: %u\n", clock_ns() - req->stat.stime, req->stat.reqlat);

    return NVME_SUCCESS;
}

static uint16_t memory_write(NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    FemuCtrl *n = ns->ctrl;
    NvmeMemoryWriteCmd *write = (NvmeMemoryWriteCmd *)cmd;
    uint64_t prp1 = le64_to_cpu(write->prp1);
    uint64_t prp2 = le64_to_cpu(write->prp2);
    uint64_t sb = le64_to_cpu(write->sb);
    uint32_t wl = le32_to_cpu(write->wl);

    femu_debug("memory_write: sb %zu, wl %u\n", sb, wl);

    uint16_t ret = check_param(ns, sb, wl, "memory_write");
    if (ret) {
        return ret;
    }

    if (dma_write_prp(n, backend_addr(ns->backend, sb), wl, prp1, prp2)) {
        femu_err("memory_write: dma_read_prp error\n");
        return NVME_DNR;
    }

    req->stat.pcie_lat = 0;
    req->stat.data_size = wl;
    req->stat.reqlat += wl / 2;
    req->stat.expire_time += wl / 2;
    req->cqe.n.result = wl;
    return NVME_SUCCESS;
}

static uint16_t memory_fill(NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    NvmeMemoryFillCmd *fill = (NvmeMemoryFillCmd *)cmd;
    uint64_t sb = le64_to_cpu(fill->sb);
    uint32_t fl = le32_to_cpu(fill->fl);

    femu_debug("memory_fill\n");

    uint16_t ret = check_param(ns, sb, fl, "memory_fill");
    if (ret) {
        return ret;
    }

    backend_fill(ns->backend, sb, fl);
    req->stat.pcie_lat = 0;
    req->stat.data_size = fl;
    req->stat.reqlat += fl / 2;
    req->stat.expire_time += fl / 2;
    req->stat.opcode = (cmd->nsid << 4) + (cmd->opcode & 0x0f);
    req->cqe.n.result = fl;
    return NVME_SUCCESS;
}

// called in bbssd/ftl.c

static void *memory_region_backend_addr(uint64_t sdaddr, uint64_t len, FemuCtrl **n, uint16_t *ret)
{
    for (int i = 0; i < nr_csd_memory_region; i++) {
        if (sdaddr >= csd_memory_region[i].dma_addr &&
            sdaddr < csd_memory_region[i].dma_addr + csd_memory_region[i].size) {
            if (sdaddr + len > csd_memory_region[i].dma_addr + csd_memory_region[i].size) {
                *ret = NVME_INVALID_FIELD;
                return NULL;
            }
            *n = csd_memory_region[i].n;
            *ret = 0;
            return (void *)(csd_memory_region[i].backend_addr + sdaddr - csd_memory_region[i].dma_addr);
        }
    }
    *ret = NVME_INVALID_FIELD;
    return NULL;
}

/*
 * copy format 2: nvm -> fdm
 * copy format 3: fdm -> nvm
 * copy format 4: fdm -> fdm
 */
static uint16_t memory_copy(NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    FemuCtrl *n = ns->ctrl;
    NvmeMemoryCopyCmd *copy = (NvmeMemoryCopyCmd *)cmd;
    uint64_t len = le64_to_cpu(copy->len);
    uint64_t prp1 = le64_to_cpu(copy->prp1);
    uint64_t prp2 = le64_to_cpu(copy->prp2);
    uint64_t sdaddr = le64_to_cpu(copy->sdaddr);
    NvmeCopyFormat cf[128];
    NvmeCopyFormat *sres = cf;
    uint16_t ret;
    void *data = memory_region_backend_addr(sdaddr, len, &req->mem_ctrl, &ret);

    if (ret) {
        return ret;
    }

    if (req->mem_ctrl != n) {
        femu_debug("memory copy: source and destination belong to different controllers!\n");
    }

    // cdft == 4: fdm -> fdm
    if (copy->cdft < 2 || copy->cdft > 4) {
        femu_err("memory copy: copy descriptor format %u not supported!\n", copy->cdft);
        return NVME_INVALID_FIELD;
    }

    if (copy->cdft == 2 || copy->cdft == 3) {
        // nvm<->fdm
        req->sres = malloc(sizeof(NvmeCopyFormat) * copy->nr);
        req->nr_sres = copy->nr;
        req->is_write = copy->cdft == 3;
        sres = req->sres;
        req->sdaddr = data;
    }

    // dma read sre
    if (dma_write_prp(n, (void*)sres, sizeof(NvmeCopyFormat) * copy->nr, prp1, prp2)) {
        femu_err("memory copy: dma_write_prp error\n");
        return NVME_DNR;
    }

    if (copy->cdft == 2 || copy->cdft == 3) {
        // nvm<->fdm, process in ftl thread
        femu_debug("memory copy: len %lu, nlb %lu, saddr %lu\n", len, len / 512, sdaddr);
        int rc = femu_ring_enqueue(n->to_ftl[req->poller], (void *)&req, 1);
        if (rc != 1) {
            femu_err("memory_copy enqueue failed, ret=%d\n", rc);
        }
        return 0;
    }

    uint64_t copyed = 0;
    for (int i = 0; i < copy->nr; i++) {
        NvmeNamespace *sns = NULL;
        NvmeCopyFormat4 *sre = &sres[i].cf4;
        uint64_t nbyte = le32_to_cpu(sre->nbyte);
        uint64_t off = le64_to_cpu(sre->saddr);
        uint32_t snsid = le32_to_cpu(sre->snsid);
        if (snsid != 2) {
            femu_err("memory_copy: fdm->fdm snsid != 2!\n");
            return NVME_INVALID_NSID;
        }
        sns = nvme_find_namespace(n, snsid);
        femu_debug("memory_copy fdm->fdm: soff %lu, doff %lu, nbyte %lu, stime %zu\n", off, sdaddr, nbyte, req->stat.stime);
        backend_rw_internal(sns->backend, data, off, nbyte, 1);
        data += nbyte;
        copyed += nbyte;
    }

    if (copyed != len) {
        femu_err("copyed %lu, len %lu\n", copyed, len);
        abort();
    }

    // standard hasn't defined the result. use copyed bytes as result
    req->cqe.n.result = copyed;
    req->stat.opcode = 0x21;
    req->stat.pcie_lat = 1;
    req->stat.data_size = copyed;
    req->stat.reqlat += copyed / 4;
    req->stat.expire_time += copyed / 4;
    req->status = NVME_SUCCESS;
    return 1;
}

static int memory_io_cmd(NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_MEMORY_FILL:
        req->is_write = 1;
        req->status = memory_fill(ns, cmd, req);
        break;
    case NVME_CMD_MEMORY_COPY:
        req->is_write = 1;
        return memory_copy(ns, cmd, req);
    case NVME_CMD_MEMORY_READ:
        req->is_write = 0;
        req->status = memory_read(ns, cmd, req);
        break;
    case NVME_CMD_MEMORY_WRITE:
        req->is_write = 1;
        req->status = memory_write(ns, cmd, req);
        break;
    default:
        femu_err("memory_io_cmd: opcode %u not supported!\n", cmd->opcode);
        req->status = NVME_INVALID_OPCODE;
        return 1;
    }
    // TODO: io command should be async
    return 1;
}

static void memory_init(NvmeNamespace *ns, Error **err)
{
    MemoryNamespace *mns = malloc(sizeof(MemoryNamespace));
    ns->private = mns;
    // nvme_init_slm(n, ns);
    init_backend(&ns->backend, FEMU_DRAM_BACKEND, NULL, ns->size);
}

CommandSetOps memory_command_set_ops = {
    .state            = NULL,
    .init             = memory_init,
    .exit             = NULL,
    .rw_check_req     = NULL,
    .admin_cmd        = NULL,
    .io_cmd           = memory_io_cmd,
    .get_log          = NULL,
};

nvme_register(NVME_CSI_MEMORY, &memory_command_set_ops);
