#ifndef __FEMU_MEM_BACKEND
#define __FEMU_MEM_BACKEND

#include <stdint.h>
#include <stdbool.h>
#include "qemu/osdep.h"

typedef struct QEMUSGList QEMUSGList;

typedef enum BackendType{
    FEMU_DRAM_BACKEND,
} BackendType;

/* SSD Backend */
typedef struct SsdBackend {
    void        *logical_space;
    int64_t     size; /* in bytes */
    int         femu_mode;
    BackendType type;
} SsdBackend;

int init_dram_backend(SsdBackend *b);
void free_dram_backend(SsdBackend *b);

int init_backend(SsdBackend **b, BackendType type, char *path, int64_t nbytes);
void free_backend(SsdBackend *b);

int backend_rw(SsdBackend *b, QEMUSGList *qsg, uint64_t *lbal, bool is_write);
void backend_rw_internal(SsdBackend *b, void *buf, uint64_t data_offset,
                           uint64_t data_size, int is_write);
void *backend_get_ptr(SsdBackend *b, uint64_t offset);
void backend_copy_internal(SsdBackend *b, uint64_t doff, uint64_t soff,
                           uint64_t data_size);
static inline void *backend_addr(SsdBackend *b, uint64_t offset)
{
    return b->logical_space + offset;
}
static inline void backend_fill(SsdBackend *b, uint64_t offset, uint64_t len)
{
    memset(backend_addr(b, offset), 0, len);
}

#endif
