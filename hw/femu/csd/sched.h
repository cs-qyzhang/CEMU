#ifndef __FEMU_CSD_SCHEDULE_H
#define __FEMU_CSD_SCHEDULE_H

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <ubpf.h>
#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "hw/femu/inc/slab.h"
#include "../statistics/statistics.h"
#include "../nvme.h"
#include "compute.h"

#define CSD_THREAD_COUNT    4
#define CSE_CORE_COUNT      4
#define MAX_STAGED_JOBS     1024
#define MAX_JOB_GROUPS      1024

typedef struct CsfScheduleInfo {
    int core_count;
    uint64_t core_last_avail_time[CSE_CORE_COUNT];
    uint64_t core_expect_finish_time[CSE_CORE_COUNT];
    uint8_t core_in_use[CSE_CORE_COUNT];
} CsfScheduleInfo;

struct NvmeRequest;
struct JobGroup;
struct csd;

typedef QSIMPLEQ_HEAD(JobList, ComputeJob) JobList;

// A job is a exec request from a nvme request
typedef struct ComputeJob {
    ubpf_jit_fn jit_fn;         // jit-ed bpf, NULL if not jit
    void *in;
    void *out;
    uint64_t in_size;
    uint64_t out_size;
    int csf_id;
    struct NvmeRequest *req;

    pthread_spinlock_t lock;    // protect stat.exec_time
    ComputeJobStat stat;
    uint64_t last_sched_time;
    uint32_t runtime;
    uint32_t user_runtime;      // user specified runtime

    struct JobGroup *group;
    QSIMPLEQ_ENTRY(ComputeJob) group_list;
    uint64_t permited_cu_mask;  // 0 means all cu are permited
    int nr_permited_cu;
    int running_cu;             // -1 for not running
    void *sched_info;           // private data used by scheduler

    struct Program *program;
    struct ComputeEngine *ce;

    struct ubpf_jit_args args;
} ComputeJob;

static inline int job_permited_on_cu(ComputeJob *job, uint8_t cu_id)
{
    assert(cu_id <= 63);
    return (job->permited_cu_mask == 0) || (job->permited_cu_mask & (1ULL << cu_id));
}

typedef struct WokerThreadPool {
    int thread_count;
    pthread_t threads[CSD_THREAD_COUNT];
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    // WokerJob jobs_ring[JOBS_RING_SIZE];
    int head;
    int tail;
} WokerThreadPool;

/**
 * group 0 is the default group, which is used to schedule jobs without group
 */
#define MAX_JOB_GROUP_PRIO     9
#define MIN_JOB_GROUP_PRIO     1
#define DEFAULT_JOB_GROUP_PRIO 5

typedef struct JobGroup {
    uint64_t start_time;
    uint64_t total_runtime;     // total runtime of all jobs in this group
    uint64_t active_runtime;    // runtime of active (running) jobs
    JobList jobs;

    // QoS related
    int prio;                   // between 1 and 9
    uint32_t bandwidth;         // bandwidth in KB/s
    uint32_t deadline;          // response time in us

    int id;
    int jobs_total;
    int jobs_finished;
    int jobs_running;
    int jobs_queueing;
    pthread_spinlock_t lock;
} JobGroup;

typedef enum ComputeUnitType {
    COMPUTE_UNIT_TYPE_GENERAL,
    COMPUTE_UNIT_TYPE_INVALID,
} ComputeUnitType;

/**
 * A compute unit is a logical compute unit, e.g., a general purpose CPU core.
 */
typedef struct ComputeUnit {
    int type;                       // enum ComputeUnitType
    int id;
    uint64_t next_sched_time;
    struct CsfScheduler *scheduler;
    ComputeJob *job_running;
    void *sched_info;               // private data used by scheduler
    size_t pqueue_pos;              // used in priority queue
    uint64_t time_slice;            // default time slice in ns
    uint64_t context_switch_time;   // default context switch time in ns
    ComputeUnitStat stat;
} ComputeUnit;

/**
 * logical compute units (CSE)
 */
typedef struct ComputeEngine {
    ComputeUnit *cu;
    FemuCtrl *ctrl;
    ComputeNamespace *cns;
    int nr_cu;

    struct CsfScheduler *scheduler;
    CsfSchedulerOption csf_sched_opt;

    // arrived jobs are staged in this ring before pushed to cu's job queue
    struct {
        pthread_spinlock_t lock;
        ComputeJob *jobs[MAX_STAGED_JOBS];
        int head;
        int tail;
    } staged_jobs;

    // job
    Slab job_slab;
    int nr_active_job;

    // job group
    int nr_active_group;
    JobGroup *job_group[MAX_JOB_GROUPS];
    Slab job_group_slab;
    pthread_spinlock_t job_group_lock;

    // functional simulation threads
    int nr_thread;

    // next poller to send cqe
    int next_poller;

    // default scheduler's pick_thread() implementation
    int next_thread;
} ComputeEngine;


