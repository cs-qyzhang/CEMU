#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>
#include <time.h>

#include "util.h"

static inline uint64_t get_clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(void) {
    int nvm_fd = open("/mnt/nvme0/0", O_RDWR | O_DIRECT);
    if (nvm_fd == -1) {
        perror("open nvm_fd");
        exit(EXIT_FAILURE);
    }

    uint64_t start, end;
    long off_in = 0;
    long off_out = 0;
    int ret;
    ssize_t length = 1024 * 512;
    char *buf = static_cast<char *>(aligned_alloc(4096, length));
    assert(buf != NULL);

    uint64_t copy_time = 0;
    uint64_t exec_time = 0;
    uint64_t read_time = 0;

    int fd = open("/dev/nvme0n1", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    struct ioctl_download download {};
    download.name = "vadd";
    prep_shared_library("./build/vadd.so", "vadd", &download);

    const char *file = "/dev/nvme0c3";
    int compute_fd = open(file, O_RDWR);
    if (compute_fd == -1) {
        perror("open dev file");
        exit(1);
    }

    // load program
    ret = ioctl(compute_fd, IOCTL_CEMU_DOWNLOAD, &download);
    if (ret <= 0)
        perror("ioctl");
    printf("download ret: %d, pind : %d\n", ret, download.pind);

    // activate program
    ret = ioctl(compute_fd, IOCTL_CEMU_ACTIVATE, &download);
    if (ret)
        perror("ioctl");
    printf("activation ret: %d, pind : %d\n", ret, download.pind);

    // create memory range set
    struct ioctl_create_mrs mrs;
    prep_mrs(&mrs, 2);
    for (int i = 0; i < mrs.nr_fd; i++) {
        char buf[64];
        sprintf(buf, "/mnt/fdm0/%d", i);
        mrs.fd[i] = open(buf, O_RDWR);
        if (mrs.fd[i] == -1) {
            perror("open data file");
            exit(1);
        }
    }
    ret = ioctl(compute_fd, IOCTL_CEMU_CREATE_MRS, &mrs);
    if (ret)
        perror("ioctl CREATE_MRS");
    printf("create_mrs ret: %d, rsid : %d\n", ret, mrs.rsid);

    struct nvme_passthru_cmd passthru_cmd;
    memset(&passthru_cmd, 0, sizeof(passthru_cmd));
    struct nvme_program_execute_cmd *cmd =
        reinterpret_cast<struct nvme_program_execute_cmd *>(&passthru_cmd);
    cmd->nsid = 3;
    cmd->opcode = 0x01;
    // cmd->cparam1 = 1024 / 4;
    // cmd->cparam2 = 0;
    // cmd->pind = download.pind;
    // cmd->rsid = mrs.rsid;
    // cmd->user_runtime = 626000;

    off_in = 0;
    off_out = 0;
    for (int i = 0; i < 1024; i++) {
        start = get_clock();
        ret = copy_file_range(nvm_fd, &off_in, mrs.fd[0], &off_out, length, 0);
        end = get_clock();
        printf("copy_file_range ret: %ld\n", static_cast<long>(ret));
        if (ret == -1) {
            perror("copy_file_range");
            exit(EXIT_FAILURE);
        }
        copy_time += end - start;

        start = get_clock();
        ret = ioctl(fd, NVME_IOCTL_IO_CMD, cmd);
        printf("ioctl ret: %d\n", ret);
        if (ret < 0) {
            perror("ioctl");
            return 1;
        }
        end = get_clock();
        exec_time += end - start;

        start = get_clock();
        ret = pread(mrs.fd[1], buf, length, 0);
        end = get_clock();
        if (ret == -1) {
            perror("pread");
            exit(EXIT_FAILURE);
        }
        read_time += end - start;
    }

    uint64_t total_time = copy_time + read_time + exec_time;
    printf("copy_time: %lu, exec_time: %lu read_time: %lu, total: %lu\n", copy_time / 1000, exec_time / 1000, read_time / 1000, total_time / 1000);

    close(nvm_fd);

    return 0;
}