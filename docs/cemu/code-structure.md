# CEMU Code Structure

This document provides an overview of the CEMU code structure, focusing on the CSD (Computational Storage Drive) implementation in the `hw/femu/csd/` directory. CEMU is built on top of FEMU (Flash Emulator), extending it with computational storage capabilities.

## Overview

CEMU extends FEMU by adding CSD-specific functionality while maintaining compatibility with the existing FEMU infrastructure. The CSD implementation consists of several key modules that work together to provide computational storage capabilities.

## Directory Structure

The CSD implementation is located in `hw/femu/csd/` and consists of the following core files:

- **compute.c/compute.h**: Core computation engine and CSF (Computational Storage Function) management
- **sched.c/sched.h**: Job scheduling system for CSD compute units
- **memory.c/memory.h**: Device memory namespace management (FDM - Functional Data Memory)
- **nvme_csd.h**: NVMe CSD command definitions and data structures

## Core Modules

### 1. Computational Storage Functions (CSF)

The CSF module (`compute.c`) is responsible for managing computational programs that can be executed on the CSD.

#### Program Types

CEMU supports multiple program types defined in `compute.h`:

- **PROGRAM_TYPE_EBPF**: eBPF programs compiled to bytecode and executed via uBPF
- **PROGRAM_TYPE_SHARED_LIB**: Shared library (.so) files containing native code functions
- **PROGRAM_TYPE_BITSTREAM**: FPGA bitstreams (reserved for future use)
- **PROGRAM_TYPE_PHANTOM**: Placeholder for testing

#### Program Lifecycle

Programs go through the following states:

1. **PROGRAM_STATE_INVALID**: Uninitialized state
2. **PROGRAM_STATE_LOADING**: Program data is being transferred
3. **PROGRAM_STATE_LOADED**: Program is loaded but not yet activated
4. **PROGRAM_STATE_ACTIVATED**: Program is ready for execution

#### Key Data Structures

```c
typedef struct Program {
    uint64_t pid;               // program unique identifier
    uint32_t nsid;
    uint16_t pind;              // program index
    uint8_t state;              // enum ProgramState
    uint8_t type;               // enum ProgramType
    void *code;                 // program data
    uint32_t size;              // program size
    bool is_indirect;           // direct or indirect usage model
    // ... runtime statistics and execution context
} Program;
```

#### Program Loading

- **Shared Libraries**: Programs are loaded via `dlopen()` and function symbols are resolved using `dlsym()`
- **eBPF Programs**: Programs are loaded into a uBPF VM and optionally JIT-compiled for better performance

### 2. NVMe CSD Commands

CEMU implements the NVMe Computational Storage specification commands for managing and executing CSFs:

#### Admin Commands

- **Load Program** (`program_load`): Downloads a CSF to the device
- **Unload Program** (`program_unload`): Removes a CSF from the device
- **Program Activation** (`program_activation`): Activates or deactivates a loaded program
- **Memory Range Set Management** (`memory_range_set_management`): Creates or deletes memory range sets

#### I/O Commands

- **Execute Program** (`program_execute`): Executes an activated program with specified parameters
- **Memory Read/Write**: Direct access to device memory
- **Memory Copy**: Copy operations within device memory
- **Memory Fill**: Fill memory regions with specified patterns

### 3. Memory Range Sets (MRS)

Memory Range Sets provide a mechanism to define and manage collections of memory regions across different namespaces. This is crucial for the indirect usage model where programs need to access data from multiple sources.

```c
typedef struct MemoryRangeSet {
    MemoryRange *mr;
    uint16_t rsid;
    uint16_t numr;              // number of memory ranges
    void **mr_addr;             // mapped addresses
    long long *mr_len;          // range lengths
    int in_use;
    pthread_spinlock_t lock;
} MemoryRangeSet;
```

### 4. Scheduling System

The scheduling module (`sched.c`) implements a flexible job scheduling system for managing compute jobs across multiple compute units (CUs).

#### Compute Units

Compute units are logical processing cores within the CSD. The system supports multiple CUs that can execute jobs concurrently.

```c
typedef struct ComputeUnit {
    int type;
    int id;
    uint64_t next_sched_time;
    ComputeJob *job_running;
    void *sched_info;           // scheduler private data
    uint64_t time_slice;
    uint64_t context_switch_time;
} ComputeUnit;
```

