#ifndef __FEMU_MEMORY_H
#define __FEMU_MEMORY_H

#include <assert.h>
#include <stdint.h>

enum NvmeMemoryIoCommands {
    NVME_CMD_MEMORY_FILL  = 0x00,
    NVME_CMD_MEMORY_COPY  = 0x01,
    NVME_CMD_MEMORY_READ  = 0x02,
    NVME_CMD_MEMORY_WRITE = 0x05,
};

typedef struct NvmeMemoryCopyCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    len;            // length
    uint64_t    rsvd4;
    uint64_t    prp1;           // source range entries
    uint64_t    prp2;
    uint64_t    sdaddr;         // starting destination address
    uint32_t    nr     : 8;     // number of ranges
    uint32_t    cdft   : 4;     // copy descriptor format type
    uint32_t    rsvd12 : 20;
    uint32_t    rsvd13[3];
} NvmeMemoryCopyCmd;
static_assert(sizeof(NvmeMemoryCopyCmd) == 64, "NvmeMemoryCopyCmd");

typedef struct NvmeMemoryFillCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd[4];
    uint64_t    sb;             // starting bytes
    uint32_t    fl;             // fill length
    uint32_t    rsvd13[3];
} NvmeMemoryFillCmd;
static_assert(sizeof(NvmeMemoryFillCmd) == 64, "NvmeMemoryFillCmd");

typedef struct NvmeMemoryReadCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;
    uint64_t    prp2;
    uint64_t    sb;             // starting byte
    uint32_t    rl;             // read length
    uint32_t    rsvd13[3];
} NvmeMemoryReadCmd;
static_assert(sizeof(NvmeMemoryReadCmd) == 64, "NvmeMemoryReadCmd");

typedef struct NvmeMemoryWriteCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;
    uint64_t    prp2;
    uint64_t    sb;             // starting byte
    uint32_t    wl;             // write length
    uint32_t    rsvd13[3];
} NvmeMemoryWriteCmd;
static_assert(sizeof(NvmeMemoryWriteCmd) == 64, "NvmeMemoryWriteCmd");

enum MemoryType {
    MEMORY_DRAM,
};

typedef struct MemoryNamespace {
    enum MemoryType type;
} MemoryNamespace;

typedef struct FemuCtrl FemuCtrl;
void *get_mem_ctrl(uint64_t sdaddr, uint64_t len, FemuCtrl **n, uint16_t *ret);

#endif // __FEMU_MEMORY_H