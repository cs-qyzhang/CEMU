#ifndef __FEMU_NVME_CSD_H
#define __FEMU_NVME_CSD_H

#include <stdint.h>

struct MemoryRange;
struct MemoryRangeSet;
struct ComputeNamespace;
struct MemoryNamespace;

struct Program;

// subsystem local memory io commands
struct NvmeMemoryRead;
struct NvmeMemoryWrite;
struct NvmeMemoryFill;
struct NvmeMemoryCopy;

// computational programs admin commands
struct NvmeCmdLoadProgram;
struct NvmeProgramActivation;
struct NvmeMemoryRangeSetManagement;

// computational programs io commands
struct NvmeExecuteProgram;

// computational programs log page commands
struct NvmeProgramList;
struct NvmeDownloadableProgramTypesList;
struct NvmeMemoryRangeSetList;
struct NvmeReachabilityGroups;
struct NvmeReachabilityAssociations;

// program identifier type
typedef uint64_t nvme_pit_t;
// program type
typedef uint64_t nvme_ptype_t;
// program index
typedef uint64_t nvme_pind_t;
// program identifier
typedef uint64_t nvme_pid_t;
// program size
typedef uint64_t nvme_psize_t;
// load offset
typedef uint64_t nvme_loff_t;
// number of bytes
typedef uint64_t nvme_numb_t;

#endif // __FEMU_NVME_CSD_H