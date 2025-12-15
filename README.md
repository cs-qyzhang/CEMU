# CEMU

**Maintainer**: [Qiuyang Zhang](https://github.com/cs-qyzhang) ([cs.qyzhang@qq.com](mailto:cs.qyzhang@qq.com)), [Jiapin Wang](https://github.com/Emilio597) ([wangjiapin@hust.edu.cn](mailto:wangjiapin@hust.edu.cn))

CEMU is a full-system computational storage drive (CSD) emulation platform that enables accurate and flexible modeling of a wide range of CSD devices. It consists of two major components: a CSD emulator and a CSD-oriented software stack.

There are three repositories:

- [CEMU](https://github.com/cs-qyzhang/CEMU). This repository contains the CSD emulator implementation, built on top of FEMU and QEMU v8.0.
- [linux-cemu](https://github.com/cs-qyzhang/linux-cemu). The CEMU software stack, based on Linux 6.8. It includes the device memory file system (FDMFS), NVMe driver adaptations, and io_uring adaptations.
- [ubpf-cemu](https://github.com/cs-qyzhang/ubpf-cemu). The eBPF execution environment, built on uBPF.

To support full-system emulation, CEMU leverages QEMU to emulate CSDs within the guest VM using the host CPU and DRAM.

## Installation

The following steps have been validated on Ubuntu 22.04. When running the commands, please pay attention to your working directory: the three repositories and the VM image folder should be placed under the same directory (e.g., `~/cemu` in our example).

```bash
# clone code
~$ mkdir cemu && cd cemu
~/cemu$ git clone https://github.com/cs-qyzhang/CEMU.git
~/cemu$ git clone https://github.com/cs-qyzhang/linux-cemu.git
~/cemu$ git clone https://github.com/cs-qyzhang/ubpf-cemu.git --recurse-submodules
# compile modified linux kernel
~/cemu$ cd linux-cemu
~/cemu/linux-cemu$ make bzImage -j$(nproc)
~/cemu/linux-cemu$ cd ..
# compile ubpf for executing eBPF programs
~/cemu$ cd ubpf-cemu
~/cemu/ubpf-cemu$ cmake -S . -B build -DUBPF_ENABLE_TESTS=true
~/cemu/ubpf-cemu$ cmake --build build --config Release
~/cemu/ubpf-cemu$ cd ..
# compile CEMU emulator
~/cemu$ cd CEMU
~/cemu/CEMU$ mkdir build && cd build
~/cemu/CEMU/build$ ../femu-scripts/femu-copy-scripts.sh .
~/cemu/CEMU/build$ sudo ./pkgdep.sh
~/cemu/CEMU/build$ ./femu-compile.sh
~/cemu/CEMU/build$ cd ../../
```

## Quick Start

Download our prebuilt VM image based on [DQIB (Debian Quick Image Baker pre-baked images)](https://people.debian.org/~gio/dqib/):

```bash
~/cemu$ wget https://cemu.oss-cn-hangzhou.aliyuncs.com/dqib.tar.xz
~/cemu$ tar -xJf dqib.tar.xzwget
```

Start CEMU VM:

```bash
~/cemu$ cd CEMU/build
~/cemu$ ./run-csd.sh
```

Open another terminal and connect to the CEMU VM:

```bash
~$ ssh root@localhost -p 2222
```

Mount CSDs inside VM:

```bash
# inside VM
root@debian:~# ls
CEMU  cemu-mount.sh
root@debian:~# ./cemu-mount.sh
```

After mounting, you will see three NVMe namespaces exposed by the NVMe CSD:

- `/dev/nvme0n1`: NVM namespace, representing the NAND flash inside the CSD. This is equivalent to a traditional NVMe SSD.
- `/dev/nvme0m2`: Memory namespace, representing the device memory inside the CSD. By abstracting device memory as a block device and mounting FDMFS on top of it, users can access device memory via file-based operations such as `pread/pwrite`, and enable in-device data copies between NAND and on-device memory via the `copy_file_range` system call.
- `/dev/nvme0c3`: Compute namespace. CEMU uses this namespace to download and execute CSFs.

You can find more information in our paper and NVMe Computational Storage Specification.

```bash
root@debian:~# lsblk
NAME    MAJ:MIN RM  SIZE RO TYPE MOUNTPOINTS
sda       8:0    0   20G  0 disk 
`-sda1    8:1    0   20G  0 part /
sr0      11:0    1 1024M  0 rom  
nvme0m2 252:0    0    2G  0 disk /mnt/fdm0
nvme0n1 259:0    0   48G  0 disk /mnt/nvme0

root@debian:~# mount | grep 'nvme\|cemu'
cemusrc on /root/CEMU type 9p (rw,relatime,access=client,trans=virtio)
/dev/nvme0n1 on /mnt/nvme0 type ext4 (rw,relatime)
/dev/nvme0m2 on /mnt/fdm0 type fdmfs (rw,relatime)
```

Run the examples:

```bash
# inside VM
root@debian:~# cd CEMU/tests/cemu/
root@debian:~CEMU/tests/cemu# ./build/test_fdmfs
root@debian:~CEMU/tests/cemu# ./build/vadd_example
root@debian:~CEMU/tests/cemu# ./build/test_p2p
root@debian:~CEMU/tests/cemu# ./build/test_indirect
root@debian:~CEMU/tests/cemu# ./build/cemu_benchmark
```

## Configuration

## Reproduce

## Acknowledgement

CEMU is built upon the widely used SSD emulator [FEMU](https://github.com/MoatLab/FEMU), which provides a system-level, high-fidelity SSD emulation platform. We sincerely thank the FEMU team for their outstanding work.

We thank [Sicen Li](https://github.com/L-LYR) for his earlier implementation of FDMFS.
