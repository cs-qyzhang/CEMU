#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <cmath>
#include <queue>
#include <algorithm>
#include <cstdint>
#include "cemu_def.h"

struct Node {
    char tag[64];
    char vector[4096];
};

std::mutex output_mutex;

// Function to calculate the squared Euclidean distance between two vectors
int calculateDistance(const int* v1, const char* v2) {
    int distance = 0;
    for (size_t i = 0; i < 4096; ++i) {
        int diff = v1[i] - (v2[i] - '0');
        distance += static_cast<int>(diff * diff);
    }
    return distance;
}

// Worker function to process a subset of nodes
void processChunk(const Node* nodes, const int* query, size_t start, size_t end, int* distances) {
    for (size_t i = start; i < end; ++i) {
        distances[i] = calculateDistance(query, nodes[i].vector);
    }
}

// KNN multithread function
void knn_multithread(char *input, long long input_size, int *output, int *query, size_t num_threads) {
    // Launch threads to calculate distances
    std::vector<std::thread> threads;
    size_t nr_vector = input_size / sizeof(Node);
    size_t chunk_size = nr_vector / num_threads;

    for (size_t t = 0; t < num_threads; ++t) {
        size_t start = t * chunk_size;
        size_t end = std::min(start + chunk_size , nr_vector);
        threads.emplace_back(processChunk, (Node *)(void *)input, query, start, end, &output[start]);
    }

    // Join threads
    for (auto& thread : threads) {
        thread.join();
    }
}

extern "C" {
long long knn(struct cemu_args *args);
}

long long knn(struct cemu_args *args)
{
    (void)args->numr;
    void **mr_addr = args->mr_addr;
    long long *mr_len = args->mr_len;
    (void)args->cparam1;
    (void)args->cparam2;
    (void)args->data_buffer;
    (void)args->buffer_len;

    size_t num_threads = 2;
    int query[4096];
    knn_multithread((char *)mr_addr[0], mr_len[0], (int *)mr_addr[1], query, num_threads);
    return 0;
}

// // Main function
// int main(int argc, char* argv[]) {
//     if (argc != 4) {
//         std::cerr << "Usage: " << argv[0] << " <input_file> <query_file> <output_file>\n";
//         return 1;
//     }

//     try {
//         knn_multithread(argv[1], argv[2], argv[3]);
//         std::cout << "Distance calculation completed and saved to " << argv[3] << "\n";
//     } catch (const std::exception& e) {
//         std::cerr << e.what() << "\n";
//         return 1;
//     }

//     return 0;
// }
