#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <string>
#include <cstring>
#include <cstdlib>
#include "cemu_def.h"

// 定义记录的长度
const int RECORD_LENGTH = 32;

// 互斥锁用于保护结果容器
std::mutex result_mutex;
std::vector<const char *> results;

// 查询函数
static void query_records(const char *data, size_t start, size_t end, int year_lower, int year_upper, char *output, long long *out_size) {
    long long output_size = 0;
    for (size_t i = start; i < end; i += RECORD_LENGTH) {
        const char *record = data + i;
        // 提取年份 (31-32 字节)
        int year = ((record[30] - '0') << 8) | (unsigned char)(record[31] - '0');

        // 检查年份是否在范围内
        if (year >= year_lower && year <= year_upper) {
            // 将符合的记录保存到结果中
            memcpy(output + output_size, record, RECORD_LENGTH);
            output_size += RECORD_LENGTH;
        }
    }
    *out_size = output_size;
}

static long long multi_thread_query(const char *data, size_t data_size, char *output, int year_lower, int year_upper, size_t num_threads) {
    // 计算每个线程处理的记录数量
    size_t records_per_thread = data_size / RECORD_LENGTH / num_threads;
    std::vector<std::thread> threads;
    long long *out_size = new long long[num_threads];

    // 创建线程
    for (size_t i = 0; i < num_threads; ++i) {
        size_t start = i * records_per_thread * RECORD_LENGTH;
        size_t end = (i == num_threads - 1) ? data_size : start + records_per_thread * RECORD_LENGTH;

        threads.emplace_back(query_records, data, start, end, year_lower, year_upper, output + start, &out_size[i]);
    }

    // 等待所有线程完成
    for (auto &thread : threads) {
        thread.join();
    }

    // 输出结果
    long long output_size = 0;
    for (size_t i = 0; i < num_threads; i++) {
        output_size += out_size[i];
    }
    // std::cout << "Output size: " << output_size << std::endl;

    return output_size;
}

extern "C" {
long long sql(struct cemu_args *args);
}

long long sql(struct cemu_args *args)
{
    (void)args->numr;
    void **mr_addr = args->mr_addr;
    long long *mr_len = args->mr_len;
    (void)args->cparam1;
    (void)args->cparam2;
    (void)args->data_buffer;
    (void)args->buffer_len;

    size_t num_threads = 2;
    int year_lower = 50;
    int year_upper = 60;
    long long ret = multi_thread_query((const char *)mr_addr[0], mr_len[0], (char *)mr_addr[1], year_lower, year_upper, num_threads);
    return ret;
}

int main() {
    // 示例输入数据
    const char *raw_data = "round1    player1    75 01012023" // 示例数据
                          "round2    player2    80 01012021";
    size_t data_size = std::strlen(raw_data);

    // 查询范围
    int year_lower = 20;
    int year_upper = 30;

    // 线程数量
    const int num_threads = 4;
    (void)data_size;
    (void)year_lower;
    (void)year_upper;
    (void)num_threads;

    // multi_thread_query(const char *data, int data_size, int year_lower, int year_upper, int num_threads)

    return 0;
}
