#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>

static inline uint64_t get_clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(int argc, char *argv[]) {
    std::string drive_id = "0";
    if (argc == 2) {
        drive_id = argv[1];
    }

    std::string nvm_path = "/mnt/nvme" + drive_id + "/test";
    int nvm_fd = open(nvm_path.c_str(), O_RDWR | O_DIRECT);
    if (nvm_fd == -1) {
        perror("open nvm_fd");
        exit(EXIT_FAILURE);
    }

    std::string fdm0_path = "/mnt/fdm" + drive_id + "/0";
    int fdm0_fd = open(fdm0_path.c_str(), O_RDWR);
    if (fdm0_fd == -1) {
        perror("open fdm_fd 0");
        exit(EXIT_FAILURE);
    }

    std::string fdm1_path = "/mnt/fdm" + drive_id + "/1";
    int fdm1_fd = open(fdm1_path.c_str(), O_RDWR);
    if (fdm1_fd == -1) {
        perror("open fdm_fd 1");
        exit(EXIT_FAILURE);
    }

    uint64_t start, end;
    long off_in = 0;
    long off_out = 0;
    ssize_t ret;
    ssize_t length = 1024*1024*16;
    char *buf = (char *)aligned_alloc(4096, length);
    assert(buf != NULL);


    printf("Starting copy_file_range for nvm->fdm...\n");
    off_in = 0;
    off_out = 0;
    start = get_clock();
    ret = copy_file_range(nvm_fd, &off_in, fdm0_fd, &off_out, length, 0);
    end = get_clock();
    if (ret == -1) {
        perror("copy_file_range");
        exit(EXIT_FAILURE);
    }
    printf("copy_file_range success! ret: %ld, time: %lu, bw: %lfMB/s\n",
            ret, end - start, (double)length / (end - start) * 1000.0);

    printf("Starting copy_file_range for fdm->nvm...\n");
    off_in = 0;
    off_out = 0;
    start = get_clock();
    ret = copy_file_range(fdm0_fd, &off_in, nvm_fd, &off_out, length, 0);
    end = get_clock();
    if (ret == -1) {
        perror("copy_file_range");
        exit(EXIT_FAILURE);
    }
    printf("copy_file_range success! ret: %ld, time: %lu, bw: %lfMB/s\n",
            ret, end - start, (double)length / (end - start) * 1000.0);

    printf("Starting copy_file_range for fdm->fdm...\n");
    off_in = 0;
    off_out = 0;
    start = get_clock();
    ret = copy_file_range(fdm0_fd, &off_in, fdm1_fd, &off_out, length, 0);
    end = get_clock();
    if (ret == -1) {
        perror("copy_file_range");
        exit(EXIT_FAILURE);
    }
    printf("copy_file_range success! ret: %ld, time: %lu, bw: %lfMB/s\n",
            ret, end - start, (double)length / (end - start) * 1000.0);

    length -= 1024;
    printf("Starting read...\n");
    off_out = 512;
    start = get_clock();
    ret = pread(fdm0_fd, buf, length, off_out);
    end = get_clock();
    if (ret == -1) {
        perror("pread");
        exit(EXIT_FAILURE);
    }
    printf("pread success! ret: %ld, time: %lu, bw: %lfMB/s\n",
            ret, end - start, (double)length / (end - start) * 1000.0);

    printf("Starting write...\n");
    off_out = 512;
    start = get_clock();
    ret = pwrite(fdm0_fd, buf, length, off_out);
    end = get_clock();
    if (ret == -1) {
        perror("pwrite");
        exit(EXIT_FAILURE);
    }
    printf("pwrite success! ret: %ld, time: %lu, bw: %lfMB/s\n",
            ret, end - start, (double)length / (end - start) * 1000.0);

    close(nvm_fd);
    close(fdm0_fd);
    close(fdm1_fd);

    return 0;
}
