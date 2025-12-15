#ifndef CEMU_DEF_H
#define CEMU_DEF_H

struct cemu_args {
    int numr;
    void **mr_addr;
    long long *mr_len;
    long long cparam1;
    long long cparam2;
    void *data_buffer;
    long long buffer_len;
} __attribute__((packed));

// re-impl assert
#ifdef assert
#  undef assert
#endif

#ifdef NDEBUG
#  define assert(expr) ((void)0)
#else
#  define assert(expr)                                                     \
    do {                                                                   \
      if (!(expr)) {                                                       \
        fprintf(stderr,                                                    \
                "ASSERT FAILED: %s\n  at %s:%d (%s)\n",                     \
                #expr, __FILE__, __LINE__, __func__);                      \
      }                                                                    \
    } while (0)
#endif

#endif