#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cemu_def.h"

long long vadd(struct cemu_args *args);
long long vadd_indirect(struct cemu_args *args);

/*
 * For direct usage model, mr[0] is output, mr[1] is input.
 */
long long vadd(struct cemu_args *args) {
    int numr = args->numr;
    void **mr_addr = args->mr_addr;
    long long *mr_len = args->mr_len;
    long long cparam1 = args->cparam1;
    (void)mr_len;
    (void)args->cparam2;
    (void)args->data_buffer;
    (void)args->buffer_len;

    assert(numr == 2);

    long long size = cparam1;
    int *a = ((int **)mr_addr)[1];
    int *b = a + 1;
    int *out = ((int **)mr_addr)[0];

    for (int i = 0; i < size; i++) {
        out[i] = a[i*2] + b[i*2];
    }
    return size;
}

/*
 * For indirect usage model, mr[0] is output, mr[1] is input,
 * other mrs are fixed between CSF executions of the indirect task.
 * The return value is the number of write back blocks (512B).
 */
long long vadd_indirect(struct cemu_args *args) {
    int numr = args->numr;
    void **mr_addr = args->mr_addr;
    long long *mr_len = args->mr_len;
    long long cparam1 = args->cparam1;
    (void)mr_len;
    (void)args->cparam2;
    (void)args->data_buffer;
    (void)args->buffer_len;

    assert(numr == 3);

    int *output = ((int **)mr_addr)[0];
    int *input = ((int **)mr_addr)[1];
    int *global_mem = ((int **)mr_addr)[2];

    // extract data
    long long size = cparam1;
    int *a = input;
    int *b = a + 1;
    int pos = global_mem[0];
    int start_loc = global_mem[1];

    if (start_loc > 0 && pos > 0) {
        // data before start_loc has been written to the output file
        // move data left to the start of the buffer
        memcpy(output, input + start_loc, (pos - start_loc) * sizeof(int));
        pos -= start_loc;
        start_loc = 0;
    }

    for (int i = 0; i < size; i++) {
        output[pos++] = a[i*2] + b[i*2];
    }

    // update global memory
    int full_blocks = pos / (512 / sizeof(int));
    if (full_blocks > 0) {
        global_mem[1] = full_blocks * (512 / sizeof(int));
    }
    global_mem[0] = pos;

    // for indirect usage model, the return value is the number of write back blocks
    return full_blocks;
}