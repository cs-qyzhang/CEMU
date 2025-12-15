#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <lz4.h>
#include "cemu_def.h"

// Function to read a file into a buffer
std::vector<char> readFile(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + filePath);
    }

    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(fileSize);
    file.read(buffer.data(), fileSize);
    return buffer;
}

// Function to compress a chunk of data using LZ4
void compressChunk(const char* input, long long in_size, char *output, long long *out_size) {
    int maxCompressedSize = LZ4_compressBound(in_size);

    int compressedSize = LZ4_compress_default(input, output, in_size, maxCompressedSize);
    if (compressedSize <= 0) {
        std::cerr << "Compression failed" << std::endl;
    }

    *out_size = compressedSize;
}

extern "C" {
long long lz4(struct cemu_args *args);
}

long long lz4(struct cemu_args *args)
{
    int numr = args->numr;
    void **mr_addr = args->mr_addr;
    long long *mr_len = args->mr_len;
    (void)args->cparam1;
    (void)args->cparam2;
    (void)args->data_buffer;
    (void)args->buffer_len;

    assert(numr == 2);
    long long size = mr_len[0];
    char *in = (char *)mr_addr[0];
    char *out = (char *)mr_addr[1];

    const int nr_threads = 6;
    long long out_size[nr_threads];
    long long thread_chunk_size = size / nr_threads;

    // Launch threads to compress chunks
    std::vector<std::thread> threads;
    for (size_t i = 0; i < nr_threads; ++i) {
        threads.emplace_back(compressChunk, in + i * thread_chunk_size, thread_chunk_size, out + i * thread_chunk_size, &out_size[i]);
    }

    // Join all threads
    for (auto& thread : threads) {
        thread.join();
    }
    size = 0;
    for (int i = 0; i < nr_threads; i++) {
        size += out_size[i];
    }
    // printf("Compressed size: %lld\n", size);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input_file> <output_file>" << std::endl;
        return 1;
    }

    std::string inputFilePath = argv[1];
    std::string outputFilePath = argv[2];

    try {
        // Read the input file
        std::vector<char> inputData = readFile(inputFilePath);
        size_t inputSize = inputData.size();
        char *output = new char[inputSize];
        void *mr_addr[2] = {inputData.data(), output};

        struct cemu_args args = {
            .numr = 2,
            .mr_addr = mr_addr,
            .mr_len = NULL,
            .cparam1 = static_cast<long long>(inputSize),
            .cparam2 = 0,
            .data_buffer = NULL,
            .buffer_len = 0,
        };
        long long out_size = lz4(&args);

        // Write compressed data to the output file
        std::ofstream outputFile(outputFilePath, std::ios::binary);
        if (!outputFile) {
            throw std::runtime_error("Failed to open output file: " + outputFilePath);
        }
        outputFile.write(output, out_size);
        std::cout << "Compression completed successfully." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
