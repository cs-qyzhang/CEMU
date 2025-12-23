// https://klyr.github.io/posts/playing_with_ubpf/
// clang -target bpf -I/usr/include/x86_64-linux-gnu -O2 -g -c vadd.bpf.c -o vadd.bpf.o

#include "cemu_def.h"

long long vadd(struct cemu_args *args) {
    int numr = args->numr;
    void **mr_addr = args->mr_addr;
    long long *mr_len = args->mr_len;
    long long cparam1 = args->cparam1;
    long long cparam2 = args->cparam2;
    void *data_buffer = args->data_buffer;
    long long buffer_len = args->buffer_len;

    long long size = cparam1;
    int *a = ((int **)mr_addr)[1];
    int *b = a + 1;
    int *out = ((int **)mr_addr)[0];

    for (int i = 0; i < size; i++) {
        out[i] = a[i*2] + b[i*2];
    }
    return size;
}