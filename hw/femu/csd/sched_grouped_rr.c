/**
 * GROUP-RR CSF scheduler
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

#define NR_INTERVAL 10

struct group {
    struct elem *head;
    struct elem *last_job_in_group;
    // uint64_t nr_jobs;
    int64_t last_arrive_time;
    int64_t time_slice;
    int32_t prio;
    int64_t sum_interval;
    int32_t nearest_arrive_interval[NR_INTERVAL];
    int tail;
};

#define MAX_GROUPS  8

struct job_queue {
    struct group groups[MAX_GROUPS];
    struct elem *freelist;
    uint64_t context_switch_time;
};

static void *grouped_rr_sched_cu_init(ComputeUnit *cu, CsfSchedulerOption *opt) {
    femu_log("grouped rr init\n");
    struct job_queue *q = calloc(1, sizeof(struct job_queue));
    for (int i = 0; i < MAX_GROUPS; i++) {
        struct group *group = &q->groups[i];
        group->time_slice = opt->time_slice;
        for (int i = 0; i < NR_INTERVAL; i++)
            group->nearest_arrive_interval[i] = 0x400000;
        group->sum_interval = 0x400000 * NR_INTERVAL;
    }
    q->context_switch_time = opt->context_switch_time;

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

static void *grouped_rr_sched_init(ComputeEngine *ce) {
    ComputeUnit *cu = &ce->cu[0];
    cu->sched_info = grouped_rr_sched_cu_init(cu, &ce->csf_sched_opt);
    for (int i = 1; i < ce->nr_cu; i++) {
        cu = &ce->cu[i];
        cu->sched_info = ce->cu[0].sched_info;
    }
    return NULL;
}

static ComputeJob *grouped_rr_sched_pick_next_job(ComputeUnit *cu, ComputeJob *last_job,
                                            uint64_t *time_slice,
                                            uint64_t *context_switch_time,
                                            void *private) {
    struct job_queue *q = (struct job_queue *)cu->sched_info;

    (void)context_switch_time;
    (void)time_slice;

    int last_group = -1;
    struct group *group;
    if (last_job) {
        last_group = last_job->group->id;
        group = &q->groups[last_group];
        assert(group->head != NULL);
        group->last_job_in_group = last_job->sched_info;
        group->last_job_in_group->running = false;
    }
    int group_id = (last_group + 1) % MAX_GROUPS;

    for (int i = 0; i < MAX_GROUPS; i++, group_id = ((group_id + 1) % MAX_GROUPS)) {
        group = &q->groups[group_id];
        struct elem *e = group->head;
        if (e == NULL)
            continue;

        struct elem *start = group->last_job_in_group;
        e = start;
        do {
            e = e->next;
        } while (e != start || e->running);

        if (last_job == e->job)
            *context_switch_time = 0;
        e->running = true;
        *time_slice = group->time_slice;
        // femu_log("group %d time slice %lu\n", group_id, group->time_slice);
        return e->job;
    }
    return NULL;
}

// enqueue job will called for every cu
static int grouped_rr_sched_enqueue_job(ComputeUnit *cu, ComputeJob *job, void *private) {
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
    int group_id = job->group->id;
    struct group * group= &q->groups[group_id];
    if (group->head == NULL) {
        e->next = e;
        e->prev = e;
        group->head = e;
        group->last_job_in_group = e;
    } else {
        struct elem *head = group->head;
        e->next = head;
        e->prev = head->prev;
        head->prev->next = e;
        head->prev = e;
    }
    if (group_id != 0) {
        NvmeProgramExecuteCmd *cmd = (NvmeProgramExecuteCmd *)&job->req->cmd;
        group->prio = cmd->chunk_nlb * 10;

        uint64_t now = clock_ns();
        int64_t interval = MIN(0x1000000ULL, now - group->last_arrive_time);
        group->last_arrive_time = now;
        group->sum_interval += interval - group->nearest_arrive_interval[group->tail];
        group->nearest_arrive_interval[group->tail] = interval;
        group->tail = (group->tail + 1) % NR_INTERVAL;
        uint64_t avg_interval = group->sum_interval / NR_INTERVAL;
        avg_interval = MIN(MAX(avg_interval, 0x10000ULL), 0x1000000ULL);
        // group->time_slice = ((double)0x400000000ULL / (double)avg_interval);// + 10000ULL;
        group->time_slice = MAX(1000,group->prio - ((double)0x80000000ULL / (double)avg_interval));// + 10000ULL;
        femu_debug("group %d time slice %lu\n", group_id, group->time_slice);
    }
    return 0;
}

static void grouped_rr_sched_switched_from(ComputeUnit *cu, ComputeJob *job, void *private) {
    // femu_debug("rr sched job preempt out\n");
    struct elem *e = job->sched_info;
    e->running = false;
}

// dequeue job will called for every cu
static void grouped_rr_sched_dequeue_job(ComputeUnit *cu, ComputeJob *job, void *private) {
    struct job_queue *q = (struct job_queue *)cu->sched_info;
    struct elem *e = job->sched_info;
    if (e == NULL) {
        // job has dequeued in other cu
        assert(0);
        return;
    }
    assert(job_has_finished(e->job));
    int group_id = job->group->id;
    struct group *group = &q->groups[group_id];
    if (e->next == e) {
        group->head = NULL;
        group->last_job_in_group = NULL;
    } else {
        e->next->prev = e->prev;
        e->prev->next = e->next;
        if (group->last_job_in_group == e)
            group->last_job_in_group = e->next;
        if (group->head == e)
            group->head = e->next;
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

CsfScheduler grouped_rr_scheduler = {
    .init = grouped_rr_sched_init,
    .pick_next_job = grouped_rr_sched_pick_next_job,
    .switched_from = grouped_rr_sched_switched_from,
    .enqueue_job = grouped_rr_sched_enqueue_job,
    .dequeue_job = grouped_rr_sched_dequeue_job,
};