/**
 * Timing simulation of CSF scheduling
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "hw/femu/inc/rte_ring.h"
#include "hw/femu/inc/pqueue.h"
#include "hw/femu/inc/slab.h"
#include "hw/femu/nvme-def.h"
#include "hw/femu/nvme.h"
#include "hw/femu/statistics/statistics.h"
#include "sched.h"
#include "compute.h"

void *sched_thread(void *arg);

/**
 * priority queue functions
 */
static inline int pqueue_cmppri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return next > curr;
}

static inline pqueue_pri_t pqueue_getpri_cu(void *cu)
{
    return ((ComputeUnit *)cu)->next_sched_time;
}

static inline void pqueue_setpri_cu(void *cu, pqueue_pri_t pri)
{
    ((ComputeUnit *)cu)->next_sched_time = pri;
}

static inline size_t pqueue_getpos_cu(void *cu)
{
    return ((ComputeUnit *)cu)->pqueue_pos;
}

static inline void pqueue_setpos_cu(void *cu, size_t pos)
{
    ((ComputeUnit *)cu)->pqueue_pos = pos;
}

/**
 * job slab
 */
static inline ComputeJob *job_alloc(ComputeEngine *ce)
{
    ComputeJob *job = slab_alloc(&ce->job_slab, 1);
    if (job) {
        memset(job, 0, sizeof(ComputeJob));
        pthread_spin_init(&job->lock, PTHREAD_PROCESS_PRIVATE);
        job->stat.exec_time = UINT64_MAX;
        job->running_cu = -1;
        ce->nr_active_job++;
    }
    return job;
}

static inline void job_free(ComputeEngine *ce, ComputeJob *job)
{
    pthread_spin_lock(&job->lock);
    pthread_spin_destroy(&job->lock);
    ce->nr_active_job--;
    slab_free(&ce->job_slab, job, 1);
}


/**
 * job group
 */
static inline void job_group_add(JobGroup *group, ComputeJob *job)
{
    pthread_spin_lock(&group->lock);
    QSIMPLEQ_INSERT_TAIL(&group->jobs, job, group_list);
    group->jobs_total++;
    group->jobs_queueing++;
    pthread_spin_unlock(&group->lock);
    job->group = group;
}

static inline void job_group_remove(JobGroup *group, ComputeJob *job)
{
    pthread_spin_lock(&group->lock);
    QSIMPLEQ_REMOVE(&group->jobs, job, ComputeJob, group_list);
    group->jobs_total--;
    group->total_runtime += job->runtime;
    group->active_runtime -= job->runtime;
    pthread_spin_unlock(&group->lock);
    job->group = NULL;
}

int sched_job_group_alloc(ComputeNamespace *cns)
{
    ComputeEngine *ce = cns->ce;
    JobGroup *group = slab_alloc(&ce->job_group_slab, 1);
    if (group == NULL)
        return -1;
    int id = group->id;
    memset(group, 0, sizeof(JobGroup));
    pthread_spin_init(&group->lock, PTHREAD_PROCESS_PRIVATE);
    QSIMPLEQ_INIT(&group->jobs);
    group->id = id;
    ce->job_group[id] = group;
    ce->nr_active_group++;
    return id;
}

int sched_job_group_free(ComputeNamespace *cns, int id)
{
    ComputeEngine *ce = cns->ce;
    JobGroup *group = job_group_get(ce, id);
    if (group == NULL)
        return -1;
    if (group->jobs_running + group->jobs_queueing > 0) {
        femu_err("job_group_free: group %d still have jobs running!!! "
                 "queueing jobs %d, running jobs %d, move these jobs to group 0\n",
                 group->id, group->jobs_queueing, group->jobs_running);
        ComputeJob *job = NULL;
        JobGroup *group0 = job_group_get(ce, 0);
        pthread_spin_lock(&group->lock);
        pthread_spin_lock(&group0->lock);
        // move all jobs to group 0
        QSIMPLEQ_FOREACH(job, &group->jobs, group_list) {
            group->jobs_total--;
            QSIMPLEQ_REMOVE(&group->jobs, job, ComputeJob, group_list);
            QSIMPLEQ_INSERT_TAIL(&group0->jobs, job, group_list);
            group0->jobs_total++;
            if (job_is_queueing(job)) {
                group0->jobs_queueing++;
            } else {
                group0->jobs_running++;
            }
            job->group = group0;
        }
        group0->active_runtime += group->active_runtime;
        pthread_spin_unlock(&group0->lock);
    } else {
        pthread_spin_lock(&group->lock);
    }
    pthread_spin_destroy(&group->lock);
    ce->job_group[group->id] = NULL;
    ce->nr_active_group--;
    slab_free(&ce->job_group_slab, group, 1);
    return 0;
}

