#include "./backend.h"
#include "../nvme.h"

int init_backend(SsdBackend **mbe, BackendType type, char *path, int64_t nbytes)
{
    SsdBackend *b = *mbe = g_malloc0(sizeof(SsdBackend));
    if (b == NULL) {
        femu_err("Failed to allocate memory for ssd backend!\n");
        return -1;
    }

    b->type = type;
    b->size = nbytes;

    switch(b->type) {
    case FEMU_DRAM_BACKEND:
        return init_dram_backend(b);
    default:
        femu_err("Unknown backend type!\n");
        abort();
        return -1;
    }
}

void free_backend(SsdBackend *b)
{
    switch(b->type) {
    case FEMU_DRAM_BACKEND:
        free_dram_backend(b);
        break;
    default:
        femu_err("Unknown backend type!\n");
    }
    g_free(b);
}

int backend_rw(SsdBackend *b, QEMUSGList *qsg, uint64_t *lbal, bool is_write)
{
    int sg_cur_index = 0;
    dma_addr_t sg_cur_byte = 0;
    dma_addr_t cur_addr, cur_len;
    uint64_t mb_oft = lbal[0];
    void *mb = b->logical_space;

    DMADirection dir = DMA_DIRECTION_FROM_DEVICE;

    if (is_write) {
        dir = DMA_DIRECTION_TO_DEVICE;
    }

    while (sg_cur_index < qsg->nsg) {
        cur_addr = qsg->sg[sg_cur_index].base + sg_cur_byte;
        cur_len = qsg->sg[sg_cur_index].len - sg_cur_byte;
        if (dma_memory_rw(qsg->as, cur_addr, mb + mb_oft, cur_len, dir, MEMTXATTRS_UNSPECIFIED)) {
            femu_err("dma_memory_rw error\n");
        }

        sg_cur_byte += cur_len;
        if (sg_cur_byte == qsg->sg[sg_cur_index].len) {
            sg_cur_byte = 0;
            ++sg_cur_index;
        }

        if (b->femu_mode == FEMU_OCSSD_MODE) {
            mb_oft = lbal[sg_cur_index];
        } else if (b->femu_mode == FEMU_BBSSD_MODE ||
                   b->femu_mode == FEMU_NOSSD_MODE ||
                   b->femu_mode == FEMU_ZNSSD_MODE ||
                   b->femu_mode == FEMU_CSD_MODE) {
            mb_oft += cur_len;
        } else {
            assert(0);
        }
    }

    qemu_sglist_destroy(qsg);

    return 0;
}

void backend_rw_internal(SsdBackend *b, void *buf, uint64_t data_offset,
                         uint64_t data_size, int is_write)
{
    const uint64_t copy_threshold = 16UL * 1024UL;
    const uint64_t page_size = 4096;
    const uint64_t page_mask = page_size - 1;

    void *src, *dest;
    if (is_write) {
        src = buf;
        dest = b->logical_space + data_offset;
    } else {
        src = b->logical_space + data_offset;
        dest = buf;
    }
    if (data_size <= copy_threshold) {
        memmove(dest, src, data_size);
    } else {
        uint64_t remain = data_size;
        if (data_offset & page_mask) {
            uint64_t sz = page_size - (data_offset & page_mask);
            memmove(dest, src, sz);
            remain -= sz;
            dest += sz;
            src += sz;
        }
        while (remain > 0) {
            uint64_t sz = remain > copy_threshold ? copy_threshold : remain;
            memmove(dest, src, sz);
            remain -= sz;
            dest += sz;
            src += sz;
        }
    }
}

void *backend_get_ptr(SsdBackend *b, uint64_t offset)
{
    return b->logical_space + offset;
}

void backend_copy_internal(SsdBackend *b, uint64_t doff, uint64_t soff,
                           uint64_t data_size)
{
    void *dest = b->logical_space + doff;
    backend_rw_internal(b, dest, soff, data_size, 0);
}