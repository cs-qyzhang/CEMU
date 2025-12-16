# Developing CSD Applications

This guide explains how to develop Computational Storage Functions (CSFs) for CEMU. We'll cover how to write CSFs as both shared libraries and eBPF programs, and how to integrate them with your applications.

## Overview

A CSF (Computational Storage Function) is a program that executes on the CSD device. CEMU supports two types of CSFs:

1. **Shared Library CSFs** (`.so` files): Native code functions compiled as shared libraries
2. **eBPF CSFs** (`.bpf.o` files): eBPF programs compiled to bytecode

## CSF Interface

All CSFs must follow a standard interface defined in `tests/cemu/kernel/cemu_def.h`:

```c
struct cemu_args {
    int numr;                    // number of memory ranges
    void **mr_addr;              // memory range addresses
    long long *mr_len;           // memory range lengths
    long long cparam1;           // parameter 1
    long long cparam2;           // parameter 2
    void *data_buffer;           // additional data buffer
    long long buffer_len;        // data buffer length
};

// CSF function signature
long long your_csf_function(struct cemu_args *args);
```

The CSF function returns the number of output bytes or blocks processed.

## Developing Shared Library CSFs

### Step 1: Write Your CSF Function

Create a source file (e.g., `my_csf.c` or `my_csf.cc`) with your CSF implementation:

```c
#include "cemu_def.h"

long long my_csf(struct cemu_args *args) {
    int numr = args->numr;
    void **mr_addr = args->mr_addr;
    long long *mr_len = args->mr_len;
    
    // Access input memory range (typically mr_addr[1])
    char *input = (char *)mr_addr[1];
    long long input_size = mr_len[1];
    
    // Access output memory range (typically mr_addr[0])
    char *output = (char *)mr_addr[0];
    long long output_size = mr_len[0];
    
    // Your computation here
    for (long long i = 0; i < input_size; i++) {
        output[i] = process(input[i]);
    }
    
    return output_size;  // Return number of bytes processed
}
```

### Step 2: Compile as Shared Library

For C code:
```bash
gcc -shared -fPIC -O2 -o my_csf.so my_csf.c
```

For C++ code:
```bash
g++ -shared -fPIC -O2 -std=c++11 -o my_csf.so my_csf.cc
```

If you need to link external libraries:
```bash
g++ -shared -fPIC -O2 -std=c++11 -o my_csf.so my_csf.cc -llz4 -L/usr/lib/x86_64-linux-gnu -llz4.a
```

### Example: LZ4 Compression CSF

See `tests/cemu/kernel/lz4.cc` for a complete example:

```cpp
extern "C" {
long long lz4(struct cemu_args *args);
}

long long lz4(struct cemu_args *args) {
    int numr = args->numr;
    void **mr_addr = args->mr_addr;
    long long *mr_len = args->mr_len;
    
    assert(numr == 2);
    long long size = mr_len[0];
    char *in = (char *)mr_addr[0];
    char *out = (char *)mr_addr[1];
    
    // Compress using LZ4 library
    int compressedSize = LZ4_compress_default(in, out, size, LZ4_compressBound(size));
    
    return compressedSize;
}
```

## Developing eBPF CSFs

### Step 1: Write Your eBPF Program

Create a `.bpf.c` file (e.g., `my_csf.bpf.c`):

```c
#include "cemu_def.h"

long long my_csf(struct cemu_args *args) {
    int numr = args->numr;
    void **mr_addr = args->mr_addr;
    long long *mr_len = args->mr_len;
    long long cparam1 = args->cparam1;
    
    long long size = cparam1;
    int *a = ((int **)mr_addr)[0];
    int *b = &((int **)mr_addr)[0][size];
    int *out = ((int **)mr_addr)[1];
    
    for (long long i = 0; i < size; i++) {
        out[i] = a[i] + b[i];
    }
    
    return size;
}
```