int sched_job_group_set_qos(ComputeNamespace *cns, int id, int prio, uint32_t bandwidth, uint32_t deadline)
{
    JobGroup *group = job_group_get(cns->ce, id);
    if (group == NULL)
        return -1;
    pthread_spin_lock(&group->lock);
    group->prio = prio;
    group->bandwidth = bandwidth;
    group->deadline = deadline;
    pthread_spin_unlock(&group->lock);
    // TODO: reschedule
    return 0;
}

// job group slab init, give each job group an id
static void job_group_init(void *elem, void *arg)
{
    JobGroup *group = elem;
    int *id = arg;
    group->id = *id;
    *id += 1;
}

void sched_init(NvmeNamespace *ns)
{
    ComputeNamespace *cns = ns->private;
    ComputeParams *param = cns->params;

    ComputeEngine *ce = malloc(sizeof(ComputeEngine));
    memset(ce, 0, sizeof(ComputeEngine));
    assert(ce != NULL);
    cns->ce = ce;
    ce->cns = cns;
    ce->ctrl = ns->ctrl;
    ce->next_poller = 1;
    ce->nr_thread = param->nr_thread;
    ce->csf_sched_opt = param->csf_sched_option;

    CsfScheduler* scheduler = NULL;

    if (param->csf_sched_option.algo == NULL ||
        strcmp(param->csf_sched_option.algo, "fifo") == 0) {
        if (param->csf_sched_option.take_care_of_group) {
            femu_log("use grouped fifo\n");
            scheduler = &grouped_fifo_scheduler;
        } else {
            femu_log("use fifo\n");
            scheduler = &fifo_scheduler;
        }
    } else if (strcmp(param->csf_sched_option.algo, "rr") == 0) {
        if (param->csf_sched_option.take_care_of_group) {
            femu_log("use grouped rr\n");
            scheduler = &grouped_rr_scheduler;
        } else {
            femu_log("use rr\n");
            scheduler = &rr_scheduler;
        }
    } else {
        femu_err("unknown scheduler %p %s\n", param->csf_sched_option.algo, param->csf_sched_option.algo);
        assert(0);
    }
    if (param->csf_sched_option.take_care_of_group_prio) {
        femu_log("sched will take care of group priority\n");
    }

    assert(scheduler->pick_next_job != NULL);
    assert(scheduler->enqueue_job != NULL);
    ce->scheduler = scheduler;

    // init staged job queue
    ce->staged_jobs.head = 0;
    ce->staged_jobs.tail = 0;
    pthread_spin_init(&ce->staged_jobs.lock, PTHREAD_PROCESS_PRIVATE);

    // init slab allocator of job
    pthread_spin_init(&ce->job_slab.lock, PTHREAD_PROCESS_PRIVATE);
    slab_init(&ce->job_slab, sizeof(ComputeJob), MAX_STAGED_JOBS * 2, NULL, NULL);

    // init job group
    int group_id = 0;
    slab_init(&ce->job_group_slab, sizeof(JobGroup), MAX_JOB_GROUPS,
              job_group_init, &group_id);
    pthread_spin_init(&ce->job_group_lock, PTHREAD_PROCESS_PRIVATE);
    // allocate group 0
    int id = sched_job_group_alloc(cns);
    assert(id == 0);
    ce->nr_active_group++;
    sched_job_group_set_qos(cns, id, DEFAULT_JOB_GROUP_PRIO, 0, 0);
    // allocate group 1
    id = sched_job_group_alloc(cns);
    assert(id == 1);
    ce->nr_active_group++;
    sched_job_group_set_qos(cns, id, DEFAULT_JOB_GROUP_PRIO, 0, 0);
    // allocate group 2
    id = sched_job_group_alloc(cns);
    assert(id == 2);
    ce->nr_active_group++;
    sched_job_group_set_qos(cns, id, DEFAULT_JOB_GROUP_PRIO, 0, 0);

    // init compute unit
    ce->nr_cu = param->nr_cu;
    if (ce->nr_cu == 0) {
        femu_err("No compute unit specified!\n");
        abort();
    }
    if (ce->nr_cu > 64) {
        femu_err("Compute unit number %d is larger than 64!\n", ce->nr_cu);
        femu_err("Due to cu_mask in ComputeJob, maximum nr_cu is 64!\n");
        abort();
    }
    ce->cu = malloc(sizeof(ComputeUnit) * ce->nr_cu);
    memset(ce->cu, 0, sizeof(ComputeUnit) * ce->nr_cu);
    for (int i = 0; i < ce->nr_cu; i++) {
        ComputeUnit *cu = &ce->cu[i];
        cu->id = i;
        cu->scheduler = scheduler;
        // default settings
        cu->time_slice = param->csf_sched_option.time_slice;
        cu->context_switch_time = param->csf_sched_option.context_switch_time;
    }

    // init scheduler
    // TODO: init for each cu
    if (scheduler->init)
        scheduler->private = scheduler->init(ce);

    // run sched_thread
    cns->sched_thread = malloc(sizeof(QemuThread));
    qemu_thread_create(cns->sched_thread, "FEMU CSD SCHED THREAD",
                       sched_thread, cns, QEMU_THREAD_JOINABLE);
}

