/*
 * benchmark
 */
#include <assert.h>
#include <numeric>
#include <iostream>
#include <chrono>
#include <deque>
#include <vector>
#include <thread>
#include <fstream>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <getopt.h>

#include "util.h"
#include "iouring.h"

#define IOU_QUEUE_DEPTH         64
#define DEFAULT_THREADS         1
#define DEFAULT_FILE_SIZE       (1024ULL*1024*1024)
#define DEFAULT_CHUNK_SIZE      (1024ULL*1024*8)
#define DEFAULT_PARALLEL_CHUNKS 1
#define DEFAULT_KERNEL_TIME     10020000
#define DEFAULT_PRINT_TIME      0
#define DEFAULT_DRIVE           "0"
#define DEFAULT_DATAFILE        "test"

bool print_time = DEFAULT_PRINT_TIME;
uint64_t kernel_time = DEFAULT_KERNEL_TIME / (1024ULL*1024*16/DEFAULT_KERNEL_TIME);
uint64_t chunk_size = DEFAULT_CHUNK_SIZE;
int parallel_chunks = DEFAULT_PARALLEL_CHUNKS;
uint64_t file_size = DEFAULT_FILE_SIZE;
int nr_threads = DEFAULT_THREADS;
int runtime_scale = 0;
const char *kernel_file = "./build/vadd.so";
const char *kernel = "vadd";
std::vector<std::string> drives;
std::string datafile = DEFAULT_DATAFILE;
size_t output_size = chunk_size / 8;
double output_ratio = 0.2;
bool output_mode = false; // true for write back to file, false for read back to host
int start_fdm_id = 0;
int start_nvm_id = 0;
int group = 0;
uint64_t runtime = 0;
double bw = 0;
int sched_prio = 0;

std::vector<int> kernel_times;
std::vector<int> input_times;
std::vector<int> output_times;

static void split(const std::string &s, char delim, std::vector<std::string> &result) {
    std::stringstream ss(s);
    std::string item;
    while (getline(ss, item, delim))
        result.push_back (item);
}

struct MemoryRange {
    std::string file;
    int fd;
    long long off;
    long long size;

    MemoryRange(std::string file, size_t off, size_t size)
        : file(file), off(off), size(size)
    {
        fd = open(file.c_str(), O_RDWR | O_DIRECT);
        std::cout << "Open " << file << std::endl;
        if (fd == -1) {
            perror(("open " + file).c_str());
            exit(EXIT_FAILURE);
        }
    }
};

struct MemoryRangeSet {
    int rsid;
    int nr_fdm;
    int *fdm_fd;
    long long *fdm_off;
    long long *fdm_size;

    MemoryRangeSet(std::vector<MemoryRange> mr) {
        nr_fdm = mr.size();
        fdm_fd = new int[nr_fdm];
        fdm_off = new long long[nr_fdm];
        fdm_size = new long long[nr_fdm];
        for (int i = 0; i < nr_fdm; i++) {
            fdm_fd[i] = mr[i].fd;
            fdm_off[i] = mr[i].off;
            fdm_size[i] = mr[i].size;
        }
    }
};

struct Task {
    int nvm_fd;
    int nvm_out_fd;
    int ng3_fd;
    int ctl_fd;
    size_t nvm_off;
    size_t nvm_size;
    size_t cur_off;
    size_t chunk_size;
    std::vector<MemoryRangeSet*> mrs;

    int pind;
    long long cparam1;
    long long cparam2;

    int parallel_chunks;

