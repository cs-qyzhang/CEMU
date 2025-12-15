/*
 * https://man7.org/linux/man-pages/man7/io_uring.7.html
 * https://man7.org/linux/man-pages/man2/io_uring_setup.2.html
 *   see IORING_SETUP_SQE128 and IORING_SETUP_CQE32
 * https://lore.gnuweeb.org/io-uring/20220505133354.GC11853@lst.de/T/
 * https://github.com/joshkan/fio/blob/big-cqe-pt.v4/engines/io_uring.c
 *   see fio_ioring_prep()
 *
 * require kernel >= 5.19
 */
#ifndef __FEMU_IO_URING_H
#define __FEMU_IO_URING_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/io_uring.h>
#include <liburing.h>
#include <linux/types.h>
#include "util.h"

#ifndef IOU_QUEUE_DEPTH
#define IOU_QUEUE_DEPTH     64
#endif

#define IORING_OP_COPY_FILE_RANGE   55

static inline void io_uring_prep_copy_file_range(struct io_uring_sqe *sqe,
        int fd_in, int fd_out, unsigned len, uint64_t off_in, uint64_t off_out)
{
    io_uring_prep_rw(IORING_OP_COPY_FILE_RANGE, sqe, fd_out, NULL, len, off_out);
	sqe->splice_off_in = off_in;
	sqe->splice_fd_in = fd_in;
	sqe->splice_flags = 0;
}

static inline void io_uring_prep_compute(struct io_uring_sqe *sqe, int fd, int cparam1, int cparam2, int pind, int rsid, unsigned int runtime, int group, int sched_prio)
{
    memset(sqe, 0, sizeof(struct io_uring_sqe));
    sqe->opcode = IORING_OP_URING_CMD;
    sqe->cmd_op = NVME_URING_CMD_IO;    // ioctl op, see linux/nvme_ioctl.h
    sqe->fd = fd;   // dev/ng0n3

    struct nvme_program_execute_cmd *cmd = (struct nvme_program_execute_cmd *)sqe->cmd;
    cmd->nsid = 3;
    cmd->opcode = 0x01;
    cmd->cparam1 = cparam1;
    cmd->cparam2 = cparam2;
    cmd->pind = pind;
    cmd->rsid = rsid;
    cmd->user_runtime = runtime;
    cmd->numr = 0;
    cmd->dlen = 0;
    cmd->group = group;
    cmd->chunk_nlb = sched_prio;
}

static int iouring_passthru_enqueue(struct io_uring *ring, int fd,
                                    const struct nvme_uring_cmd *cmd,
                                    void *user_data)
{
    // https://github.com/joshkan/fio/blob/big-cqe-pt.v4/engines/io_uring.c
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        printf("Failed to get io_uring sqe\n");
        return -1;
    }
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_URING_CMD;
    sqe->cmd_op = NVME_URING_CMD_IO;    // ioctl op, see linux/nvme_ioctl.h
    // fd: open('/dev/ng0n1')
    sqe->fd = fd;
    io_uring_sqe_set_data(sqe, user_data);
    memcpy(sqe->cmd, cmd, sizeof(struct nvme_uring_cmd));
    return 0;
}

static int iouring_submit(struct io_uring *ring)
{
    int nr = io_uring_submit(ring);
    if (nr < 0) {
        perror("io_uring_submit");
        return -1;
    }
    return nr;
}

static int iouring_wait_nr(struct io_uring *ring, int nr, struct io_uring_cqe **cqes)
{
    struct io_uring_cqe *tmp[IOU_QUEUE_DEPTH];
    if (cqes == NULL) {
        cqes = tmp;
    }
    if (io_uring_wait_cqe_nr(ring, &cqes[0], nr) < 0) {
        perror("io_uring_wait_cqes");
        return -1;
    }
    int ret = io_uring_peek_batch_cqe(ring, &cqes[0], nr);
    if (ret != nr) {
        fprintf(stderr, "io_uring_wait_cqe_nr: %d/%d!\n", ret, nr);
        return -1;
    }
    for (int i = 0; i < nr; i++) {
        struct io_uring_cqe *cqe = cqes[i];
        int err = cqe->big_cqe[0] || cqe->res;
        if (err < 0) {
            fprintf(stderr, "io_uring_wait_cqe_nr: %d, %s\n", err, strerror(-err));
            return -1;
        }
        io_uring_cqe_seen(ring, cqe);
    }
    return nr;
}

#endif // __FEMU_IO_URING_H