static void job_finish(ComputeEngine *ce, CsfScheduler *scheduler,
                       ComputeJob *job, uint64_t finish_time)
{
    // finish_time = clock_ns();
    FemuCtrl *n = ce->ctrl;
    NvmeRequest *req = job->req;
    JobGroup *group = job->group;
    Program *program = job->program;

    // update statistics
    job->stat.finish_time = finish_time;
    req->stat.reqlat = finish_time - req->stat.stime;
    req->stat.expire_time = finish_time;
    req->stat.exec.queueing_time += job->stat.start_time - req->stat.stime;
    req->stat.exec.nr_context_switch += job->stat.nr_context_switch;
    req->stat.exec.runtime += job->runtime;
    req->cqe.n.rsvd += req->stat.exec.queueing_time;

    // job group statistics
    if (!job_is_indirect(job)) {
        pthread_spin_lock(&group->lock);
        group->jobs_running--;
        group->jobs_finished++;
        pthread_spin_unlock(&group->lock);
    }

    if (scheduler && scheduler->dequeue_job) {
        uint64_t mask = job->permited_cu_mask;
        for (int i = 0; i < ce->nr_cu; i++) {
            if (mask == 0 || cu_is_masked(mask, i)) {
                ComputeUnit *cu = get_cu(ce, i);
                scheduler->dequeue_job(cu, job, scheduler->private);
            }
        }
    }

    if (!job_is_indirect(job)) {
        // send req to nvme cq thread
        int next_poller = ce->next_poller;
        int rc = femu_ring_enqueue(n->to_poller[next_poller], (void *)&req, 1);
        if (rc != 1) {
            femu_err("CSD to_poller enqueue failed\n");
        }
        if (n->multipoller_enabled) {
            next_poller = next_poller + 1;
            if (next_poller > n->nr_pollers)
                next_poller = 1;
            ce->next_poller = next_poller;
        }

        qatomic_dec(&program->jobs_running);
        if (job->args.data_buffer)
            free(job->args.data_buffer);
        job_group_remove(job->group, job);
        job_free(ce, job);
    } else {
        femu_ring_enqueue(req->indirect_task.ring, (void *)&req, 1);
    }
}

void sched_job_finish_indirect(ComputeJob *job)
{
    pthread_spin_lock(&job->group->lock);
    job->group->jobs_running--;
    job->group->jobs_finished++;
    pthread_spin_unlock(&job->group->lock);

    qatomic_dec(&job->program->jobs_running);
    job_group_remove(job->group, job);
    job_free(job->ce, job);
}

// return 0 if success, -1 if failed
static int stage_job(ComputeEngine *ce, ComputeJob *job)
{
    pthread_spin_lock(&ce->staged_jobs.lock);
    int next_tail = (ce->staged_jobs.tail + 1) % MAX_STAGED_JOBS;
    if (next_tail == ce->staged_jobs.head) {
        femu_err("Staged job queue is full!\n");
        uint64_t now = clock_ns();
        job_finish(ce, NULL, job, now);
        return -1;
    }
    ce->staged_jobs.jobs[ce->staged_jobs.tail] = job;
    ce->staged_jobs.tail = next_tail;
    pthread_spin_unlock(&ce->staged_jobs.lock);
    return 0;
}