    Task(std::string nvm_file, std::string nvm_out_file, std::string drive_id,
            std::vector<MemoryRangeSet*> mrs, size_t chunk_size,
            std::string kernel_file, std::string kernel, long long cparam1,
            long long cparam2, size_t nvm_size = 0, size_t nvm_off = 0)
        :nvm_off(nvm_off), nvm_size(nvm_size), cur_off(0), chunk_size(chunk_size), cparam1(cparam1), cparam2(cparam2), parallel_chunks(mrs.size())
    {
        nvm_fd = open(nvm_file.c_str(), O_RDWR | O_DIRECT);
        std::cout << "Open " << nvm_file << std::endl;
        if (nvm_fd == -1) {
            perror("open nvm_fd");
            exit(EXIT_FAILURE);
        }
        // get nvm_fd file size
        if (nvm_size == 0) {
            struct stat st;
            fstat(nvm_fd, &st);
            this->nvm_size = st.st_size - nvm_off;
        }
        if (output_mode) {
            nvm_out_fd = open(nvm_out_file.c_str(), O_RDWR | O_CREAT | O_DIRECT);
            std::cout << "Open " << nvm_out_file << std::endl;
            int ret = fallocate(nvm_out_fd, 0, 0, this->nvm_size);
            if (ret) {
                perror("fallocate nvm_out_fd");
                exit(EXIT_FAILURE);
            }
            if (nvm_out_fd == -1) {
                perror("open nvm_fd");
                exit(EXIT_FAILURE);
            }
        }
        std::string ng3_path = "/dev/ng" + drive_id + "n3";
        ng3_fd = open(ng3_path.c_str(), O_RDWR);
        if (ng3_fd == -1) {
            perror("open ng3_fd");
            exit(EXIT_FAILURE);
        }
        std::string compute_path = "/dev/nvme" + drive_id + "c3";
        ctl_fd = open(compute_path.c_str(), O_RDWR);
        if (ctl_fd == -1) {
            perror("open ctl_fd");
            exit(EXIT_FAILURE);
        }
        this->mrs = std::move(mrs);

        // download program
        ioctl_download download = {};
        download.name = kernel.c_str();
        download.runtime = kernel_time;
        download.runtime_scale = runtime_scale;
        prep_shared_library(kernel_file.c_str(), kernel.c_str(), &download);
        int ret = ioctl(ctl_fd, IOCTL_CEMU_DOWNLOAD, &download);
        if (ret <= 0) {
            perror("load_program ioctl error");
        }
        pind = download.pind;

        // ioctl_download activation = {};
        // activation.pind = pind;
        ret = ioctl(ctl_fd, IOCTL_CEMU_ACTIVATE, &download);
        if (ret) {
            perror("program_activation ioctl error");
        }

        // create memory range sets
        struct ioctl_create_mrs c = {};
        for (auto mr : this->mrs) {
            c.nr_fd = mr->nr_fdm;
            c.fd = mr->fdm_fd;
            c.off = mr->fdm_off;
            c.size = mr->fdm_size;
            int ret = ioctl(ctl_fd, IOCTL_CEMU_CREATE_MRS, &c);
            if (ret) {
                perror("create_mrs ioctl error");
                exit(EXIT_FAILURE);
            }
            mr->rsid = c.rsid;
            if (mr->rsid <= 0) {
                perror("create_mrs ioctl error");
                exit(EXIT_FAILURE);
            }
        }
    }

    ~Task() {
        struct ioctl_create_mrs c = {};
        for (auto mr : this->mrs) {
            c.rsid = mr->rsid;
            int ret = ioctl(ctl_fd, IOCTL_CEMU_DELETE_MRS, &c);
            if (ret) {
                perror("delete_mrs ioctl error");
                exit(EXIT_FAILURE);
            }
        }

        struct ioctl_download download = {};
        download.pind = pind;

        // deactivate program
        int ret = ioctl(ctl_fd, IOCTL_CEMU_DEACTIVATE, &download);
        if (ret)
            perror("ioctl");
        printf("deactivation ret: %d, pind : %d\n", ret, download.pind);

        // unload program
        ret = ioctl(ctl_fd, IOCTL_CEMU_UNLOAD, &download);
        if (ret)
            perror("ioctl");
        printf("remove ret: %d, pind : %d\n", ret, download.pind);
    }

