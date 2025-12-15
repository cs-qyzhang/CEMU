#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include "iouring.h"

static unsigned long long clock_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ULL + t.tv_nsec;
}

int main(void) {
    struct io_uring ring;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;

    // setup iouring
    int ret = io_uring_queue_init(64, &ring,
        IORING_SETUP_SQE128 | IORING_SETUP_CQE32 |
        IORING_SETUP_IOPOLL | IORING_SETUP_SQPOLL);
    if (ret < 0) {
        perror("io_uring_queue_init");
        exit(EXIT_FAILURE);
    }

    printf("iouring setuped\n");

    // open file
    int nvm_fd = open("/mnt/nvme0/test", O_RDWR | O_DIRECT);
    if (nvm_fd == -1) {
        perror("open nvm_fd");
        exit(EXIT_FAILURE);
    }
    int fdm0_fd = open("/mnt/fdm0/0", O_RDWR | O_DIRECT);
    if (fdm0_fd == -1) {
        perror("open fdm_fd");
        exit(EXIT_FAILURE);
    }

    printf("files opened\n");

    // read nvme test
    char *buf = aligned_alloc(4096, 1024*1024*32);
    size_t off = 0;
    size_t size = 1024*1024;
    for (int i = 0; i < 10; i++) {
        sqe = io_uring_get_sqe(&ring);
        if (!sqe) {
            return 1;
        }
        io_uring_prep_read(sqe, fdm0_fd, buf, size, off);
        // io_uring_prep_copy_file_range(sqe, nvm_fd, fdm0_fd, size, off, off);
        off += size;

        int nr = io_uring_submit(&ring);
        if (nr < 0) {
            perror("io_uring_submit");
            return -1;
        }

        unsigned long long start = clock_ns();
        while (io_uring_cq_ready(&ring) == 0)
            ;
        unsigned long long end = clock_ns();
        ret = io_uring_peek_cqe(&ring, &cqe);
        if (ret) {
            perror("io_uring_wait_cqe\n");
            return 1;
        }

        printf("nvm read waited %d, res %d, time %lluus, start %llu, end %llu\n",
               nr, cqe->res,
               (unsigned long long)((end - start) / 1000),
               start, end);
        io_uring_cqe_seen(&ring, cqe);
    }

    off = 0;
    for (int i = 0; i < 10; i++) {
        unsigned long long start = clock_ns();
        int ret = pread(fdm0_fd, buf, size, off);
        unsigned long long end = clock_ns();
        if (ret < 0) {
            perror("pread");
            return -1;
        }
        printf("nvm read waited %d, time %lluus, start %llu, end %llu\n",
               ret,
               (unsigned long long)((end - start) / 1000),
               start, end);
    }

    close(nvm_fd);
    close(fdm0_fd);
    return 0;
}