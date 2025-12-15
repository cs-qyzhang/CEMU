#ifndef __TEST_CEMU_H
#define __TEST_CEMU_H

#include <stdint.h>
#include <stdbool.h>
#include <linux/nvme_ioctl.h>

#include "cemu_ioctl.h"

#define LBA_SIZE 	512 // logical block size, DON'T CHANGE THIS

/*
 * Compatibility: older distro headers may not ship io_uring NVMe UAPI yet.
 * In real environments where these are available, this block is bypassed.
 */
#ifndef NVME_URING_CMD_IO
#define NVME_URING_CMD_IO 0
struct nvme_uring_cmd {
	__u8	opcode;
	__u8	flags;
	__u16	rsvd1;
	__u32	nsid;
	__u32	cdw2;
	__u32	cdw3;
	__u64	metadata;
	__u64	addr;
	__u32	metadata_len;
	__u32	data_len;
	__u32	cdw10;
	__u32	cdw11;
	__u32	cdw12;
	__u32	cdw13;
	__u32	cdw14;
	__u32	cdw15;
	__u32	timeout_ms;
	__u32	result;
};
#endif

enum {
    PROGRAM_TYPE_PHANTOM,
    PROGRAM_TYPE_EBPF,
    PROGRAM_TYPE_SHARED_LIB,
    PROGRAM_TYPE_BITSTREAM,
    PROGRAM_TYPE_INVALID,
};

typedef struct NvmeCopyFormat2 {
    uint32_t snsid;         // source namespace id
    uint32_t rsvd;
    uint64_t slba;
    uint32_t rsvd2;
    uint16_t nlb;
    uint16_t rsvd3 : 15;
    uint16_t fco   : 1;     // fast copy only
    uint64_t rsvd4;
} NvmeCopyFormat2;

// source range entry of copy descriptor format 4, see NVMe SLM spec
typedef struct NvmeCopyFormat4 {
    uint32_t snsid;         // source namespace id
    uint32_t rsvd;
    uint64_t saddr;         // starting source address
    uint32_t nbyte;
    uint16_t rsvd2;
    uint16_t rsvd3 : 15;
    uint16_t fco   : 1;     // fast copy only
    uint64_t rsvd4;
} NvmeCopyFormat4;

typedef union NvmeCopyFormat {
    NvmeCopyFormat2 cf2;
    NvmeCopyFormat4 cf4;
} NvmeCopyFormat;

struct nvme_program_execute_cmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint16_t    pind;           // program index
    uint16_t    rsid;           // memory range set id
    uint32_t    numr;           // number of memory ranges
    uint32_t    dlen;           // data buffer len
    uint32_t    rsvd;
    uint64_t    prp1;           // data buffer
    uint64_t    prp2;
    uint64_t    cparam1;        // parameter data
    uint64_t    cparam2;        // parameter data
    uint32_t    group     : 8;  // CSF group id
    uint32_t    chunk_nlb : 24; // indirect chunk nlb
    uint32_t    user_runtime;
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get file mappings for NVM file and split them evenly into parallel chunks
 * @param filename: NVM file name
 * @param filesize: file size
 * @param cf2: copy format 2 array
 * @param parallel_chunks: number of parallel chunks
 * @param nr_nlb_per_chunk: inout. number of logical blocks per chunk. if all zeros, use as output, otherwise use as input.
 * @param nr_cf2_per_chunk: output. number of copy format 2 entries per chunk
 * @return: number of total copy format 2 entries, equal to sum(nr_cf2_per_chunk)
 */
extern int get_file_mappings(const char *filename, uint64_t *filesize, NvmeCopyFormat2 **cf2, int parallel_chunks, int *nlb_per_chunk, int *nr_cf2_per_chunk);

/**
 * Prepare shared library for program execution
 * @param file: shared library file name
 * @param kernel: kernel file name
 * @param download: download structure
 */
extern void prep_shared_library(const char *file, const char *kernel, struct ioctl_download *download);

/**
 * Prepare eBPF program for program execution
 * @param file: eBPF program file name
 * @param kernel: kernel file name
 * @param jit: whether to use JIT
 * @param download: download structure
 */
extern void prep_ebpf(const char *file, const char *kernel, bool jit, struct ioctl_download *download);

/**
 * Prepare memory range set for program execution
 * @param mrs: memory range set structure
 * @param nr_fd: number of file descriptors
 */
extern void prep_mrs(struct ioctl_create_mrs *mrs, int nr_fd);

/**
 * Prepare an NVMe ioctl passthru command buffer for "program execute".
 *
 * Note:
 * - The NVMe command payload is written into the same memory as `struct nvme_passthru_cmd`
 *   (first 64 bytes), using `struct nvme_program_execute_cmd` layout.
 * - If (data != NULL && data_len > 0), both the passthru cmd `addr`/`data_len`
 *   and the execute cmd `dlen` are set.
 */
extern void prep_nvme_passthru_program_execute(struct nvme_passthru_cmd *cmd,
                                              uint64_t cparam1,
                                              uint64_t cparam2,
                                              uint16_t pind,
                                              uint16_t rsid,
                                              uint32_t numr,
                                              uint32_t group,
                                              uint32_t chunk_nlb,
                                              uint32_t user_runtime,
                                              void *data,
                                              uint32_t data_len);

/**
 * Prepare an NVMe io_uring passthru command buffer for "program execute".
 *
 * Note:
 * - The NVMe command payload is written into the same memory as `struct nvme_uring_cmd`
 *   (first 64 bytes), using `struct nvme_program_execute_cmd` layout.
 * - If (data != NULL && data_len > 0), both the nvme_uring_cmd `addr`/`data_len`
 *   and the execute cmd `dlen` are set.
 */
extern void prep_nvme_uring_program_execute(struct nvme_uring_cmd *cmd,
                                           uint64_t cparam1,
                                           uint64_t cparam2,
                                           uint16_t pind,
                                           uint16_t rsid,
                                           uint32_t numr,
                                           uint32_t group,
                                           uint32_t chunk_nlb,
                                           uint32_t user_runtime,
                                           void *data,
                                           uint32_t data_len);

#ifdef __cplusplus
}
#endif

#endif // __TEST_CEMU_H