void sched_alloc_job(ComputeNamespace *cns, NvmeRequest *req)
{
    ComputeEngine *ce = cns->ce;
    ComputeJob *job = job_alloc(ce);
    req->job = job;
    job->req = req;
    job->ce = ce;
    req->stat.exec.queueing_time = 0;
    req->stat.exec.nr_context_switch = 0;
    req->stat.exec.runtime = 0;
    req->cqe.n.rsvd = 0;
}

void sched_enqueue_job(ComputeNamespace *cns, NvmeRequest *req)
{
    // init job and add it to timing simulation thread (sched_thread)
    ComputeEngine *ce = cns->ce;
    CsfScheduler *scheduler = ce->scheduler;
    NvmeProgramExecuteCmd *exec = (NvmeProgramExecuteCmd *)&req->cmd;
    ComputeJob *job = req->job;

    // add job to group
    JobGroup *group = job_group_get(ce, exec->group);
    if (group == NULL) {
        femu_err("new_job_arrived: job group %d doesn't exist!\n", exec->group);
    } else {
        job_group_add(group, job);
    }
    assert(group->id == exec->group);

    // add job to staged queue
    if (!job_is_indirect(job) && stage_job(ce, job)) {
        // job is rejected
        femu_err("sched_add_job error\n");
    }

    // push job to functional simulation
    int csd_thread = 0;
    if (scheduler->pick_thread) {
        csd_thread = scheduler->pick_thread(ce, job, scheduler->private);
    } else {
        csd_thread = ce->next_thread;
        ce->next_thread = (ce->next_thread + 1) % ce->nr_thread;
    }
    req->stat.exec.thread = csd_thread;
}

void sched_enqueue_indirect(ComputeNamespace *cns, ComputeJob *job)
{
    // TODO: job cu mask
    job->stat.exec_time = UINT64_MAX;
    job->stat.start_time = 0;
    job->stat.finish_time = 0;
    job->stat.nr_context_switch = 0;
    job->last_sched_time = 0;
    job->runtime = 0;

    stage_job(job->ce, job);
}

// add newly added job from staged queue to compute unit's job queue
// return mask of cu that needs reschedule (preemption occurred)
static uint64_t enqueue_staged_job(ComputeNamespace *cns, ComputeEngine *ce)
{
    CsfScheduler *scheduler = ce->scheduler;

    int tail = 0;
    int head = 0;
    pthread_spin_lock(&ce->staged_jobs.lock);
    tail = ce->staged_jobs.tail;
    head = ce->staged_jobs.head;
    pthread_spin_unlock(&ce->staged_jobs.lock);

    if (head == tail)
        return 0;

    uint64_t cu_mask = 0;
    int rc = 0;
    int new_jobs = 0;
    while (head != tail) {
        ComputeJob *job = ce->staged_jobs.jobs[head];

        int pushed = 0;
        uint64_t mask = job->permited_cu_mask;
        for (int i = 0; i < ce->nr_cu; i++) {
            if (mask == 0 || cu_is_masked(mask, i)) {
                ComputeUnit *cu = get_cu(ce, i);
                assert(job->sched_info == NULL);
                rc = scheduler->enqueue_job(cu, job, scheduler->private);
                if (rc == 1) {
                    // preemption occurred, cu needs reschedule
                    cu_mask = mask_cu(cu_mask, i);
                    pushed += 1;
                } else {
                    // 0 success, -1 reject
                    pushed += rc + 1;
                }
            }
        }
        if (pushed == 0) {
            // job is rejected
            // TODO status code REJECTED
            job->req->cqe.status = NVME_DNR;
            uint64_t now = clock_ns();
            job_finish(ce, NULL, job, now);
        }
        head = (head + 1) % MAX_STAGED_JOBS;
        new_jobs++;
    }

    pthread_spin_lock(&ce->staged_jobs.lock);
    ce->staged_jobs.head = head;
    pthread_spin_unlock(&ce->staged_jobs.lock);

    return cu_mask;
}

