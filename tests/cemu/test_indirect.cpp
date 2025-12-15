#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <chrono>
#include <iostream>
#include <getopt.h>
#include <random>

#include "iouring.h"
#include "util.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define IOU_QUEUE_DEPTH         64
#define DEFAULT_THREADS         2
#define DEFAULT_FILE_SIZE       (1024ULL*1024*128)
#define DEFAULT_CHUNK_SIZE      (1024ULL*1024)
#define DEFAULT_PARALLEL_CHUNKS 4
#define DEFAULT_KERNEL_TIME     10020000
#define DEFAULT_PRINT_TIME      0

bool print_time = DEFAULT_PRINT_TIME;
uint64_t kernel_time = DEFAULT_KERNEL_TIME / (1024ULL*1024*16/DEFAULT_KERNEL_TIME);
uint64_t chunk_size = DEFAULT_CHUNK_SIZE;
int parallel_chunks = DEFAULT_PARALLEL_CHUNKS;
uint64_t file_size = DEFAULT_FILE_SIZE;
int nr_threads = DEFAULT_THREADS;
std::string datafile;

static void parse_arg(int argc, char **argv) {
    int opt;
    int option_index = 0;
    static struct option long_options[] = {
        {"print_time", no_argument, 0, 'v'},
        {"chunk_size", required_argument, 0, 'c'},
        {"parallel_chunks", required_argument, 0, 'p'},
        {"file", required_argument, 0, 'f'},
        {"size", required_argument, 0, 's'},
        {"threads", required_argument, 0, 't'},
        {0, 0, 0, 0}
    };
    while ((opt = getopt_long(argc, argv, "vc:p:f:s:t:", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'v':
            print_time = 1;
            break;
        case 'c':
            chunk_size = atoll(optarg) * 1024ULL * 1024ULL;
            break;
        case 'p':
            parallel_chunks = atoi(optarg);
            break;
        case 'f':
            datafile = optarg;
            break;
        case 's':
            file_size = atoll(optarg) * 1024ULL * 1024ULL;
            break;
        case 't':
            nr_threads = atoi(optarg);
            break;
        default:
            std::cerr << "Usage: ./test_indirect [-c chunk_size_in_mb] [-p parallel_chunks] [-t]\n";
            exit(EXIT_FAILURE);
        }
    }
    kernel_time = DEFAULT_KERNEL_TIME / (1024ULL*1024*16/chunk_size);
}

int main(int argc, char **argv)
{
    parse_arg(argc, argv);

    std::cout << "INFO: chunk_size      = " << chunk_size / 1024.0 / 1024 << " MiB" << std::endl;
    std::cout << "INFO: parallel_chunks = " << parallel_chunks << std::endl;
    std::cout << "INFO: nr_threads      = " << nr_threads << std::endl;
    std::cout << "INFO: file_size       = " << file_size / 1024.0 / 1024.0 / 1024.0 << " GiB" << std::endl;
    std::cout << "INFO: kernel_time     = " << kernel_time / 1000 << " us" << std::endl;
    std::cout << "INFO: print_time      = " << print_time << std::endl;
    std::cout << "INFO: datafile        = " << datafile << std::endl;

    struct ioctl_download download{};
    download.name = "vadd_indirect";
    download.indirect = 1;
    prep_shared_library("./build/vadd.so", "vadd_indirect", &download);

    int ctl_fd = open("/dev/nvme0c3", O_RDWR);
    if (ctl_fd == -1) {
        perror("open dev file");
        exit(1);
    }

    // load program
    int ret = ioctl(ctl_fd, IOCTL_CEMU_DOWNLOAD, &download);
    if (ret <= 0)
        perror("ioctl DOWNLOAD");
    printf("download ret: %d, pind : %d\n", ret, download.pind);

    // activate program
    printf("IOCTL_CEMU_ACTIVATE: %lu\n", (unsigned long)IOCTL_CEMU_ACTIVATE);
    ret = ioctl(ctl_fd, IOCTL_CEMU_ACTIVATE, &download);
    if (ret)
        perror("ioctl ACTIVATE");
    printf("activation ret: %d, pind : %d\n", ret, download.pind);

    // create memory range set
    struct ioctl_create_mrs mrs{};
    prep_mrs(&mrs, 3 * parallel_chunks);
    for (int i = 0; i < mrs.nr_fd; i++) {
        char buf[64];
        sprintf(buf, "/mnt/fdm0/%d", i);
        mrs.fd[i] = open(buf, O_RDWR);
        mrs.off[i] = 0;
        mrs.size[i] = chunk_size;
        if (mrs.fd[i] == -1) {
            perror("open data file");
            exit(1);
        }
        if ((i + 1) % 3 == 0) {
            char *tmp_buf = (char *)aligned_alloc(4096, 64);
            memset(tmp_buf, 0, 64);
            int ret = pwrite(mrs.fd[i], tmp_buf, 64, 0);
            if (ret < 0) {
                perror("pwrite");
                exit(1);
            }
            printf("pwrite global_mem: %d\n", ret);
            free(tmp_buf);
        }
    }
    ret = ioctl(ctl_fd, IOCTL_CEMU_CREATE_MRS, &mrs);
    if (ret)
        perror("ioctl CREATE_MRS");
    printf("create_mrs ret: %d, rsid : %d\n", ret, mrs.rsid);

    // execute program
    struct io_uring iou;
    io_uring_queue_init(IOU_QUEUE_DEPTH, &iou, IORING_SETUP_SQE128 | IORING_SETUP_CQE32);

    int ng_fd = open("/dev/ng0n3", O_RDWR);
    if (ng_fd == -1) {
        perror("open dev file");
        exit(1);
    }
    printf("ng_fd: %d\n", ng_fd);

    // get input nvm file mappings
    int *nr_in_cf_per_chunk = new int[parallel_chunks];
    int *nr_out_cf_per_chunk = new int[parallel_chunks];
    int *in_nlb_per_chunk = new int[parallel_chunks];
    int *out_nlb_per_chunk = new int[parallel_chunks];
    uint64_t total_nlb = (file_size / LBA_SIZE) / (2 * parallel_chunks) * (2 * parallel_chunks);
    file_size = total_nlb * LBA_SIZE;
    printf("total_nlb: %lu, file_size: %lu\n", total_nlb, file_size);
    for (int i = 0; i < parallel_chunks; i++) {
        in_nlb_per_chunk[i] = 0;
        out_nlb_per_chunk[i] = 0;
    }

    NvmeCopyFormat2 *cf = NULL;
    int nr_cf = get_file_mappings("/mnt/nvme0/test", &file_size, &cf, parallel_chunks, in_nlb_per_chunk, nr_in_cf_per_chunk);
    if (nr_cf < 0) {
        perror("get_file_mappings");
        return -1;
    }
    for (int i = 0; i < parallel_chunks; i++) {
        printf("in_nlb_per_chunk[%d]: %d, nr_in_cf_per_chunk[%d]: %d\n", i, in_nlb_per_chunk[i], i, nr_in_cf_per_chunk[i]);
    }
    printf("get_file_mappings: size %.2lfGiB, nr_cf %d\n", file_size / 1024.0 / 1024 / 1024, nr_cf);
    for (int i = 0; i < MIN(10, nr_cf); i++)
        printf("  cf[%d]: nsid %d, slba %lu, nlb %d\n", i, cf[i].snsid, cf[i].slba, cf[i].nlb);
    if (nr_cf > 10) {
        printf("  ......\n");
        for (int i = MAX(10, nr_cf - 10); i < nr_cf; i++)
            printf("  cf[%d]: nsid %d, slba %lu, nlb %d\n", i, cf[i].snsid, cf[i].slba, cf[i].nlb);
    }

    // get output nvm file mappings
    NvmeCopyFormat2 *out_cf = NULL;
    file_size = file_size / 2;
    for (int i = 0; i < parallel_chunks; i++) {
        out_nlb_per_chunk[i] = in_nlb_per_chunk[i] / 2;
        assert(in_nlb_per_chunk[i] % 2 == 0);
    }
    int nr_out_cf = get_file_mappings("/mnt/nvme0/output", &file_size, &out_cf, parallel_chunks, out_nlb_per_chunk, nr_out_cf_per_chunk);
    if (nr_out_cf < 0) {
        perror("get_file_mappings");
        return -1;
    }
    printf("get_file_mappings: size %.2lfGiB, nr_out_cf %d\n", file_size / 1024.0 / 1024 / 1024, nr_out_cf);
    for (int i = 0; i < parallel_chunks; i++) {
        printf("out_nlb_per_chunk[%d]: %d, nr_out_cf_per_chunk[%d]: %d\n", i, out_nlb_per_chunk[i], i, nr_out_cf_per_chunk[i]);
    }
    for (int i = 0; i < MIN(10, nr_out_cf); i++)
        printf("  out_cf[%d]: nsid %d, slba %lu, nlb %d\n", i, out_cf[i].snsid, out_cf[i].slba, out_cf[i].nlb);
    if (nr_out_cf > 10) {
        printf("  ......\n");
        for (int i = MAX(10, nr_out_cf - 10); i < nr_out_cf; i++)
            printf("  out_cf[%d]: nsid %d, slba %lu, nlb %d\n", i, out_cf[i].snsid, out_cf[i].slba, out_cf[i].nlb);
    }

    int *buf_in = (int *)aligned_alloc(4096, 8192);
    int *buf_out = (int *)aligned_alloc(4096, 8192);
    int in_fd = open("/mnt/nvme0/test", O_RDWR | O_DIRECT);
    int out_fd = open("/mnt/nvme0/output", O_RDWR | O_DIRECT);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 1000000);

    for (size_t i = 0; i < 8192 / sizeof(int); i++) {
        buf_out[i] = dis(gen);
    }

    for (size_t offset = 0; offset < file_size; offset += 8192) {
        if (pwrite(out_fd, buf_out, 8192, offset) < 0) {
            perror("pwrite");
            return -1;
        }
    }

    struct nvme_uring_cmd cmd{};
    uint64_t cparam1 = chunk_size / sizeof(int) / 2; // vadd_indirect param
    uint32_t chunk_nlb = (chunk_size / 512) - 1;

    bool output_in_nvm = true;
    int task_info_len = sizeof(int)*4 + parallel_chunks*sizeof(int) + nr_cf*sizeof(NvmeCopyFormat2);
    if (output_in_nvm) {
        task_info_len += parallel_chunks*sizeof(int) + nr_out_cf*sizeof(NvmeCopyFormat2);
    }

    char *data_buffer = (char *)aligned_alloc(4096, task_info_len);
    char *cur_ptr = data_buffer;

    // input sres
    *((int *)cur_ptr) = parallel_chunks;
    cur_ptr += sizeof(int);
    *((int *)cur_ptr) = output_in_nvm ? 1 : 0; // output mode
    cur_ptr += sizeof(int);
    *((int *)cur_ptr) = nr_cf;
    cur_ptr += sizeof(int);
    *((int *)cur_ptr) = output_in_nvm ? nr_out_cf : 0;
    cur_ptr += sizeof(int);

    memcpy(cur_ptr, nr_in_cf_per_chunk, parallel_chunks*sizeof(int));
    cur_ptr += parallel_chunks*sizeof(int);

    if (output_in_nvm) {
        memcpy(cur_ptr, nr_out_cf_per_chunk, parallel_chunks*sizeof(int));
        cur_ptr += parallel_chunks*sizeof(int);
    }

    memcpy(cur_ptr, cf, nr_cf * sizeof(NvmeCopyFormat2));
    free(cf);
    cur_ptr += nr_cf * sizeof(NvmeCopyFormat2);

    // output sres
    if (output_in_nvm) {
        memcpy(cur_ptr, out_cf, nr_out_cf * sizeof(NvmeCopyFormat2));
        free(out_cf);
        cur_ptr += nr_out_cf * sizeof(NvmeCopyFormat2);
    }

    prep_nvme_uring_program_execute(&cmd,
                                    /*cparam1=*/cparam1,
                                    /*cparam2=*/0,
                                    /*pind=*/download.pind,
                                    /*rsid=*/mrs.rsid,
                                    /*numr=*/nr_cf,
                                    /*group=*/0,
                                    /*chunk_nlb=*/chunk_nlb,
                                    /*user_runtime=*/kernel_time,
                                    /*data=*/data_buffer,
                                    /*data_len=*/task_info_len);
    printf("kernel time: %lu, chunk_nlb: %u\n", kernel_time, chunk_nlb);

    printf("nr_cf: %d\n", nr_cf);
    for (int i = 0; i < parallel_chunks * 2 + 4; i++) {
        printf("data_buffer[%d]: %d\n", i, *((int *)data_buffer + i));
    }

    printf("opcode: %d\n", cmd.opcode);
    iouring_passthru_enqueue(&iou, ng_fd, &cmd, NULL);

    int nr = io_uring_submit(&iou);
    if (nr < 0) {
        perror("io_uring_submit");
        return -1;
    }
    auto start = std::chrono::high_resolution_clock::now();

    nr = iouring_wait_nr(&iou, 1, NULL);
    if (nr < 0) {
        perror("io_uring_wait_nr");
        return -1;
    }
    auto end = std::chrono::high_resolution_clock::now();
    printf("waited %d, time %ldus\n", nr, std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());

    // validate data
    for (uint64_t offset = 0; offset + 1 < file_size; offset += 8192) {
        ret = pread(in_fd, buf_in, 8192, offset);
        if (ret < 0) {
            perror("pread");
            return -1;
        }
        ret = pread(out_fd, buf_out, 4096, offset / 2);
        if (ret < 0) {
            perror("pread");
            return -1;
        }
        for (unsigned long i = 0; i < 4096 / sizeof(int); i++) {
            assert(buf_in[i * 2] + buf_in[i * 2 + 1] == buf_out[i]);
        }
    }
    printf("test passed\n");

    free(data_buffer);

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

    return 0;
}