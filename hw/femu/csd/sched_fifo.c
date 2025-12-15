/**
 * FIFO CSF scheduler
 */
#include <assert.h>
#include "./sched.h"
#include "hw/femu/nvme.h"
#include "qemu/queue.h"

struct entry {
    ComputeJob *job;
    QSIMPLEQ_ENTRY(entry) next;
};

QSIMPLEQ_HEAD(fifo, entry);
QSIMPLEQ_HEAD(freelist, entry);

struct job_queue {
    struct fifo fifo;
    struct freelist freelist;
};

static void *fifo_sched_cu_init(ComputeUnit *cu)
{
    struct job_queue *q = malloc(sizeof(struct job_queue));
    QSIMPLEQ_INIT(&q->fifo);
    QSIMPLEQ_INIT(&q->freelist);
    return q;
}

static void *fifo_sched_init(ComputeEngine *ce)
{
    for (int i = 0; i < ce->nr_cu; i++) {
        ComputeUnit *cu = &ce->cu[i];
        cu->sched_info = fifo_sched_cu_init(cu);
    }
    return NULL;
}

static ComputeJob *fifo_sched_pick_next_job(ComputeUnit *cu, ComputeJob *last_job,
                                            uint64_t *time_slice,
                                            uint64_t *context_switch_time,
                                            void *private)
{
    struct job_queue *q = (struct job_queue *)cu->sched_info;

    if (last_job != NULL && !job_has_finished(last_job)) {
        // resume last job
        *context_switch_time = 0;
        *time_slice = *time_slice;
        return last_job;
    }
    if (last_job != NULL)
        femu_debug("sched_pick_job last_job finished\n");

    // use default context switch time
    *context_switch_time = *context_switch_time;
    // use default time slice
    *time_slice = *time_slice;

    while (1) {
        struct entry *e = QSIMPLEQ_FIRST(&q->fifo);
        if (e == NULL) {
            return NULL;
        }
        QSIMPLEQ_REMOVE_HEAD(&q->fifo, next);
        if (e->job->stat.start_time) {
            // job is running on other cu, remove it
            QSIMPLEQ_INSERT_TAIL(&q->freelist, e, next);
        } else {
            return e->job;
        }
    }
}

static int fifo_sched_enqueue_job(ComputeUnit *cu, ComputeJob *job, void *private)
{
    assert(job->stat.start_time == 0);
    struct job_queue *q = (struct job_queue *)cu->sched_info;
    struct entry *e;
    if (QSIMPLEQ_EMPTY(&q->freelist)) {
        e = malloc(sizeof(struct entry));
    } else {
        e = QSIMPLEQ_FIRST(&q->freelist);
        QSIMPLEQ_REMOVE_HEAD(&q->freelist, next);
    }
    e->job = job;
    QSIMPLEQ_INSERT_TAIL(&q->fifo, e, next);
    return 0;
}

static void fifo_sched_switched_from(ComputeUnit *cu, ComputeJob *job, void *private)
{
    // femu_debug("fifo sched job preempt out\n");
}

static void fifo_sched_dequeue_job(ComputeUnit *cu, ComputeJob *job, void *private)
{
    // femu_debug("fifo sched job finished\n");
}

CsfScheduler fifo_scheduler = {
    .init           = fifo_sched_init,
    .pick_next_job  = fifo_sched_pick_next_job,
    .switched_from  = fifo_sched_switched_from,
    .enqueue_job    = fifo_sched_enqueue_job,
    .dequeue_job    = fifo_sched_dequeue_job,
};