// update statistics when job is preempted out
static inline ComputeJob *job_switched_out(ComputeNamespace *cns, ComputeUnit *cu,
                                           ComputeJob *job, uint64_t now)
{
    if (job == NULL)
        return NULL;

    CsfScheduler *scheduler = cu->scheduler;
    JobGroup *group = job->group;

    // update statistics
    uint64_t runtime = now - job->last_sched_time;
    job->runtime += runtime;
    pthread_spin_lock(&group->lock);
    group->active_runtime += runtime;
    pthread_spin_unlock(&group->lock);

    if (scheduler->switched_from)
        scheduler->switched_from(cu, job, scheduler->private);

    job->running_cu = -1;
    if (job_has_finished(job)) {
        job_finish(cns->ce, scheduler, job, now);
        return NULL;
    }
    return job;
}

// update statistics when job is switched in
static inline void job_switched_in(ComputeUnit* cu, ComputeJob *job, uint64_t now,
                                   uint64_t context_switch_time,
                                   uint64_t time_slice)
{
    job->last_sched_time = now + context_switch_time;
    if (job->stat.start_time == 0)
        job->stat.start_time = now;
    job->running_cu = cu->id;
}

// timing simulation of csf scheduling
void *sched_thread(void *arg)
{
    ComputeNamespace *cns = (ComputeNamespace *)arg;
    NvmeNamespace *ns = cns->ns;
    ComputeEngine *ce = cns->ce;
    CsfScheduler *scheduler = ce->scheduler;
    ComputeJob *last_job = NULL;
    ComputeJob *next_job = NULL;
    ComputeUnit *cu = NULL;

    // compute unit priority queue, sorted by cu->next_sched_time
    pqueue_t *pq = pqueue_init(cns->params->nr_cu, pqueue_cmppri, pqueue_getpri_cu,
            pqueue_setpri_cu, pqueue_getpos_cu, pqueue_setpos_cu);

    while (!ns->ctrl->dataplane_started) {
        usleep(100000);
    }

    femu_log("CSD scheduling thread started\n");

    uint64_t now = clock_ns();
    for (int i = 0; i < ce->nr_cu; i++) {
        cu = get_cu(ce, i);
        cu->next_sched_time = now + i * 100;
        pqueue_insert(pq, cu);
    }

    while (1) {
        // add newly arrived job to compute unit's job queue
        // return mask of cu that needs reschedule (preemption occurred)
        uint64_t cu_mask = enqueue_staged_job(cns, ce);

        // cu is the compute unit with the earliest next_sched_time
        cu = pqueue_peek(pq);
        now = clock_ns();
        if (now >= cu->next_sched_time) {
            cu_mask = mask_cu(cu_mask, cu->id);
        }

        for (int i = 0; i < ce->nr_cu; i++) {
            if (!cu_is_masked(cu_mask, i))
                continue;

            // cu needs reschedule
            cu = get_cu(ce, i);
            now = clock_ns();

            last_job = cu->job_running;
            // return NULL if job is finished
            last_job = job_switched_out(cns, cu, last_job, now);

            while (1) {
                // pick job
                uint64_t context_switch_time = cu->context_switch_time;
                uint64_t time_slice = cu->time_slice;
                next_job = scheduler->pick_next_job(cu, last_job,
                        &time_slice, &context_switch_time, scheduler->private);
                cu->job_running = next_job;

                if (next_job) {
                    // exec time comes from functional simulation
                    uint64_t exec_time = job_exec_time(next_job);
                    if (next_job->runtime >= exec_time) {
                        // job has finished
                        job_finish(ce, scheduler, next_job, now);
                        last_job = NULL;
                        continue;
                    } else if (exec_time < UINT64_MAX) {
                        // functional simulation has finished
                        uint64_t remain = exec_time - next_job->runtime;
                        if (remain <= time_slice)
                            time_slice = MAX(remain, 1000);
                    }

                    if (next_job->stat.start_time == 0) {
                        // job is switched in for the first time
                        JobGroup *group = next_job->group;
                        pthread_spin_lock(&group->lock);
                        group->jobs_queueing--;
                        group->jobs_running++;
                        pthread_spin_unlock(&group->lock);
                        next_job->stat.start_time = now + context_switch_time;
                    }

                    if (next_job != last_job || context_switch_time > 0) {
                        next_job->stat.nr_context_switch++;
                    }

                    job_switched_in(cu, next_job, now, context_switch_time, time_slice);
                } else {
                    // no job to run
                    context_switch_time = 0;
                    time_slice = 1000;
                }
                cu->next_sched_time = now + context_switch_time + time_slice;
                pqueue_change_priority(pq, cu->next_sched_time, cu);
                break;
            }
        }
    }
}