**Note**: eBPF has limitations (no loops with variable bounds, limited library functions, etc.). For complex operations, use shared libraries instead.

### Step 2: Compile eBPF Program

```bash
clang -target bpf -static -O2 -g -c -o my_csf.bpf.o my_csf.bpf.c
```

You need `clang` with BPF target support installed.

## Using CSFs in Your Application

### Step 1: Include Required Headers

```c
#include "util.h"
#include "cemu_ioctl.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
```

### Step 2: Open the Compute Namespace Device

```c
int ctl_fd = open("/dev/nvme0c3", O_RDWR);  // Compute namespace
if (ctl_fd == -1) {
    perror("open dev file");
    exit(1);
}
```

### Step 3: Download the CSF

For shared library:
```c
struct ioctl_download download = {0};
prep_shared_library("./build/my_csf.so", "my_csf", &download);

int ret = ioctl(ctl_fd, IOCTL_CEMU_DOWNLOAD, &download);
if (ret < 0) {
    perror("download CSF");
    exit(1);
}
printf("CSF downloaded, pind: %d\n", download.pind);
```

For eBPF:
```c
struct ioctl_download download = {0};
bool use_jit = true;  // Enable JIT compilation for better performance
prep_ebpf("./build/my_csf.bpf.o", "my_csf", use_jit, &download);

int ret = ioctl(ctl_fd, IOCTL_CEMU_DOWNLOAD, &download);
```

### Step 4: Activate the CSF

```c
ret = ioctl(ctl_fd, IOCTL_CEMU_ACTIVATE, &download);
if (ret < 0) {
    perror("activate CSF");
    exit(1);
}
```

### Step 5: Create Memory Range Set (Optional)

If your CSF needs to access device memory (FDM - Functional Data Memory):

```c
struct ioctl_create_mrs mrs;
prep_mrs(&mrs, 2);  // 2 memory ranges

// Open FDMFS files
char buf[64];
sprintf(buf, "/mnt/fdm0/input");
mrs.fd[0] = open(buf, O_RDWR);
sprintf(buf, "/mnt/fdm0/output");
mrs.fd[1] = open(buf, O_RDWR);

// Set offsets and sizes
mrs.off[0] = 0;
mrs.size[0] = input_size;
mrs.off[1] = 0;
mrs.size[1] = output_size;

ret = ioctl(ctl_fd, IOCTL_CEMU_CREATE_MRS, &mrs);
if (ret < 0) {
    perror("create MRS");
    exit(1);
}
printf("MRS created, rsid: %d\n", mrs.rsid);
```

### Step 6: Execute the CSF

#### Using ioctl (Synchronous)

```c
struct ioctl_execute exec = {0};
exec.pind = download.pind;
exec.rsid = mrs.rsid;  // Use MRS, or set to 0 if using direct memory
exec.nr_fd = 0;        // Not needed if using MRS
exec.cparam1 = input_size;
exec.cparam2 = 0;

ret = ioctl(ctl_fd, IOCTL_CEMU_EXECUTE, &exec);
if (ret < 0) {
    perror("execute CSF");
    exit(1);
}
```

#### Using io_uring (Asynchronous)

For better performance with multiple concurrent executions:

```c
#include "iouring.h"

// Initialize io_uring
struct io_uring ring;
io_uring_queue_init(IOU_QUEUE_DEPTH, &ring, 0);

// Prepare NVMe command
struct nvme_uring_cmd cmd = {0};
void *data = NULL;  // Optional data buffer
uint32_t data_len = 0;

prep_nvme_uring_program_execute(
    &cmd,
    cparam1,           // parameter 1
    cparam2,           // parameter 2
    download.pind,     // program index
    mrs.rsid,          // memory range set ID
    0,                 // numr (0 when using MRS)
    0,                 // group ID
    0,                 // chunk_nlb (for indirect model)
    0,                 // user_runtime
    data,              // data buffer
    data_len           // data buffer length
);

// Submit command
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_cmd(sqe, ctl_fd, &cmd);
io_uring_submit(&ring);

// Wait for completion
struct io_uring_cqe *cqe;
io_uring_wait_cqe(&ring, &cqe);
int result = cqe->res;
io_uring_cqe_seen(&ring, cqe);
```