        bool finished() {
            return cur_off >= nvm_off + nvm_size;
        }
};

struct Job {
    int stage;      // 0: input IO, 1: compute, 2: output IO
    bool stage_finished;
    int nvm_fd;
    int nvm_out_fd;
    size_t nvm_off;
    size_t nvm_out_off;
    size_t size;
    size_t finished_size;

    MemoryRangeSet *mr;
    void *data_buf;

    int pind;
    int rsid;
    long long ret;
    long long cparam1;
    long long cparam2;
    uint64_t end_time;

    Task *task;

    std::chrono::high_resolution_clock::time_point start;
    std::chrono::high_resolution_clock::time_point end;

    void init(Task *task, size_t off, size_t size, MemoryRangeSet *mr, size_t out_off = 0) {
        this->task = task;
        nvm_off = off;
        nvm_out_off = out_off;
        this->size = size;
        stage = 0;
        ret = 0;
        cparam1 = task->cparam1;
        cparam2 = task->cparam2;
        nvm_fd = task->nvm_fd;
        nvm_out_fd = task->nvm_out_fd;
        pind = task->pind;
        this->mr = mr;
        rsid = mr->rsid;
        data_buf = aligned_alloc(4096, size);
        finished_size = 0;
    }

    void reset(size_t off, size_t size, size_t out_off = 0) {
        finished_size += this->size;
        this->size = size;
        nvm_off = off;
        nvm_out_off = out_off;
        stage = 0;
        ret = 0;
    }

    bool finished() { return stage == 2 && stage_finished; }

    void setup_io(io_uring_sqe *sqe) {
        switch (stage) {
        case 0:
            io_uring_prep_copy_file_range(sqe, nvm_fd, mr->fdm_fd[0], size, nvm_off, 0);
            // io_uring_prep_read(sqe, mr->fdm_fd[0], data_buf, size, 0);
            // io_uring_prep_read(sqe, nvm_fd, data_buf, size, 0);
            // io_uring_prep_compute(sqe, task->ng3_fd, cparam1, cparam2, pind, rsid, kernel_time);
            break;
        case 1:
            io_uring_prep_compute(sqe, task->ng3_fd, cparam1, cparam2, pind, rsid, kernel_time, group, sched_prio);
            break;
        case 2:
            if (output_mode) {
                // std::cout << "copy from nvm to fdm" << std::endl;
                io_uring_prep_copy_file_range(sqe, mr->fdm_fd[1], nvm_out_fd, output_size, 0, nvm_out_off);
            }
            else
                io_uring_prep_read(sqe, mr->fdm_fd[1], data_buf, output_size, 0);
            break;
        default:
            abort();
        }
        io_uring_sqe_set_data(sqe, this);
        start = std::chrono::high_resolution_clock::now();
    }

    void print_time(int res) {
        switch (stage) {
        case 0:
            std::cout << "Input time: " << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() << "us, res: " << res << std::endl;
            input_times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
            break;
        case 1:
            std::cout << "Compute time: " << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() << "us, res: " << res << std::endl;
            kernel_times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
            break;
        case 2:
            std::cout << "Output time: " << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() << "us, res: " << res << std::endl;
            output_times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
            break;
        default:
            break;
        }
    }
};