#### Compute Jobs

Jobs represent individual CSF execution requests:

```c
typedef struct ComputeJob {
    ubpf_jit_fn jit_fn;         // compiled function pointer
    void *in;
    void *out;
    uint64_t in_size;
    uint64_t out_size;
    int csf_id;
    struct NvmeRequest *req;
    uint32_t runtime;
    struct Program *program;
    struct ComputeEngine *ce;
} ComputeJob;
```

#### Scheduler Interface

CEMU provides a pluggable scheduler interface that allows different scheduling algorithms:

```c
typedef struct CsfScheduler {
    void *(*init)(ComputeEngine *ce);
    void (*fini)(void *private);
    ComputeJob *(*pick_next_job)(ComputeUnit *cu, ...);
    int (*enqueue_job)(ComputeUnit *cu, ComputeJob *job, ...);
    void (*dequeue_job)(ComputeUnit *cu, ComputeJob *job, ...);
    void (*switched_from)(ComputeUnit *cu, ComputeJob *job, ...);
    int (*pick_thread)(ComputeEngine *ce, ComputeJob *job, ...);
} CsfScheduler;
```

#### Built-in Schedulers

CEMU includes several scheduler implementations:

- **FIFO Scheduler** (`sched_fifo.c`): First-In-First-Out scheduling
- **Round-Robin Scheduler** (`sched_rr.c`): Round-robin scheduling with time slicing
- **Grouped FIFO Scheduler** (`sched_grouped_fifo.c`): FIFO with job grouping support
- **Grouped Round-Robin Scheduler** (`sched_grouped_rr.c`): Round-robin with job grouping

#### Job Groups

Job groups allow applications to organize related jobs and apply QoS policies:

```c
typedef struct JobGroup {
    uint64_t start_time;
    uint64_t total_runtime;
    JobList jobs;
    int prio;                   // priority (1-9)
    uint32_t bandwidth;         // bandwidth in KB/s
    uint32_t deadline;          // response time in us
    int id;
    int jobs_total;
    int jobs_finished;
    int jobs_running;
} JobGroup;
```

### 5. Device Memory Namespace

The memory namespace (`memory.c`) manages the Functional Data Memory (FDM) that provides high-speed memory for computational storage operations. This memory can be accessed via:

- Direct memory read/write commands
- Memory range sets for structured access
- File-based access via FDMFS (Functional Data Memory File System)

### 6. Execution Models

CEMU supports two execution models for CSFs:

#### Direct Usage Model

In the direct model, the host provides all input/output memory ranges explicitly with each execution command. This is suitable for applications where data locations vary between executions.

#### Indirect Usage Model

In the indirect model, programs can access data from the NVM namespace using logical block addresses (LBAs). The program is provided with a chunk size, and the system handles the mapping between LBAs and device memory. This model is more efficient for processing large files stored in the NVM namespace.

## Integration with FEMU

CEMU extends FEMU by:

1. Adding a new device mode (`femu_mode=4`) for CSD functionality
2. Extending the namespace structure to support compute and memory namespaces
3. Adding NVMe command handlers for CSD-specific commands
4. Implementing compute threads that execute CSFs in parallel with I/O operations

## Configuration Parameters

Key configuration parameters for CSD operation:

- `fdm_size`: Total Functional Data Memory size in MB
- `csf_runtime_scale`: Scaling factor for CSF execution time simulation
- `nr_cu`: Number of compute units
- `nr_thread`: Number of threads for functional simulation
- `time_slice`: Default time slice for scheduler (nanoseconds)
- `context_switch_time`: Context switch overhead (nanoseconds)

## Statistics and Monitoring

CEMU includes comprehensive statistics collection for:

- Program execution times
- Job scheduling metrics
- Compute unit utilization
- Memory access patterns

These statistics can be exported via message queues for real-time analysis in Python scripts.

## Threading Model

CEMU uses multiple threads for different purposes:

- **Compute Threads**: Execute CSFs using functional simulation
- **Scheduler Thread**: Manages job scheduling across compute units
- **I/O Threads**: Handle NVMe command processing (from FEMU)
- **Main Thread**: Coordinates overall system operation

This multi-threaded architecture enables concurrent execution of I/O operations and computational tasks, providing realistic performance modeling for computational storage devices.