### Complete Example: vadd_example.cpp

See `tests/cemu/vadd_example.cpp` for a complete working example that demonstrates:
- Downloading both shared library and eBPF CSFs
- Creating memory range sets
- Executing CSFs with different execution models

## Direct vs Indirect Usage Models

### Direct Model

In the direct model, memory ranges are explicitly provided with each execution. This is suitable when data locations vary between executions.

```c
// Memory ranges are specified directly in the execute command
// mr_addr[0]: output
// mr_addr[1], mr_addr[2], ...: inputs
```

### Indirect Model

In the indirect model, the CSF processes data from the NVM namespace using logical block addresses. The system handles the mapping automatically.

```c
// The CSF receives chunk information and processes LBAs
// This is more efficient for processing large files
long long my_csf_indirect(struct cemu_args *args) {
    // Process data from NVM namespace
    // args->cparam1 contains chunk size
    // args->data_buffer contains LBA information
    // Return number of blocks to write back
}
```

See `tests/cemu/kernel/vadd.c` for examples of both models.

## Building Your Application

### Link Against libcemu

Your application needs to link against the CEMU utility library:

```makefile
# In your Makefile
$(BUILD_DIR)/my_app: my_app.cpp $(BUILD_DIR)/libcemu.a
	$(CXX) $(CXXFLAGS) -o $@ $^ -luring
```

The `libcemu.a` library provides helper functions like `prep_shared_library()`, `prep_ebpf()`, `prep_mrs()`, etc.

### Compile CSF Kernels

Add CSF compilation to your build system:

```makefile
# Shared library CSF
$(BUILD_DIR)/my_csf.so: kernel/my_csf.cc
	$(CXX) $(CXXFLAGS) -shared -fPIC -o $@ $<

# eBPF CSF
$(BUILD_DIR)/my_csf.bpf.o: kernel/my_csf.bpf.c
	clang -target bpf -static -O2 -g -c -o $@ $<
```

See `tests/cemu/kernel/Makefile` for reference.

## Best Practices

1. **Error Handling**: Always check return values from ioctl calls and handle errors appropriately

2. **Memory Alignment**: When using O_DIRECT file I/O, ensure buffers are aligned to 4096 bytes:
   ```c
   void *buf = aligned_alloc(4096, size);
   ```

3. **Concurrent Execution**: Use io_uring for better performance with multiple concurrent CSF executions

4. **Memory Range Sets**: Create MRS once and reuse them for multiple executions to reduce overhead

5. **Program State**: Remember to activate programs after downloading, and deactivate/unload when done

6. **Performance Tuning**: 
   - Use JIT compilation for eBPF programs (`use_jit = true`)
   - Specify appropriate runtime estimates for better scheduling
   - Use job groups for QoS control in multi-tenant scenarios

## Testing Your CSF

1. **Unit Testing**: Test your CSF function directly with test data:
   ```c
   struct cemu_args args = {
       .numr = 2,
       .mr_addr = {output_buffer, input_buffer},
       .mr_len = {output_size, input_size},
       .cparam1 = size,
   };
   long long result = my_csf(&args);
   ```

2. **Integration Testing**: Use the benchmark framework (`cemu_benchmark.cpp`) to test with realistic workloads

3. **Performance Analysis**: Monitor execution times and adjust runtime estimates for accurate scheduling

## Example Applications

CEMU includes several example CSFs in `tests/cemu/kernel/`:

- **vadd**: Vector addition (direct and indirect models)
- **lz4**: LZ4 compression
- **grep**: Pattern matching
- **knn**: K-nearest neighbors computation
- **sql**: SQL-like filtering operations

Study these examples to understand different usage patterns and implementation techniques.