static void *thread_main(void *arg) {
    std::cout << "thread start" << std::endl;
    std::vector<Job *> *jobs = (std::vector<Job *> *)arg;
    std::vector<Job*> pending_jobs;
    pending_jobs.reserve(IOU_QUEUE_DEPTH);
    for (auto job : *jobs)
        pending_jobs.push_back(job);
    long end_time = (*jobs)[0]->end_time;

    // setup iouring
    struct io_uring ring;
    int ret = io_uring_queue_init(64, &ring,
        // 0);
        IORING_SETUP_SQE128 | IORING_SETUP_CQE32);
        // IORING_SETUP_IOPOLL | IORING_SETUP_SQPOLL);
    if (ret < 0) {
        perror("io_uring_queue_init");
        exit(EXIT_FAILURE);
    }

    // kernel_times.reserve(file_size / chunk_size);
    // input_times.reserve(file_size / chunk_size);
    // output_times.reserve(file_size / chunk_size);

    while (true) {
        if (end_time && end_time <= std::chrono::high_resolution_clock::now().time_since_epoch().count())
            break;
        // setup IO
        int nr = 0;
        for (auto job : pending_jobs) {
            io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            job->setup_io(sqe);
            nr++;
        }
        pending_jobs.clear();

        // submit IO
        int ret = 0;
        do {
            int ret = io_uring_submit(&ring);
            if (ret < 0) {
                perror("io_uring_submit");
                exit(EXIT_FAILURE);
            }
        } while (ret != 0);

        // reap IO
        io_uring_cqe *cqe;

        while (io_uring_cq_ready(&ring) == 0)
            ;
        ret = io_uring_peek_cqe(&ring, &cqe);

        auto end = std::chrono::high_resolution_clock::now();
        Job *job = (Job *)io_uring_cqe_get_data(cqe);
        // std::cout << "cqe->res: " << cqe->res << std::endl;
        job->stage_finished = true;
        job->end = end;
        if (print_time)
            job->print_time(cqe->res);
        if (!job->finished()) {
            job->stage++;
            pending_jobs.push_back(job);
        } else if (runtime || job->nvm_off + job->size * parallel_chunks < file_size) {
            size_t off = (job->nvm_off + job->size * parallel_chunks) % file_size;
            job->reset(off, job->size, off);
            pending_jobs.push_back(job);
        } else {
            break;
        }
        io_uring_cqe_seen(&ring, cqe);
    }
    free(jobs);
    return nullptr;
}

