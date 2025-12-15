#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "../nvme.h"
#include "../nvme-def.h"
#include "./statistics.h"

// femu_stat is shared between FEMU and analysis.py
// FEMU produces statistics and analysis.py consumes them

pthread_spinlock_t stat_lock;
int total_drives = 0;

void femu_stat_init(FemuCtrl *n)
{
    // shm is created in analysis.py
    char shm_name[256];
    pthread_spin_lock(&stat_lock);
    n->drive_id = total_drives++;
    pthread_spin_unlock(&stat_lock);
    sprintf(shm_name, "%s-%d", FEMU_STAT_SHM_NAME, n->drive_id);
    femu_debug("femu_stat_init: shm_name %s\n", shm_name);
    n->stat_shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (n->stat_shm_fd == -1) {
        femu_err("femu_stat_init: shm_open failed. %s\n", strerror(errno));
        n->stat = calloc(1, sizeof(FemuStat));
    } else {
        n->stat = mmap(NULL, sizeof(FemuStat), PROT_WRITE, MAP_SHARED, n->stat_shm_fd, 0);
        if (n->stat == MAP_FAILED) {
            femu_err("femu_stat_init: mmap failed. %s\n", strerror(errno));
            perror("mmap");
            exit(1);
        }
    }
    n->stat->tail = 0;
    n->stat->capacity = FEMU_STAT_CAPACITY;
    n->stat->start_time = clock_ns();
    n->stat->alive = 1;
}

void femu_stat_exit(FemuCtrl *n)
{
    for (int i = 0; i < 4; i++) {
        if (n->stat->cse_core_stat[i].idle_time + n->stat->cse_core_stat[i].exec_time != 0)
            femu_log("core %d: idle %lu, exec %lu, percent %.2f%%\n", i, n->stat->cse_core_stat[i].idle_time, n->stat->cse_core_stat[i].exec_time, (double)n->stat->cse_core_stat[i].exec_time / (n->stat->cse_core_stat[i].idle_time + n->stat->cse_core_stat[i].exec_time));
    }
    if (n->stat_shm_fd) {
        n->stat->end_time = clock_ns();
        n->stat->alive = 0;
        munmap(n->stat, sizeof(FemuStat));
        close(n->stat_shm_fd);
    } else {
        free(n->stat);
    }
    n->stat = NULL;
}

inline void femu_stat_add_req(FemuCtrl *n, NvmeRequest *req)
{
    if (n->stat_shm_fd == -1)
        return;

    memcpy(&n->stat->req_stat[n->stat->tail], &req->stat, sizeof(NvmeRequest));
    n->stat->tail = (n->stat->tail + 1) % n->stat->capacity;
}