#ifndef __FEMU_NVME_DEF_H
#define __FEMU_NVME_DEF_H

#include <assert.h>
#include <stdint.h>

enum NvmeAdminCommands {
    NVME_ADM_CMD_DELETE_SQ      = 0x00,
    NVME_ADM_CMD_CREATE_SQ      = 0x01,
    NVME_ADM_CMD_GET_LOG_PAGE   = 0x02,
    NVME_ADM_CMD_DELETE_CQ      = 0x04,
    NVME_ADM_CMD_CREATE_CQ      = 0x05,
    NVME_ADM_CMD_IDENTIFY       = 0x06,
    NVME_ADM_CMD_ABORT          = 0x08,
    NVME_ADM_CMD_SET_FEATURES   = 0x09,
    NVME_ADM_CMD_GET_FEATURES   = 0x0a,
    NVME_ADM_CMD_ASYNC_EV_REQ   = 0x0c,
    NVME_ADM_CMD_ACTIVATE_FW    = 0x10,
    NVME_ADM_CMD_DOWNLOAD_FW    = 0x11,
    NVME_ADM_CMD_FORMAT_NVM     = 0x80,
    NVME_ADM_CMD_SECURITY_SEND  = 0x81,
    NVME_ADM_CMD_SECURITY_RECV  = 0x82,
    NVME_ADM_CMD_SET_DB_MEMORY  = 0x7c,
    NVME_ADM_CMD_FEMU_DEBUG     = 0xee,
    NVME_ADM_CMD_FEMU_FLIP      = 0xef,
};

// /* START */ and /* END */ are used in python to extract definitions

/* START */
enum NvmeIoCommands {
    NVME_CMD_FLUSH              = 0x00,
    NVME_CMD_WRITE              = 0x01,
    NVME_CMD_READ               = 0x02,
    NVME_CMD_WRITE_UNCOR        = 0x04,
    NVME_CMD_COMPARE            = 0x05,
    NVME_CMD_WRITE_ZEROES       = 0x08,
    NVME_CMD_DSM                = 0x09,
    NVME_CMD_ZONE_MGMT_SEND     = 0x79,
    NVME_CMD_ZONE_MGMT_RECV     = 0x7a,
    NVME_CMD_ZONE_APPEND        = 0x7d,
    NVME_CMD_OC_ERASE           = 0x90,
    NVME_CMD_OC_WRITE           = 0x91,
    NVME_CMD_OC_READ            = 0x92,
    // CSD commands. FDM: Functional Data Memory
    // TODO: admin command?
    NVME_CMD_CSD_DOWNLOAD       = 0xa1, // Download CSF to CSD
    NVME_CMD_CSD_ALLOC_FDM      = 0xb0, // Allocate FDM
    NVME_CMD_CSD_DEALLOC_AFDM   = 0xc0, // Deallocate FDM
    NVME_CMD_CSD_NVM_TO_AFDM    = 0xd0, // Read data from NVM to FDM
    NVME_CMD_CSD_EXEC           = 0xe0, // Execute CSF on FDM
    NVME_CMD_CSD_READ_AFDM      = 0xf2, // Read data from FDMA
    NVME_CMD_CSD_WRITE_AFDM     = 0xf5, // Write data to FDMA
    NVME_CMD_CSD_CREATE_GROUP   = 0xf6, // Create job group
    NVME_CMD_CSD_SET_QOS        = 0xf7, // Set QoS for job group
    NVME_CMD_CSD_DELETE_GROUP   = 0xf8, // Delete job group
};
/* END */

