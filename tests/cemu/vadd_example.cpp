#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "iouring.h"
#include "util.h"

int main(int argc, char *argv[])
{
    struct ioctl_download download {};
    download.name = "vadd";
    int ret;
    bool use_ebpf = false;
    bool use_jit = false;
    bool exec_use_iou = false;
    bool src_in_file = false;
    bool dst_in_file = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ebpf") == 0) {
            use_ebpf = true;
        } else if (strcmp(argv[i], "--jit") == 0) {
            use_jit = true;
        } else if (strcmp(argv[i], "--src-in-file") == 0) {
            src_in_file = true;
        } else if (strcmp(argv[i], "--dst-in-file") == 0) {
            dst_in_file = true;
        }
    }

    printf("use_ebpf: %d, use_jit: %d\n", use_ebpf, use_jit);
    printf("exec_use_iou: %d\n", exec_use_iou);
    printf("src_in_file: %d, dst_in_file: %d\n", src_in_file, dst_in_file);

    if (use_ebpf) {
        prep_ebpf("./build/vadd.bpf.o", "vadd", use_jit, &download);
    } else {
        prep_shared_library("./build/vadd.so", "vadd", &download);
    }

    int ctl_fd = open("/dev/nvme0c3", O_RDWR);
    if (ctl_fd == -1) {
        perror("open dev file");
        exit(1);
    }

    // load program
    ret = ioctl(ctl_fd, IOCTL_CEMU_DOWNLOAD, &download);
    if (ret <= 0)
        perror("ioctl");
    printf("download ret: %d, pind : %d\n", ret, download.pind);

    // activate program
    ret = ioctl(ctl_fd, IOCTL_CEMU_ACTIVATE, &download);
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
    ret = ioctl(ctl_fd, IOCTL_CEMU_CREATE_MRS, &mrs);
    if (ret)
        perror("ioctl CREATE_MRS");
    printf("create_mrs ret: %d, rsid : %d\n", ret, mrs.rsid);

    // create input data
    int *buf_in = static_cast<int *>(aligned_alloc(4096, 4096 * 2));
    int *buf_out = static_cast<int *>(aligned_alloc(4096, 4096));
    for (int i = 0; i < 1024; i++) {
        buf_in[i] = i;
        buf_in[i*2] = i*2;
        buf_out[i] = 0;
    }

    if (src_in_file) {
        // write data to file
        int nvm_in_fd = open("/mnt/nvme0/in", O_RDWR | O_CREAT | O_DIRECT, 0666);
        if (nvm_in_fd == -1) {
            perror("open nvm in file");
            exit(1);
        }
        ret = pwrite(nvm_in_fd, buf_in, 8192, 0);
        assert(ret == 8192);

        // copy data from nvm to fdm
        long off_in = 0;
        long off_out = 0;
        ret = copy_file_range(nvm_in_fd, &off_in, mrs.fd[1], &off_out, 8192, 0);
        assert(ret == 8192);

        close(nvm_in_fd);
    } else {
        // directly write data to memory range set
        ret = pwrite(mrs.fd[1], buf_in, 8192, 0);
        assert(ret == 8192);
    }
    // clear output buffer
    ret = pwrite(mrs.fd[0], buf_out, 4096, 0);
    assert(ret == 4096);

    // execute program
    int ng_fd = open("/dev/ng0n3", O_RDWR);
    if (ng_fd == -1) {
        perror("open dev file");
        exit(1);
    }

    struct nvme_passthru_cmd ioctl_cmd {};
    prep_nvme_passthru_program_execute(&ioctl_cmd,
                                       /*cparam1=*/1024,
                                       /*cparam2=*/0,
                                       /*pind=*/download.pind,
                                       /*rsid=*/mrs.rsid,
                                       /*numr=*/0,
                                       /*group=*/0,
                                       /*chunk_nlb=*/0,
                                       /*user_runtime=*/0,
                                       /*data=*/NULL,
                                       /*data_len=*/0);

    struct nvme_uring_cmd uring_cmd {};
    prep_nvme_uring_program_execute(&uring_cmd,
                                    /*cparam1=*/1024,
                                    /*cparam2=*/0,
                                    /*pind=*/download.pind,
                                    /*rsid=*/mrs.rsid,
                                    /*numr=*/0,
                                    /*group=*/0,
                                    /*chunk_nlb=*/0,
                                    /*user_runtime=*/0,
                                    /*data=*/NULL,
                                    /*data_len=*/0);

    // passthru program execute nvme command
    if (exec_use_iou) {
        // use iouring nvme passthru
        struct io_uring iou;
        io_uring_queue_init(128, &iou, IORING_SETUP_SQE128 | IORING_SETUP_CQE32);
        iouring_passthru_enqueue(&iou, ng_fd, &uring_cmd, NULL);
        iouring_submit(&iou);

        int nr = io_uring_submit(&iou);
        if (nr < 0) {
            perror("io_uring_submit");
            return -1;
        }
        printf("submitted %d\n", nr);

        nr = iouring_wait_nr(&iou, 1, NULL);
        if (nr < 0) {
            perror("io_uring_wait_nr");
            return -1;
        }
        printf("waited %d\n", nr);
    } else {
        // use ioctl nvme passthru
        ret = ioctl(ng_fd, NVME_IOCTL_IO_CMD, &ioctl_cmd);
        printf("ioctl nvme passthru ret: %d\n", ret);
        if (ret < 0) {
            perror("ioctl");
            return 1;
        }
    }

    // validate data
    if (dst_in_file) {
        // copy data from fdm to nvm
        int nvm_out_fd = open("/mnt/nvme0/out", O_RDWR | O_CREAT | O_DIRECT, 0666);
        if (nvm_out_fd == -1) {
            perror("open nvm out file");
            exit(1);
        }
        long off_in = 0;
        long off_out = 0;
        ret = copy_file_range(mrs.fd[0], &off_in, nvm_out_fd, &off_out, 4096, 0);
        assert(ret == 4096);

        // read output from nvm
        ret = pread(nvm_out_fd, buf_out, 4096, 0);
        assert(ret == 4096);
        close(nvm_out_fd);
    } else {
        // read directly from fdm
        ret = pread(mrs.fd[0], buf_out, 4096, 0);
        assert(ret == 4096);
    }

    for (int i = 0; i < 1024; i++) {
        assert(buf_in[i*2] + buf_in[i*2+1] == buf_out[i]);
    }
    printf("data validation passed\n");

    // deactivate program
    ret = ioctl(ctl_fd, IOCTL_CEMU_DEACTIVATE, &download);
    if (ret)
        perror("ioctl");
    printf("deactivation ret: %d, pind : %d\n", ret, download.pind);

    // unload program
    ret = ioctl(ctl_fd, IOCTL_CEMU_UNLOAD, &download);
    if (ret)
        perror("ioctl");
    printf("remove ret: %d, pind : %d\n", ret, download.pind);

    close(ctl_fd);
    close(ng_fd);
    for (int i = 0; i < mrs.nr_fd; i++) {
        close(mrs.fd[i]);
    }
    free(buf_in);
    free(buf_out);

    return 0;
}