/**
 * Scheduler interface
 *
 * A scheduler needs to implement this interface.
 *
 * There is only one scheduler instance per CSD drive.
 *
 * sched_info of ComputeJob/ComputeUnit etc can be used by scheduler
 * to store scheduler's private data. It is initialized to NULL when
 * a job is created.
 */
typedef struct CsfScheduler {
    /**
     * @brief (optional) Scheduler initialization
     *
     * @return private data pointer used by scheduler
     */
    void *(*init)(ComputeEngine *ce);

    /**
     * @brief (optional) Scheduler finalization
     *
     * @return private data pointer
     */
    void (*fini)(void *private);

    /**
     * @brief Pick next job on a cu
     *
     * @param[in] last_job last job executed on this cu, NULL if no job executed
     * @param[inout] time_slice next job's time_slice in ns
     * @param[inout] context_switch_time context switch time in ns
     * @param private private data pointer returned by init()
     * @return next job, NULL if no job is available
     */
    ComputeJob *(*pick_next_job)(ComputeUnit *cu, ComputeJob *last_job,
                                 uint64_t *time_slice,
                                 uint64_t *context_switch_time, void *private);

    /**
     * @brief Enqueue a job to cu
     *
     * A job is enqueued multiple times for every cu allowed.
     * Per-job init can be done here.
     *
     * @return 0 on success, -1 means refused to add this job to cu,
     *         1 means success and the cu needs reschedule
     */
    int (*enqueue_job)(ComputeUnit *cu, ComputeJob *job, void *private);

    /**
     * @brief (optional) Called when a job is finished running
     */
    void (*dequeue_job)(ComputeUnit* cu, ComputeJob *job, void *private);

    /**
     * @brief (optional) Called when a job is preempted out
     *        switched_in is merged to pick_next_job()
     */
    void (*switched_from)(ComputeUnit *cu, ComputeJob *job, void *private);

    /**
     * @brief (optional) Pick a thread to run functional simulation
     * @return thread id, [0, ce->nr_thread-1]
     */
    int (*pick_thread)(ComputeEngine *ce, ComputeJob *job, void *private);

    void *private;
} CsfScheduler;


/**
 * List of scheduler implementations
 */
extern CsfScheduler fifo_scheduler;
extern CsfScheduler rr_scheduler;
extern CsfScheduler grouped_fifo_scheduler;
extern CsfScheduler grouped_rr_scheduler;

/**
 * job helper function
 */
static inline uint64_t job_exec_time(ComputeJob *job)
{
    pthread_spin_lock(&job->lock);
    uint64_t exec_time = job->stat.exec_time;
    pthread_spin_unlock(&job->lock);
    return exec_time;
}

static inline int job_has_finished(ComputeJob *job)
{
    return job->runtime >= job_exec_time(job);
}

static inline int job_is_queueing(ComputeJob *job)
{
    return job->stat.start_time == 0;
}


/**
 * job group helper function
 */
static inline JobGroup *job_group_get(ComputeEngine *ce, int id)
{
    return ce->job_group[id];
}


/**
 * compute unit mask
 */
static inline uint64_t mask_cu(uint64_t mask, uint8_t id)
{
    return mask | (1ULL << id);
}

static inline uint64_t unmask_cu(uint64_t mask, uint8_t id)
{
    return mask & ~(1ULL << id);
}

static inline int cu_is_masked(uint64_t mask, uint8_t id)
{
    return mask & (1ULL << id);
}

static inline ComputeUnit *get_cu(ComputeEngine *ce, int id)
{
    return &ce->cu[id];
}

static inline void set_sched_runtime(ComputeJob *job, uint64_t runtime)
{
    pthread_spin_lock(&job->lock);
    job->stat.exec_time = runtime;
    // req->stat.exec.csf_id = csf_id;
    pthread_spin_unlock(&job->lock);
}

/* Indirect usage model */
static inline bool job_is_indirect(ComputeJob *job)
{
    return job->program->is_indirect;
}

/**
 * functions exported and called in other module/threads
 */
typedef struct ComputeNamespace ComputeNamespace;
void sched_init(NvmeNamespace *ns);
void sched_alloc_job(ComputeNamespace *ns, NvmeRequest *req);
void sched_enqueue_job(ComputeNamespace *ns, NvmeRequest *req);
void sched_enqueue_indirect(ComputeNamespace *cns, ComputeJob *job);
void sched_job_finish_indirect(ComputeJob *job);
int  sched_job_group_alloc(ComputeNamespace *nns);
int  sched_job_group_free(ComputeNamespace *nns, int id);
int  sched_job_group_set_qos(ComputeNamespace *nns, int id, int prio,
                             uint32_t bandwidth, uint32_t deadline);

#endif // __FEMU_CSD_SCHEDULE_H