enum NvmeStatusCodes {
    NVME_SUCCESS                = 0x0000,
    NVME_INVALID_OPCODE         = 0x0001,
    NVME_INVALID_FIELD          = 0x0002,
    NVME_CID_CONFLICT           = 0x0003,
    NVME_DATA_TRAS_ERROR        = 0x0004,
    NVME_POWER_LOSS_ABORT       = 0x0005,
    NVME_INTERNAL_DEV_ERROR     = 0x0006,
    NVME_CMD_ABORT_REQ          = 0x0007,
    NVME_CMD_ABORT_SQ_DEL       = 0x0008,
    NVME_CMD_ABORT_FAILED_FUSE  = 0x0009,
    NVME_CMD_ABORT_MISSING_FUSE = 0x000a,
    NVME_INVALID_NSID           = 0x000b,
    NVME_CMD_SEQ_ERROR          = 0x000c,
    NVME_INVALID_CMD_SET        = 0x002c,
    NVME_LBA_RANGE              = 0x0080,
    NVME_CAP_EXCEEDED           = 0x0081,
    NVME_NS_NOT_READY           = 0x0082,
    NVME_NS_RESV_CONFLICT       = 0x0083,

    // memory namespace
    NVME_NS_NOT_REACHABLE       = 0x0088,

    // compute namespace
    NVME_INSUFFICIENT_RESOURCE  = 0x008a,
    NVME_INVALID_MEMORY_NS      = 0x008b,
    NVME_INVALID_MR_SET         = 0x008c,   // invalid memory range
    NVME_INVALID_MR_SET_ID      = 0x008d,   // invalid memory range set identifier
    NVME_INVALID_PROGRAM_DATA   = 0x008e,
    NVME_INVALID_PIND           = 0x008f,   // invalid program index
    NVME_INVALID_PTYPE          = 0x0090,   // invalid program type
    NVME_MR_EXCEEDED            = 0x0091,   // memory range exceeded
    NVME_MR_SET_EXCEEDED        = 0x0092,   // memory range set exceeded
    NVME_MAX_PROGRAM_ACTIVATED  = 0x0093,
    NVME_PROGRAM_BYTES_EXCEEDED = 0x0094,
    NVME_MR_SET_IN_USE          = 0x0095,
    NVME_NO_PROGRAM             = 0x0096,
    NVME_OVERLAP_MR             = 0x0097,
    NVME_PROGRAM_NOT_ACTIVATED  = 0x0098,
    NVME_PROGRAM_IN_USE         = 0x0099,
    NVME_PIND_NOT_DOWNLOADABLE  = 0x009a,
    NVME_PROGRAM_TOO_BIG        = 0x009b,

    NVME_INVALID_CQID           = 0x0100,
    NVME_INVALID_QID            = 0x0101,
    NVME_MAX_QSIZE_EXCEEDED     = 0x0102,
    NVME_ACL_EXCEEDED           = 0x0103,
    NVME_RESERVED               = 0x0104,
    NVME_AER_LIMIT_EXCEEDED     = 0x0105,
    NVME_INVALID_FW_SLOT        = 0x0106,
    NVME_INVALID_FW_IMAGE       = 0x0107,
    NVME_INVALID_IRQ_VECTOR     = 0x0108,
    NVME_INVALID_LOG_ID         = 0x0109,
    NVME_INVALID_FORMAT         = 0x010a,
    NVME_FW_REQ_RESET           = 0x010b,
    NVME_INVALID_QUEUE_DEL      = 0x010c,
    NVME_FID_NOT_SAVEABLE       = 0x010d,
    NVME_FID_NOT_NSID_SPEC      = 0x010f,
    NVME_FW_REQ_SUSYSTEM_RESET  = 0x0110,
    NVME_CONFLICTING_ATTRS      = 0x0180,
    NVME_INVALID_PROT_INFO      = 0x0181,
    NVME_WRITE_TO_RO            = 0x0182,
    NVME_ZONE_BOUNDARY_ERROR    = 0x01b8,
    NVME_ZONE_FULL              = 0x01b9,
    NVME_ZONE_READ_ONLY         = 0x01ba,
    NVME_ZONE_OFFLINE           = 0x01bb,
    NVME_ZONE_INVALID_WRITE     = 0x01bc,
    NVME_ZONE_TOO_MANY_ACTIVE   = 0x01bd,
    NVME_ZONE_TOO_MANY_OPEN     = 0x01be,
    NVME_ZONE_INVAL_TRANSITION  = 0x01bf,
    NVME_INVALID_MEMORY_ADDRESS = 0x01C0,
    NVME_WRITE_FAULT            = 0x0280,
    NVME_UNRECOVERED_READ       = 0x0281,
    NVME_E2E_GUARD_ERROR        = 0x0282,
    NVME_E2E_APP_ERROR          = 0x0283,
    NVME_E2E_REF_ERROR          = 0x0284,
    NVME_CMP_FAILURE            = 0x0285,
    NVME_ACCESS_DENIED          = 0x0286,
    NVME_DULB                   = 0x0287,
    NVME_MORE                   = 0x2000,
    NVME_DNR                    = 0x4000,
    NVME_NO_COMPLETE            = 0xffff,
};

