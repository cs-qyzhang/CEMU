#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include "hw/femu/inc/slab.h"

void slab_init(Slab *s, int elem_size, int nr_elem,
               void (*init)(void *elem, void *arg), void *init_arg)
{
    s->elem_size = elem_size;
    s->nr_free = nr_elem;
    s->mem = malloc(elem_size * nr_elem);
    assert(s->mem != NULL);
    s->free_list = malloc(sizeof(void *) * nr_elem);
    assert(s->free_list != NULL);
    for (int i = 0; i < nr_elem; i++) {
        void *elem = s->mem + i * elem_size;
        // allocation order is the same of mem order
        s->free_list[nr_elem - 1 - i] = elem;
        if (init)
            init(elem, init_arg);
    }
    pthread_spin_init(&s->lock, PTHREAD_PROCESS_PRIVATE);
}

void *slab_alloc(Slab *s, int lock)
{
    if (lock) {
        pthread_spin_lock(&s->lock);
        if (s->nr_free == 0) {
            pthread_spin_unlock(&s->lock);
            return NULL;
        }
        void *data = s->free_list[--s->nr_free];
        pthread_spin_unlock(&s->lock);
        return data;
    } else {
        if (s->nr_free == 0)
            return NULL;
        return s->free_list[--s->nr_free];
    }
}

void slab_free(Slab *s, void *data, int lock)
{
    if (lock) {
        pthread_spin_lock(&s->lock);
        s->free_list[s->nr_free++] = data;
        pthread_spin_unlock(&s->lock);
    } else {
        s->free_list[s->nr_free++] = data;
    }
}
