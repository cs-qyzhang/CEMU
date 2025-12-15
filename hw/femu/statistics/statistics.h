#ifndef __FEMU_STATISTICS_H
#define __FEMU_STATISTICS_H

#include <assert.h>
#include <stdint.h>

// /* START */ and /* END */ are used in python to extract definitions

/* START */
#define FEMU_STAT_SHM_NAME "FEMU-STAT-SHM"
#define FEMU_STAT_CAPACITY 65536

/**
 * exec time: RequestStat.reqlat - ExecStat.queueing_time
 * context switch time: RequestStat.reqlat - ExecStat.queueing_time - ExecStat.runtime
 */
typedef struct ExecStat {
    uint32_t runtime;           // cpu time
    uint32_t queueing_time;     // queueing time before execution
    uint32_t csf_id      : 8;
    uint32_t in_afdm_id  : 12;
    uint32_t out_afdm_id : 12;
    uint16_t nr_context_switch;
    uint8_t  thread;            // logical CSE core id
    uint8_t  prerun : 1;
    uint8_t  jit    : 1;
    uint8_t  _pad   : 6;
} ExecStat;

typedef struct AfdmStat {
    uint32_t afdm_id;
    uint32_t offset;
} AfdmStat;

typedef struct RWStat {
    uint32_t pcie_queueing_lat;
    uint32_t nand_lat;
    uint32_t slba;
    uint32_t nlb;
} RWStat;

// sizeof(RequestStat) == 64, a cacheline
typedef struct RequestStat {
    uint64_t expire_time;
    union {
        uint64_t stime;         // start time
        struct {
            uint32_t src_drive; // p2p src drive id
            uint32_t dst_drive; // p2p dst drive id
        };
    };
    uint64_t etime;         // end time
    uint32_t data_size;
    uint32_t reqlat;
    uint32_t pcie_lat;
    uint32_t opcode : 8;
    uint32_t result : 24;
    struct {
        union {
            RWStat   rw;
            ExecStat exec;
        };
        AfdmStat afdm;
    };
} RequestStat;

typedef struct CSECoreStat {
    uint64_t exec_time;
    uint64_t idle_time;
} CSECoreStat;

typedef struct FemuStat {
    uint64_t tail;
    uint64_t alive;     // is FEMU running?
    uint64_t capacity;
    uint64_t start_time;
    uint64_t end_time;
    uint64_t max_processed;
    uint64_t _pad[2];   // cacheline align
    CSECoreStat cse_core_stat[128];
    RequestStat req_stat[FEMU_STAT_CAPACITY];
} FemuStat;

typedef struct ComputeJobStat {
    uint64_t start_time;
    uint64_t exec_time;     // UINT64_MAX before functional simulation finished
    uint64_t finish_time;
    int nr_context_switch;
} ComputeJobStat;

typedef struct ComputeUnitStat {
    uint32_t jobs_finished;
    uint32_t jobs_total;
    uint64_t start_time;
    uint64_t exec_time;
    uint64_t idle_time;
    uint64_t nr_context_switch;
    uint64_t context_switch_time;
} ComputeUnitStat;
/* END */

static_assert(sizeof(RequestStat) == 64, "sizeof(RequestStat) != 64");

typedef struct NvmeRequest NvmeRequest;
typedef struct FemuCtrl FemuCtrl;

void femu_stat_add_req(FemuCtrl *n, NvmeRequest *req);
void femu_stat_init(FemuCtrl *n);
void femu_stat_exit(FemuCtrl *n);

#endif // __FEMU_STATISTICS_H