typedef struct NvmeRwCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2;
    uint64_t    mptr;
    uint64_t    prp1;
    uint64_t    prp2;
    uint64_t    slba;
    uint16_t    nlb;
    uint16_t    control;
    uint32_t    dsmgmt;
    uint32_t    reftag;
    uint16_t    apptag;
    uint16_t    appmask;
} NvmeRwCmd;
static_assert(sizeof(NvmeRwCmd) == 64, "NvmeRwCmd size mismatch");

// CSD extension

enum {
    NVME_CSD_CSF_TYPE_PHANTOM,
    NVME_CSD_CSF_TYPE_EBPF,
    NVME_CSD_CSF_TYPE_BITSTREAM,
    NVME_CSD_CSF_TYPE_SHARED_LIB,
    NVME_CSD_CSF_TYPE_INVALID,
};

enum {
    NVME_CSD_FDM_TYPE_HOST,
    NVME_CSD_FDM_TYPE_INVALID,
};

typedef struct NvmeCsdDownloadCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;           // bytecode str
    uint64_t    prp2;
    uint64_t    size;           // program size in bytes
    uint8_t     csf_type;       // 0: ebpf, 1: bitstream
    uint8_t     jit       : 1;  // 1 if jit
    uint8_t     rsvd_ctrl : 7;
    uint16_t    runtime_scale;
    uint32_t    runtime;        // runtime in ns when type is phantom
    uint32_t    rsvd15;
} NvmeCsdDownloadCmd;
static_assert(sizeof(NvmeCsdDownloadCmd) == 64, "NvmeCsdDownloadCmd");

typedef struct NvmeCsdAllocFdmCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;
    uint64_t    prp2;
    uint64_t    size;   // fdm size in bytes
    uint8_t     type;   // emulated fdm type, currently support host memory
    uint8_t     rsvd14[3];
    uint64_t    rsvd15;
} NvmeCsdAllocFdmCmd;
static_assert(sizeof(NvmeCsdAllocFdmCmd) == 64, "NvmeCsdAllocFdmCmd");

typedef struct NvmeCsdDeallocAfdmCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;
    uint64_t    prp2;
    uint32_t    id;     // afdm id
    uint32_t    rsvd11[5];
} NvmeCsdDeallocAfdmCmd;
static_assert(sizeof(NvmeCsdDeallocAfdmCmd) == 64, "NvmeCsdDeallocAfdmCmd");

typedef struct NvmeCsdNvmToAfdmCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;
    uint64_t    prp2;
    uint64_t    slba;
    uint16_t    nlb;
    uint16_t    rsvd12;
    uint32_t    id;     // afdm id
    uint64_t    offset; // offset in afdm
} NvmeCsdNvmToAfdmCmd;
static_assert(sizeof(NvmeCsdNvmToAfdmCmd) == 64, "NvmeCsdNvmToAfdmCmd");

typedef struct NvmeCsdExecCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;
    uint64_t    prp2;
    uint32_t    csf_id;         // csf id
    uint32_t    in_afdm_id;     // input afdm id
    uint32_t    out_afdm_id;    // output afdm id
    uint32_t    group;          // job group id
    uint32_t    rsvd14;
    uint32_t    runtime;        // runtime in ns. 0 means actual running
} NvmeCsdExecCmd;
static_assert(sizeof(NvmeCsdExecCmd) == 64, "NvmeCsdExecCmd");

typedef struct NvmeCsdReadAfdmCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;
    uint64_t    prp2;
    uint64_t    offset;       // offset in afdm
    uint64_t    size;         // read size afdm
    uint32_t    id;           // afdm id
    uint32_t    rsvd15;
} NvmeCsdReadAfdmCmd;
static_assert(sizeof(NvmeCsdReadAfdmCmd) == 64, "NvmeCsdReadAfdmCmd");

typedef struct NvmeCsdWriteAfdmCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;   // write data
    uint64_t    prp2;
    uint64_t    offset; // offset in afdm
    uint64_t    size;   // write size
    uint32_t    id;     // afdm id
    uint32_t    rsvd15;
} NvmeCsdWriteAfdmCmd;
static_assert(sizeof(NvmeCsdWriteAfdmCmd) == 64, "NvmeCsdWriteAfdmCmd");

typedef struct NvmeCsdCreateGroupCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;
    uint64_t    prp2;
    int8_t      prio;       // priority, signed integer. TODO: nvme report max/min prio
    uint8_t     prefer_bandwidth : 1;
    uint8_t     prefer_deadline  : 1;
    uint8_t     _pad             : 6;
    uint16_t     rsvd10;
    uint32_t    bandwidth;  // bandwidth in KB/s. TODO: nvme report max bandwidth
    uint32_t    deadline;   // response time in us. TODO: nvme report granularity
    uint32_t    rsvd14[2];
} NvmeCsdCreateGroupCmd;
static_assert(sizeof(NvmeCsdCreateGroupCmd) == 64, "NvmeCsdCreateGroup");

typedef struct NvmeCsdSetQoSCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;
    uint64_t    prp2;
    int8_t      prio;
    uint8_t     prefer_bandwidth : 1;
    uint8_t     prefer_deadline  : 1;
    uint8_t     _pad             : 6;
    uint16_t     rsvd10;
    uint32_t    bandwidth;
    uint32_t    deadline;
    uint32_t    id;         // job group id
    uint32_t    rsvd15;
} NvmeCsdSetQoSCmd;
static_assert(sizeof(NvmeCsdSetQoSCmd) == 64, "NvmeCsdSetQoS");

typedef struct NvmeCsdDeleteGroupCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint64_t    rsvd2[2];
    uint64_t    prp1;
    uint64_t    prp2;
    uint32_t    id;     // job group id
    uint32_t    rsvd11[5];
} NvmeCsdDeleteGroupCmd;
static_assert(sizeof(NvmeCsdDeleteGroupCmd) == 64, "NvmeCsdDeleteGroup");

/*************** Memory Namespace ***************/

// source range entry of copy descriptor format 2
// currently it is not documented in the NVMe NVM spec
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
static_assert(sizeof(NvmeCopyFormat2) == 32, "NvmeCopyFormat2");

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
static_assert(sizeof(NvmeCopyFormat4) == 32, "NvmeCopyFormat4");

typedef union NvmeCopyFormat {
    NvmeCopyFormat2 cf2;
    NvmeCopyFormat4 cf4;
} NvmeCopyFormat;
static_assert(sizeof(NvmeCopyFormat) == 32, "NvmeCopyFormat");

/*************** Compute Namespace ***************/

enum NvmeComputeIoCommands {
    NVME_CMD_COMPUTE_EXEC           = 0x01,
    INDIRECT_THREAD_FINISH          = 0x88,
};

// memory range descriptor
typedef struct NvmeMemoryRange {
    uint32_t nsid;      // namespace id
    uint32_t len;       // length
    uint64_t sb;        // starting bytes
    uint64_t rsvd[2];
} NvmeMemoryRange;
static_assert(sizeof(NvmeMemoryRange) == 32, "NvmeMemoryRange");

typedef struct NvmeProgramExecuteCmd {
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
    uint32_t    group     : 8;  // group
    uint32_t    chunk_nlb : 24; // indirect chunk nlb
    uint32_t    runtime;        // CEMU specific
} NvmeProgramExecuteCmd;
static_assert(sizeof(NvmeProgramExecuteCmd) == 64, "NvmeProgramExecuteCmd");

