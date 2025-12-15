#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <cstring>
#include <cstdio>
#include "cemu_def.h"

// Mutex to protect the result vector
std::mutex result_mutex;

// Function to search for the pattern in a range of rows
void grep_worker(const char* data, int rows, int cols, const char* pattern, int pattern_length, int start_row, int end_row, long long *out_size) {
    (void)rows;
    int cnt = 0;
    for (int r = start_row; r < end_row; ++r) {
        const char* line = data + r * cols;
        for (int c = 0; c <= cols - pattern_length; ++c) {
            if (*(long long *)&line[c] != *(long long*)pattern) {
                continue;
            }
            if (std::strncmp(line + c, pattern, pattern_length) == 0) {
                cnt++;
            }
        }
    }
    *out_size = cnt * 8;
}

// Multithreaded grep function
long long multithreaded_grep(const char* data, int rows, int cols, const char* pattern, int thread_count) {
    int pattern_length = std::strlen(pattern);
    std::vector<std::thread> threads;

    // Calculate rows per thread
    int rows_per_thread = rows / thread_count;
    int remainder = rows % thread_count;

    // std::cout << "Rows per thread: " << rows_per_thread << ", rows " << rows << ", thread_count " << thread_count << ", cols " << cols << std::endl;

    int start_row = 0;
    long long *out_size = new long long[thread_count];
    for (int t = 0; t < thread_count; ++t) {
        int end_row = start_row + rows_per_thread + (t < remainder ? 1 : 0);
        threads.emplace_back(grep_worker, data, rows, cols, pattern, pattern_length, start_row, end_row, &out_size[t]);
        start_row = end_row;
    }

    // Wait for all threads to finish
    for (auto& thread : threads) {
        thread.join();
    }

    // Copy results to the output array
    long long out_size_sum = 0;
    for (int i = 0; i < thread_count; i++) {
        out_size_sum += out_size[i];
    }
    // std::cout << "out_size_sum: " << out_size_sum << std::endl;
    return out_size_sum;
}

extern "C" {
long long grep(struct cemu_args *args);
}

long long grep(struct cemu_args *args)
{
    (void)args->numr;
    void **mr_addr = args->mr_addr;
    long long *mr_len = args->mr_len;
    (void)args->cparam1;
    (void)args->cparam2;
    (void)args->data_buffer;
    (void)args->buffer_len;

    const int nr_thread = 9;
    long long cols = 1024;
    long long rows = mr_len[0] / cols;
    // std::cout << "rows: " << rows << ", cols: " << cols << std::endl;
    const char *pattern = "AABACCDCACBDACDBDDBAADAAB";
    int ret = multithreaded_grep((const char*)mr_addr[0], rows, cols, pattern, nr_thread);
    // std::cout << "grep ret: " << ret << std::endl;
    return ret;
}

int main() {
    // Example input data
    const int rows = 10;
    const int cols = 21;
    const char data[rows][cols] = {
        "SFFGELKAHGLKSDHLGFKJ",
        "SFFGELKAHGLKSDHLGFKJ",
        "SFFGELKAHGLKSDHLGFKJ",
        "SFFGELKAHGLKSDHLGFKJ",
        "SFFGELKAHGLKSDHLGFKJ",
        "SFFGELKAHGLKSDHLGFKJ",
        "SFFGELKAHGLKSDHLGFKJ",
        "SFFGELKAHGLKSDHLGFKJ",
        "SFFGELKAHGLKSDHLGFKJ",
        "SFFGELKAHGLKSDHLKAHG",
    };

    const char* pattern = "KAHG";

    // Call the grep function
    void *mr_addr[2];
    mr_addr[0] = (void*)data;
    mr_addr[1] = (void*)pattern;
    struct cemu_args args = {
        .numr = 2,
        .mr_addr = mr_addr,
        .mr_len = nullptr,
        .cparam1 = rows,
        .cparam2 = cols,
        .data_buffer = nullptr,
        .buffer_len = 0,
    };
    long long out_size = grep(&args);
    std::cout << "Found " << out_size / 8 << " occurrences of the pattern." << std::endl;

    return 0;
}
