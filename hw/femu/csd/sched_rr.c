/**
 * RR CSF scheduler
 */
#include <assert.h>
#include "./sched.h"
#include "hw/femu/nvme.h"

struct elem {
    struct elem *prev;
    struct elem *next;
    ComputeJob  *job;
    bool running;
    bool free;
};

struct job_queue {
    struct elem *head;
    struct elem *freelist;
};

static void *rr_sched_cu_init(ComputeUnit *cu) {
    femu_log("rr init\n");
    struct job_queue *q = malloc(sizeof(struct job_queue));
    q->head = NULL;
    struct elem *elems = malloc(sizeof(struct elem) * 256);
    for (int i = 0; i < 256; i++) {
        struct elem *e = &elems[i];
        e->job = NULL;
        e->next = &elems[(i + 1) % 256];
        e->prev = &elems[(i + 255) % 256];
        e->running = false;
        e->free = true;
    }
    q->freelist = elems;
    return q;
}

static void *rr_sched_init(ComputeEngine *ce) {
    ComputeUnit *cu = &ce->cu[0];
    cu->sched_info = rr_sched_cu_init(cu);
    for (int i = 1; i < ce->nr_cu; i++) {
        cu = &ce->cu[i];
        cu->sched_info = ce->cu[0].sched_info;
    }
    return NULL;
}

static ComputeJob *rr_sched_pick_next_job(ComputeUnit *cu, ComputeJob *last_job,
                                            uint64_t *time_slice,
                                            uint64_t *context_switch_time,
                                            void *private) {
    struct job_queue *q = (struct job_queue *)cu->sched_info;

    // use default context switch time
    (void)context_switch_time;
    // use default time slice
    (void)time_slice;

    struct elem *e = q->head;
    if (e == NULL)
        return NULL;

    struct elem *start = e;
    if (last_job) {
        e = last_job->sched_info;
        assert(e->job == last_job && e->free == false);
        assert(e->running == false);
        start = e;
    }
    do
        e = e->next;
    while (e != start || job_has_finished(e->job));

    if (last_job == e->job)
        *context_switch_time = 0;
    if (job_has_finished(e->job)) {
        return NULL;
    } else {
        e->running = true;
        return e->job;
    }
}

// enqueue job will called for every cu
static int rr_sched_enqueue_job(ComputeUnit *cu, ComputeJob *job, void *private) {
    if (job->sched_info != NULL) {
        // job has queued in other cu
        assert(0);
        return 0;
    }
    assert(job->stat.start_time == 0);
    struct job_queue *q = (struct job_queue *)cu->sched_info;
    struct elem *e = q->freelist;
    if (e == NULL) {
        e = malloc(sizeof(struct elem));
    } else if (q->freelist->next == q->freelist) {
        q->freelist = NULL;
    } else {
        q->freelist->next->prev = e->prev;
        q->freelist->prev->next = e->next;
        q->freelist = e->next;
    }
    e->job = job;
    job->sched_info = e;
    e->running = false;
    e->free = false;
    if (q->head == NULL) {
        e->next = e;
        e->prev = e;
        q->head = e;
    } else {
        struct elem *head = q->head;
        e->next = head;
        e->prev = head->prev;
        head->prev->next = e;
        head->prev = e;
    }
    return 0;
}

static void rr_sched_switched_from(ComputeUnit *cu, ComputeJob *job, void *private) {
    // femu_debug("rr sched job preempt out\n");
    struct elem *e = job->sched_info;
    e->running = false;
}

// dequeue job will called for every cu
static void rr_sched_dequeue_job(ComputeUnit *cu, ComputeJob *job, void *private) {
    struct job_queue *q = (struct job_queue *)cu->sched_info;
    struct elem *e = job->sched_info;
    if (e == NULL) {
        // job has dequeued in other cu
        assert(0);
        return;
    }
    assert(e->job == job);
    // assert(e->running == false);
    assert(job_has_finished(e->job));
    if (e->next == e) {
        q->head = NULL;
    } else {
        e->next->prev = e->prev;
        e->prev->next = e->next;
    }
    if (e == q->head) {
        q->head = e->next;
    }

    if (q->freelist == NULL) {
        q->freelist = e;
        e->next = e;
        e->prev = e;
    } else {
        e->next = q->freelist;
        e->prev = q->freelist->prev;
        q->freelist->prev->next = e;
        q->freelist->prev = e;
    }
    e->running = false;
    e->free = true;
    job->sched_info = NULL;
}

CsfScheduler rr_scheduler = {
    .init = rr_sched_init,
    .pick_next_job = rr_sched_pick_next_job,
    .switched_from = rr_sched_switched_from,
    .enqueue_job = rr_sched_enqueue_job,
    .dequeue_job = rr_sched_dequeue_job,
};