/* Admin Commands */
enum NvmeComputeAdminCommands {
    NVME_CMD_COMPUTE_MRS_MGMT   = 0x21,
    NVME_CMD_COMPUTE_LOAD       = 0x22,
    NVME_CMD_COMPUTE_ACTIVATE   = 0x23,
};

typedef struct NvmeMemoryRangeSetManageCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint32_t    rsvd[4];
    uint64_t    prp1;           // data buffer
    uint64_t    prp2;
	uint16_t    sel    : 4;     // type of management operation
	uint16_t    rsvd10 : 12;
	uint16_t    rsid;           // memory range set identifier
	uint8_t     numr;           // number of memory ranges
	uint8_t     rsvd11a;        // number of memory ranges
	uint16_t    rsvd11b;
    uint32_t    rsvd12[4];
} NvmeMemoryRangeSetManageCmd;
static_assert(sizeof(NvmeMemoryRangeSetManageCmd) == 64, "NvmeMemoryRangeSetManageCmd");

typedef struct NvmeLoadProgramCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint16_t    jit       : 1;  // 1 if jit, CEMU only
    uint16_t    rsvd_ctrl : 15;
    uint16_t    runtime_scale;  // CEMU only
    uint32_t    runtime;        // runtime in ns when type is phantom, CEMU only
    uint32_t    rsvd4[2];
    uint64_t    prp1;           // program buffer
    uint64_t    prp2;
    uint16_t    pind;           // program index
    uint8_t     ptype;          // program type
    uint8_t     sel      : 1;   // 0 for load 1 for unload
    uint8_t     pit      : 3;   // program identifier type, 0 is not used, 1 is PUID (program unique identifier)
    uint8_t     indirect : 1;
    uint8_t     rsvd10   : 3;
    uint32_t    psize;          // total program size in bytes
    uint64_t    pid;            // program identifier, PUID when pit is 1
    uint32_t    numb;           // number of bytes to transfer
    uint32_t    loff;           // load offset
} NvmeLoadProgramCmd;
static_assert(sizeof(NvmeLoadProgramCmd) == 64, "NvmeLoadProgramCmd");

typedef struct NvmeProgramActivationCmd {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    cid;
    uint32_t    nsid;
    uint32_t    rsvd[4];
    uint64_t    prp1;           // program buffer
    uint64_t    prp2;
    uint32_t    pind   : 16;
    uint32_t    sel    : 4;     // 0 for deactivate, 1 for activate
    uint32_t    rsvd10 : 12;
    uint32_t    runtime;        // runtime in ns when type is phantom, CEMU only
    uint32_t    rsvd12[4];
} NvmeProgramActivationCmd;
static_assert(sizeof(NvmeProgramActivationCmd) == 64, "NvmeProgramActivationCmd");

struct sre_iter {
    NvmeCopyFormat2 *sres;
    int nr_sres;
    int pos;
    int done;
    int nlb;
};

// indirect task
typedef struct IndirectTask {
    int chunk_id;             // chunk id
    int nr_concurrent_chunks; // number of concurrent chunks
    int destination;          // 0: output in FDM; 1: output in NVM
    int chunk_nlb;            // nlb per chunk
    int stage;                // 0: input; 1: compute; 2: output
    int nr_total_input_cf2;   // sum(nr_input_cf2)
    int nr_total_output_cf2;  // sum(nr_output_cf2)
    int *nr_input_cf2;        // [nr_concurrent_chunks]
    int *nr_output_cf2;       // [nr_concurrent_chunks]
    int *nr_output_nlb;       // [nr_concurrent_chunks]
    int nr_total_nlb;         // sum(chunk_nlb)
    int nr_total_output_nlb;
    struct sre_iter *iter_in;  // [nr_concurrent_chunks]
    struct sre_iter *iter_out; // [nr_concurrent_chunks]
    char *raw_data_buffer;     // data buffer in nvme command

    int *nr_finished_nlb;
    int *nr_finished_output_nlb;
    int *iter_finished;

    struct rte_ring     *ring;    // message ring between indirect thread and ftl/compute thread
    NvmeCopyFormat      *sres;    // sres buffer
} IndirectTask;

#endif // __FEMU_NVME_DEF_H