/*
 * Test P2P communication between NVM of CSD1 and FDM of CSD0.
 */
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

static inline __attribute__((unused)) uint64_t get_clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(void) {
    int nvm_fd = open("/mnt/nvme1/test", O_RDWR | O_DIRECT | O_CREAT, 0644);
    if (nvm_fd == -1) {
        perror("open nvm_fd");
        exit(EXIT_FAILURE);
    }

    char *buf = static_cast<char *>(aligned_alloc(512, 4096));
    strcpy(buf, "hello, cemu!");

    ssize_t ret = pwrite(nvm_fd, buf, 4096, 0);
    if (ret < 512) {
        perror("pwrite");
        exit(EXIT_FAILURE);
    }

    int fdm_fd = open("/mnt/fdm0/0", O_RDWR | O_DIRECT | O_CREAT, 0644);
    if (fdm_fd == -1) {
        perror("open fdm_fd");
        exit(EXIT_FAILURE);
    }

    loff_t in_off = 0;
    loff_t out_off = 0;
    ret = copy_file_range(nvm_fd, &in_off, fdm_fd, &out_off, 4096, 0);
    if (ret < 0) {
        perror("copy_file_range");
        exit(EXIT_FAILURE);
    }

    memset(buf, 0, 4096);
    ret = pread(fdm_fd, buf, 4096, 0);
    if (ret == -1) {
        perror("pread");
        exit(EXIT_FAILURE);
    }
    printf("%s\n", buf);

    close(nvm_fd);

    return 0;
}