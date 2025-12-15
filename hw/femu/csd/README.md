## Dependencies

- [libcjson](https://github.com/DaveGamble/cJSON). Used to store statistics in json.
- [libpmem](https://pmem.io/pmdk/). Optional, see [Persistent Memory Backend](#persistent-memory-backend).
- [libnng](https://github.com/nanomsg/nng). Optional, see [Meson Options](#meson-options).
- [CFFI](https://cffi.readthedocs.io/en/latest/). Optional, see [Statistics and Analysis in Python](#statistics-and-analysis-in-python)

## QEMU command

`femu_mode=4` means using CSD extensions.

```shell
sudo ./x86_64-softmmu/qemu-system-x86_64 \
    -name "FEMU-CSD-DQIB" \
    -cpu host \
    -m 8G \
    -smp 16 \
    -enable-kvm \
    -kernel ./dqib_amd64-pc/kernel \
    -initrd ./dqib_amd64-pc/initrd \
    -drive file=./dqib_amd64-pc/image.qcow2 \
    -device femu,devsz_mb=$((16*1024)),femu_mode=4,fdm_size=$((2*1024)) \
    -net user,hostfwd=tcp::2222-:22 \
    -net nic,model=virtio \
    -append "root=LABEL=rootfs console=ttyS0" \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log
```

1. Compile FEMU, see top level README
2. Download [DQIB](https://people.debian.org/~gio/dqib/) of amd64-pc, rename downloaded file to `dqib.zip`, unzip it to `build-femu`
3. Copy `femu-scripts/run-csd.sh` to `build-femu`
4. `cd build-femu`
5. `./run-csd.sh`
6. Create another terminal, run `ssh -p 2222 root@localhost`, username `root`, password `root`
7. `lsblk` will show `nvme0n1`, which is our CSD drive

### CSD parameters

- `fdm_size`: total functional data memory size in MB.
- `csf_runtim_scale`: csf timing simulation scale, default value is 3.
- `nr_cu`: number of compute unit, default value is 4.
- `nr_thread`: number of thread for functional simulation, default value is 4.
- `time_slice`: time slice for scheduler interface, default value is 200000 ns.
- `context_switch_time`: time of one context switch for scheduler interface, default value is 200 ns.
- `pmem_file`: if you have pmem and want to use it to act the backend of fdm, you can set this param with the path to your pmem file, more details in [Persistent Memory Backend](#persistent-memory-backend).

See `femu-scripts/run-csd.sh` and [FEMU 代码分析](https://jianyue.tech/posts/femu/).

## Examples

`tests/femu` contains some examples to run applications atop FEMU CSD. The CSF (Computational Storage Function) needs to be written as eBPF program, see `tests/femu/double.bpf.c`, which is a simple CSF doing double. To compile eBPF, you need `clang` (see `tests/femu/Makefile`):

```bash
clang -target bpf -O2 -g -c xxx.bpf.c -o xxx.bpf.o
```

`tests/femu/csd_helper.c` wrapped lower level NVMe commands, which is send using io_uring NVMe passthru, see [使用 io_uring 进行 NVMe 命令的异步透传](https://jianyue.tech/posts/io_uring-passthrough/). Since NVMe passthru feature of io_uring is only avaliable since Linux v5.19, so it's better to compile examples inside FEMU.

You can use QEMU virtio 9pfs to share folder between host and FEMU, see [共享文件](https://jianyue.tech/posts/femu/#%E5%85%B1%E4%BA%AB%E6%96%87%E4%BB%B6).After FEMU is started, goto `tests/femu/build` folder, run `make -C ..`.

## Statistics and Analysis in Python

To have a powerful and convenient way to do statistics and analysis, we use a message queue (shared memory) to send real-time statistics to python, so we can do real-time analysis in python (see `analysis.py`).

You need to run `analysis.py` first if you want do this.

## CSF Runtime Simulation

Two methods:

1. Scale. CSF runtime is the runtime on host times scale factor (set via FEMU propety `csf_runtime_scale`, default is 3).
2. Run CSF on ARM server. Get CSF runtime by running it on remote ARM server. To enable this support, you need to add `--enable-femu-arm-server` argument when running `./configure`, see [Meson Options](#meson-options).

### Run CSF on ARM Server

You need to run `arm_server` program on ARM server first. In your ARM server (e.g. Raspberry Pi):

```shell
cd hw/femu/csd/arm_server
make
./arm_server
```

Then add `arm_server_url="tcp://<arm-server-ip>:4231"` in your QEMU command, e.g:

```shell
    -device femu,devsz_mb=$((16*1024)),femu_mode=4,arm_server_url="tcp://192.168.1.1:4231" \
```

## Persistent Memory Backend

FEMU uses DRAM to store SSD data. Since Optane persistent memory has a bigger capacity, we've added support of storing SSD data on persistent memory. To compile this support into FEMU, you should install PMDK first. When meson detected PMDK, it will enable this feature automatically (`CONFIG_LIBPMEM`).

**Attention:** the bandwidth of Optane depends on its configuration (interleave), a single Optane DIMM cannot satisfy the bandwidth requirement of SSD. You should use two or more Optane DIMM and configure them in a interleaved manner.

## Meson Options

See `meson_options.txt`.

- `femu_arm_server`: defines whether compiling the support of running CSF on ARM server. Dependencies: [libnng](https://github.com/nanomsg/nng), used to communicate with ARM Server.

## FDMFS

FDMFS is developed on libfuse and provide a convenient method to run CSF.

The source files in `hw/femu/csd/fuse` implements the FDMFS.

FDMFS depends on the modified bzImage and libfuse, so first you shall meet the prerequisites.

1. build bzImage and replace kernel

```bash
cd ./build-femu
git clone https://github.com/torvalds/linux --depth 1 --branch v6.1
cd ./linux
git apply ../../hw/femu/csd/fuse/fuse_modify.patch
cp ../../hw/femu/csd/fuse/config .config # cp provided kernel compilation config
make bzImage -j$(nproc) # build modified kernel
cp ./arch/x86/boot/bzImage ../dqib_amd64-pc/new_kernel
```

2. modify the run-csd.sh
```
./x86_64-softmmu/qemu-system-x86_64 \
    -name "FEMU-CSD-DQIB" \
    -cpu host \
    -m 8G \
    -smp 16 \
    -enable-kvm \
    -kernel ./dqib_amd64-pc/new_kernel \ # use new kernel
    -initrd ./dqib_amd64-pc/initrd \
    -drive file=./dqib_amd64-pc/image.qcow2 \
    -device femu,devsz_mb=$((16*1024)),femu_mode=4,fdm_size=$((2*1024)) \
    -net user,hostfwd=tcp::2222-:22 \
    -net nic,model=virtio \
    -append "root=LABEL=rootfs console=ttyS0" \
    -nographic \
    -fsdev local,path=../,security_model=mapped-xattr,id=femusrc \
    -device virtio-9p-pci,fsdev=femusrc,mount_tag=femusrc \
    -fsdev local,path=../../fio,security_model=mapped-xattr,id=fiosrc \ # mount fio
    -device virtio-9p-pci,fsdev=fiosrc,mount_tag=fiosrc \
    -fsdev local,path=../../ubpf,security_model=mapped-xattr,id=ubpfsrc \ # mount ubpf
    -device virtio-9p-pci,fsdev=ubpfsrc,mount_tag=ubpfsrc \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log
```

3. modify libfuse

```
cd ./hw/femu/csd/fuse
git clone https://github.com/libfuse/libfuse
cd libfuse
git apply ../libfuse_modify.patch
# start the qemu
sudo ./run-csd.sh
# open another terminal to login as root
ssh -P 2222 root@localhost
cd ~/femu/hw/femu/csd/fuse/libfuse
meson setup build
meson compile -C build
```

auto mount source code directories

```
# vim /etc/fstab
femusrc /root/femu 9p trans=virtio,oversion=9p2000.L 0 0
fiosrc /root/fio 9p trans=virtio,oversion=9p2000.L 0 0
ubpfsrc /root/ubpf 9p trans=virtio,oversion=9p2000.L 0 0
```

4. compile and test the FDMFS

```
# the same ssh terminal in the previous step
cd ~/femu/hw/femu/csd/fuse
make
cd test_helper
make

# init and test
bash ./init_nvme.sh
bash ./start_fuse.sh
bash ./test_ebpf.sh
```