static void parse_arg(int argc, char **argv) {
    int opt;
    int option_index = 0;
    static struct option long_options[] = {
        {"print_time", no_argument, 0, 'v'},
        {"chunk_size", required_argument, 0, 'c'},
        {"parallel_chunks", required_argument, 0, 'p'},
        {"file", required_argument, 0, 'f'},
        {"size", required_argument, 0, 's'},
        {"drive", required_argument, 0, 'd'},
        {"threads", required_argument, 0, 't'},
        {"output", required_argument, 0, 'o'},
        {"output_ratio", required_argument, 0, 'e'},
        {"bw", required_argument, 0, 'b'},
        {"start_fdm_id", required_argument, 0, 'm'},
        {"start_nvm_id", required_argument, 0, 'a'},
        {"group", required_argument, 0, 'g'},
        {"runtime", required_argument, 0, 'r'},
        {"sched_prio", required_argument, 0, 'i'},
        {"shared_library", required_argument, 0, 'l'},
        {"kernel", required_argument, 0, 'n'},
        {"runtime_scale", required_argument, 0, 'x'},
        {0, 0, 0, 0}
    };
    while ((opt = getopt_long(argc, argv, "vc:p:f:s:d:t:o:e:b:m:a:g:r:i:l:n:x:", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'v':
            print_time = 1;
            break;
        case 'c':
            chunk_size = atoll(optarg) * 1024ULL * 1024ULL;
            break;
        case 'e':
            output_ratio = atof(optarg);
            break;
        case 'p':
            parallel_chunks = atoi(optarg);
            break;
        case 'f':
            datafile = optarg;
            break;
        case 's':
            file_size = atof(optarg) * 1024ULL * 1024ULL * 1024ULL;
            break;
        case 'd':
            split(std::string(optarg), ',', drives);
            break;
        case 't':
            nr_threads = atoi(optarg);
            break;
        case 'o':
            output_mode = atoi(optarg);
            break;
        case 'b':
            bw = atof(optarg);     // kernel bandwidth in GB/s
            break;
        case 'm':
            start_fdm_id = atoi(optarg);
            break;
        case 'a':
            start_nvm_id = atoi(optarg);
            break;
        case 'g':
            group = atoi(optarg);
            break;
        case 'r':
            // runtime = atof(optarg) * 1000000000ULL; // ns
            kernel_time = atof(optarg);
            break;
        case 'i':
            sched_prio = atoi(optarg);
            break;
        case 'l':
            kernel_file = optarg;
            break;
        case 'n':
            kernel = optarg;
            break;
        case 'x':
            runtime_scale = (int)(atof(optarg) * 10.0);
            break;
        default:
            std::cerr <<
"Usage: ./test_parallel_chunks\n\
        [-c chunk_size_in_mb]\n\
        [-p parallel_chunks]\n\
        [-x runtime_scale]\n\
        [-e output_ratio]\n\
        [-o 0/1] 0: output to host, 1: output to NAND\n\
        [-v] profile time of every step\n\
";
            exit(EXIT_FAILURE);
        }
    }
    if (bw) {
        kernel_time = chunk_size / bw;
    } else {
        // kernel_time = DEFAULT_KERNEL_TIME / (1024ULL*1024*16/chunk_size);
        // bw = (double)chunk_size / kernel_time;
    }
    output_size = chunk_size * output_ratio;
    output_size = output_size / 4096 * 4096;
}

int main(int argc, char **argv) {
    parse_arg(argc, argv);
    if (drives.empty())
        drives.push_back(DEFAULT_DRIVE);

    std::string drive_str;
    for (auto drive : drives)
        drive_str += drive + ",";
    drive_str.pop_back();

    std::cout << "INFO: chunk_size      = " << chunk_size / 1024.0 / 1024 << " MiB" << std::endl;
    std::cout << "INFO: parallel_chunks = " << parallel_chunks << std::endl;
    std::cout << "INFO: nr_threads      = " << nr_threads << std::endl;
    std::cout << "INFO: file_size       = " << file_size / 1024.0 / 1024.0 / 1024.0 << " GiB" << std::endl;
    std::cout << "INFO: kernel_time     = " << kernel_time / 1000 << " us" << std::endl;
    std::cout << "INFO: kernel_file     = " << kernel_file << std::endl;
    std::cout << "INFO: kernel          = " << kernel << std::endl;
    std::cout << "INFO: drive           = " << drive_str << std::endl;
    std::cout << "INFO: print_time      = " << print_time << std::endl;
    std::cout << "INFO: datafile        = " << datafile << std::endl;
    std::cout << "INFO: output_mode     = " << (output_mode ? "write back" : "read back") << std::endl;
    std::cout << "INFO: output_size     = " << output_size / 1024 << "KiB" << std::endl;
    std::cout << "INFO: group           = " << group << std::endl;
    std::cout << "INFO: bw              = " << bw << "GB/s" << std::endl;
    std::cout << "INFO: runtime         = " << runtime << std::endl;
    std::cout << "INFO: sched_prio      = " << sched_prio << std::endl;
    std::cout << "INFO: runtime_scale   = " << runtime_scale << std::endl;

    if (parallel_chunks < nr_threads || parallel_chunks % nr_threads != 0) {
        std::cout << "ERROR: parallel_chunks must be a multiple of nr_threads" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (output_mode && (output_size % 512)) {
        std::cerr << "ERROR: output_size must be a multiple of 512" << std::endl;
        exit(EXIT_FAILURE);
    }

    // init task
    std::vector<MemoryRangeSet*> mrs;
    std::vector<Task*> tasks;
    std::vector<int> drive_fdm_nfile(16, start_fdm_id);
    std::vector<Job> jobs(parallel_chunks*drives.size());
    std::vector<std::thread> threads;
    int jobs_per_thread = parallel_chunks / nr_threads;
    uint64_t end_time = runtime ? std::chrono::high_resolution_clock::now().time_since_epoch().count() + runtime : 0;
    for (unsigned i = 0; i < drives.size(); i++) {
        int id = atoi(drives[i].c_str());
        std::string fdm_path = "/mnt/fdm" + drives[i] + "/";
        std::string nvm_path = "/mnt/nvme" + drives[i] + "/" + datafile;
        std::string nvm_out_path = output_mode ? "/mnt/nvme" + drives[i] + "/output" : "";
        for (int i = 0; i < parallel_chunks; i++) {
            std::vector<MemoryRange> mr;
            mr.emplace_back(fdm_path + std::to_string(drive_fdm_nfile[id]++), 0, chunk_size);
            mr.emplace_back(fdm_path + std::to_string(drive_fdm_nfile[id]++), 0, chunk_size);
            mrs.push_back(new MemoryRangeSet(mr));
        }
        Task *task = new Task(nvm_path, nvm_out_path, drives[i], mrs, chunk_size,
                kernel_file, kernel,
                chunk_size / sizeof(int), 0, file_size, 0);

        for (unsigned j = parallel_chunks*i; j < parallel_chunks*(i+1); j++) {
            Job *job = &jobs[j];
            job->init(task, task->cur_off, task->chunk_size, task->mrs[j], task->cur_off);
            task->cur_off += task->chunk_size;
            job->end_time = end_time;
        }

        for (int j = 0; j < nr_threads; j++) {
            std::vector<Job *> *thread_jobs = new std::vector<Job *>();
            for (int k = 0; k < jobs_per_thread; k++)
                thread_jobs->push_back(&jobs[i*parallel_chunks+j*jobs_per_thread+k]);
            threads.emplace_back(thread_main, thread_jobs);
        }
        tasks.push_back(task);
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (auto &thread : threads) {
        thread.join();
    }
    auto end = std::chrono::high_resolution_clock::now();

    std::cout << "Time: " << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() << "us" << std::endl;

    uint64_t finished_size = 0;
    for (auto &job : jobs) {
        finished_size += job.finished_size;
    }
    std::cout << "finished size: " << finished_size << std::endl;
    // calculate bw in GB
    double bw = (double)finished_size / std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::cout << "BW: " << bw << "MB" << std::endl;

    for (auto task : tasks) {
        delete task;
    }

    if (print_time) {
        std::fstream outFile("kernel_times_cemu.txt", std::ios::binary | std::ios::out);
        outFile.write((char *)kernel_times.data(), kernel_times.size() * sizeof(int));
        outFile.close();
        std::cout << "INFO: write " << kernel_times.size() << " kernel time to kernel_times.txt" << std::endl;

        outFile.open("input_times_cemu.txt", std::ios::binary | std::ios::out);
        outFile.write((char *)input_times.data(), input_times.size() * sizeof(int));
        outFile.close();
        std::cout << "INFO: write " << input_times.size() << " kernel time to input_times.txt" << std::endl;

        outFile.open("output_times_cemu.txt", std::ios::binary | std::ios::out);
        outFile.write((char *)output_times.data(), output_times.size() * sizeof(int));
        outFile.close();
        std::cout << "INFO: write " << output_times.size() << " kernel time to output_times.txt" << std::endl;

        uint64_t kernel_times_total = std::accumulate(kernel_times.begin(), kernel_times.end(), 0);
        uint64_t input_times_total = std::accumulate(input_times.begin(), input_times.end(), 0);
        uint64_t output_times_total = std::accumulate(output_times.begin(), output_times.end(), 0);
        std::cout << "Input times total: " << input_times_total << std::endl;
        std::cout << "Kernel times total: " << kernel_times_total << std::endl;
        std::cout << "Output times total: " << output_times_total << std::endl;
        std::cout << "Total: " << input_times_total + kernel_times_total + output_times_total << std::endl;
    }
}