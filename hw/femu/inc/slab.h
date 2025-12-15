#ifndef __FEMU_SLAB_H
#define __FEMU_SLAB_H

#include <pthread.h>

typedef struct Slab {
    void *mem;
    void **free_list;
    int nr_free;
    int elem_size;
    pthread_spinlock_t lock;
} Slab;

void slab_init(Slab *s, int elem_size, int nr_elem,
               void (*init)(void *elem, void *arg), void *init_arg);
void *slab_alloc(Slab *s, int lock);
void slab_free(Slab *s, void *data, int lock);
static inline void *slab_at(Slab *s, int index)
{
    return (void *)((char *)s->mem + index * s->elem_size);
}

#endif // __FEMU_SLAB_H