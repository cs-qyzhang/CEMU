/**
 * Grouped FIFO CSF scheduler
 */
#include "./sched.h"
#include "hw/femu/nvme.h"
#include "qemu/queue.h"
#include <assert.h>

typedef struct q_entry_t {
  ComputeJob *job;
  QSIMPLEQ_ENTRY(q_entry_t) next;
} q_entry_t;

typedef QSIMPLEQ_HEAD(, q_entry_t) queue_t;

typedef struct qq_entry_t {
  uint32_t group_id;
  queue_t q;
  QLIST_ENTRY(qq_entry_t) next;
} qq_entry_t;

typedef QLIST_HEAD(, qq_entry_t) q_queue_t;

struct job_queue {
  bool take_care_of_group_prio;
  q_queue_t fifos;
  queue_t freelist;
  qq_entry_t *last_sched_q;
};

static queue_t *get_fifo_by_id(struct job_queue *q, uint32_t id) {
  qq_entry_t *fifo_handle = NULL;
  QLIST_FOREACH(fifo_handle, &q->fifos, next) {
    if (fifo_handle->group_id == id) {
      return &fifo_handle->q;
    }
  }
  return NULL;
}

static qq_entry_t *create_fifo_for_id(struct job_queue *q, uint32_t id) {
  qq_entry_t *new_fifo_handle = NULL;
  new_fifo_handle = calloc(1, sizeof(qq_entry_t));
  QSIMPLEQ_INIT(&new_fifo_handle->q);
  new_fifo_handle->group_id = id;

  if (QLIST_EMPTY(&q->fifos)) {
    QLIST_INSERT_HEAD(&q->fifos, new_fifo_handle, next);
  } else {
    bool inserted = false;
    qq_entry_t *insert_handle = NULL;
    QLIST_FOREACH(insert_handle, &q->fifos, next) {
      if (insert_handle->group_id > id) {
        QLIST_INSERT_BEFORE(insert_handle, new_fifo_handle, next);
        inserted = true;
        break;
      }
    }
    if (!inserted) {
      insert_handle = QLIST_FIRST(&q->fifos);
      QLIST_INSERT_BEFORE(insert_handle, new_fifo_handle, next);
    }
  }
  return new_fifo_handle;
}

static void append_job_to_group_fifo(struct job_queue *q, ComputeJob *job) {
  queue_t *fifo = get_fifo_by_id(q, job->group->id);
  if (fifo == NULL) {
    // ok we shall create a q for this group;
    fifo = &create_fifo_for_id(q, job->group->id)->q;
  }
  struct q_entry_t *e;
  if (QSIMPLEQ_EMPTY(&q->freelist)) {
    e = malloc(sizeof(struct q_entry_t));
  } else {
    e = QSIMPLEQ_FIRST(&q->freelist);
    QSIMPLEQ_REMOVE_HEAD(&q->freelist, next);
  }
  e->job = job;
  QSIMPLEQ_INSERT_TAIL(fifo, e, next);
}

static struct job_queue *grouped_fifo_sched_cu_init(ComputeUnit *cu) {
  struct job_queue *q = malloc(sizeof(struct job_queue));
  QLIST_INIT(&q->fifos);
  q->last_sched_q = create_fifo_for_id(q, 0);
  QSIMPLEQ_INIT(&q->freelist);
  return q;
}

static void *grouped_fifo_sched_init(ComputeEngine *ce) {
  for (int i = 0; i < ce->nr_cu; i++) {
    ComputeUnit *cu = &ce->cu[i];
    struct job_queue *q = grouped_fifo_sched_cu_init(cu);
    q->take_care_of_group_prio = ce->csf_sched_opt.take_care_of_group_prio;
    cu->sched_info = q;
  }
  return NULL;
}

static ComputeJob *
grouped_fifo_sched_pick_next_job(ComputeUnit *cu, ComputeJob *last_job,
                                 uint64_t *time_slice,
                                 uint64_t *context_switch_time, void *private) {
  struct job_queue *q = (struct job_queue *)cu->sched_info;

  if (last_job != NULL && !job_has_finished(last_job)) {
    // resume last job
    *context_switch_time = 0;
    *time_slice = *time_slice;
    return last_job;
  }
  if (last_job != NULL) {
    femu_debug("sched_pick_job last_job finished\n");
  }

  // use default context switch time
  *context_switch_time = *context_switch_time;

  qq_entry_t *fifo_e = QLIST_NEXT(q->last_sched_q, next);
  if (fifo_e == NULL) {
    fifo_e = QLIST_FIRST(&q->fifos);
  }
  q->last_sched_q = fifo_e;

  queue_t *fifo = &fifo_e->q;
  while (1) {
    struct q_entry_t *e = QSIMPLEQ_FIRST(fifo);
    if (e == NULL) {
      return NULL;
    }
    QSIMPLEQ_REMOVE_HEAD(fifo, next);
    if (e->job->stat.start_time) {
      // job is running on other cu, remove it
      QSIMPLEQ_INSERT_TAIL(&q->freelist, e, next);
    } else {
      QSIMPLEQ_INSERT_TAIL(&q->freelist, e, next);
      if (q->take_care_of_group_prio) {
        *time_slice = cu->time_slice * e->job->group->prio;
      }
      return e->job;
    }
  }

  return NULL;
}

static int grouped_fifo_sched_enqueue_job(ComputeUnit *cu, ComputeJob *job,
                                          void *private) {
  assert(job->stat.start_time == 0);
  struct job_queue *q = (struct job_queue *)cu->sched_info;
  append_job_to_group_fifo(q, job);
  return 0;
}

static void grouped_fifo_sched_switched_from(ComputeUnit *cu, ComputeJob *job,
                                             void *private) {
  // femu_debug("fifo sched job preempt out\n");
}

static void grouped_fifo_sched_dequeue_job(ComputeUnit *cu, ComputeJob *job,
                                           void *private) {
  // femu_debug("fifo sched job %p with group %d finished\n", job,
  // job->group->id);
}

CsfScheduler grouped_fifo_scheduler = {
    .init = grouped_fifo_sched_init,
    .pick_next_job = grouped_fifo_sched_pick_next_job,
    .switched_from = grouped_fifo_sched_switched_from,
    .enqueue_job = grouped_fifo_sched_enqueue_job,
    .dequeue_job = grouped_fifo_sched_dequeue_job,
};