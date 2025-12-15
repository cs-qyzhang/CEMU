#ifndef __FEMU_COMPUTE_H
#define __FEMU_COMPUTE_H

#include <assert.h>
#include <pthread.h>
#include <ubpf.h>
#include "hw/femu/nvme.h"
#include "hw/femu/inc/slab.h"

#define MAX_INDIRECT_JOBS   16

typedef struct MemoryRange {
    uint32_t nsid;      // namespace id
    uint32_t len;       // length
    uint64_t sb;        // starting bytes
    void *addr;
} MemoryRange;

typedef struct MemoryRangeSet {
    MemoryRange *mr;
    uint16_t rsid;
    uint16_t numr;
    void **mr_addr;
    long long *mr_len;
    int in_use;
    pthread_spinlock_t lock;
} MemoryRangeSet;

enum ProgramState {
    PROGRAM_STATE_INVALID,
    PROGRAM_STATE_LOADING,
    PROGRAM_STATE_LOADED,
    PROGRAM_STATE_ACTIVATED,
};

enum ProgramType {
    PROGRAM_TYPE_PHANTOM,
    PROGRAM_TYPE_EBPF,
    PROGRAM_TYPE_SHARED_LIB,
    PROGRAM_TYPE_BITSTREAM,
    PROGRAM_TYPE_INVALID,
};

/**** CSF Metadata ****/
typedef struct CsfRuntime {
    uint64_t avg_runtime;
    uint64_t max_runtime;
    uint64_t min_runtime;
    uint64_t total_runtime;
    uint64_t count;
} CsfRuntime;

// Runtime statistics for host
typedef struct CsfRuntimeStat {
    CsfRuntime host;
} CsfRuntimeStat;

typedef struct Program {
    uint64_t pid;               // program unique identifier
    uint32_t nsid;
    uint16_t pind;              // program index
    uint8_t state;              // enum ProgramState
    uint8_t type;               // enum ProgramType
    void *code;                 // program data
    uint32_t size;              // program size
    uint32_t load_size;         // bytes loaded
    bool is_indirect;           // direct or indirect
    unsigned int jobs_running;  // atomic ref count
    pthread_mutex_t lock;

    uint64_t runtime;
    double runtime_scale;
    CsfRuntimeStat stat;

    union {
        struct {
            ubpf_jit_fn jit_fn;     // jit-ed bpf, NULL if not jit
            struct ubpf_vm *vm;
        } ebpf;

        struct {
            ubpf_jit_fn jit_fn;
            void *so_handle;
        } shared_lib;
    };
} Program;

typedef struct ComputeParams {
    uint16_t csf_runtime_scale;
    uint8_t  nr_cu;                 // number of compute unit
    uint8_t  nr_thread;             // number of functional simulation thread
    CsfSchedulerOption csf_sched_option;
} ComputeParams;

typedef struct ComputeNamespace {
    Slab programs;
    Slab mrs;                                   // memory range set
    NvmeNamespace *ns;
    ComputeParams *params;
    struct rte_ring **to_csd;
    struct ComputeEngine *ce;
    QemuThread *compute_threads;
    QemuThread *sched_thread;
} ComputeNamespace;

static inline Slab *program_slab(NvmeNamespace *ns)
{
    return &((ComputeNamespace *)ns->private)->programs;
}

static inline Slab *memory_range_set_slab(NvmeNamespace *ns)
{
    return &((ComputeNamespace *)ns->private)->mrs;
}

static inline Program *program_get(NvmeNamespace *ns, uint16_t pind)
{
    return (Program *)slab_at(program_slab(ns), pind - 1);
}

static inline MemoryRangeSet *memory_range_set_get(NvmeNamespace *ns, uint16_t rsid)
{
    return (MemoryRangeSet *)slab_at(memory_range_set_slab(ns), rsid - 1);
}

#endif // __FEMU_COMPUTE_H