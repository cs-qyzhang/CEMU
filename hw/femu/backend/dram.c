#include "../nvme.h"
#include <stdlib.h>

/* Coperd: FEMU Memory Backend (mbe) for emulated SSD */

int init_dram_backend(SsdBackend *b)
{
    // b->logical_space = mmap(NULL, b->size, PROT_READ | PROT_WRITE,
    //                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_HUGETLB | MAP_POPULATE, -1, 0);
    b->logical_space = aligned_alloc(4096, b->size);
    if (b->logical_space == NULL || b->logical_space == (void *)-1) {
        femu_err("Failed to allocate memory for ssd backend!\n");
        abort();
    }

    // if (mlock(b->logical_space, b->size) == -1) {
    //     femu_err("Failed to pin the memory backend to the host DRAM\n");
    // }

    return 0;
}

void free_dram_backend(SsdBackend *b)
{
    if (b->logical_space) {
        // munmap(b->logical_space, b->size);
        free(b->logical_space);
        // munlock(b->logical_space, b->size);
    }
}
