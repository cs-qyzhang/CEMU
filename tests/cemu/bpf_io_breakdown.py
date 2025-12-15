#!/usr/bin/python3
from bcc import BPF
import time
import sys

# eBPF program
bpf_program = """
#include <uapi/linux/ptrace.h>
#include <linux/blkdev.h>
#include <linux/nvme.h>

BPF_HASH(start_time, u64, u64);
BPF_HASH(start, u64);
// BPF_ARRAY(time, u64, 128)

int test_nvme_queue_rq(struct pt_regs *ctx) {
    bpf_trace_printk("nvme_queue_rq %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

int trace_vfs_read(struct pt_regs *ctx, struct file *file, char __user *buf, size_t count, loff_t *pos) {
    dev_t dev = file->f_inode->i_sb->s_dev;
    bpf_trace_printk("vfs_read dev %d, %llu\\n", dev, bpf_ktime_get_boot_ns());

    return 0;
}

int trace_vfs_copy_file_range(struct pt_regs *ctx) {
    bpf_trace_printk("vfs_copy_file_range %llu\\n", bpf_ktime_get_boot_ns());

    return 0;
}

int trace_ksys_read(struct pt_regs *ctx, unsigned int fd, char __user *buf, size_t count) {
    dev_t dev = 1;
    bpf_trace_printk("ksys_read dev %d, %llu\\n", dev, bpf_ktime_get_boot_ns());
    return 0;
}

//int trace_io_read(struct pt_regs *ctx) {
//    bpf_trace_printk("io_read %llu\\n", bpf_ktime_get_boot_ns());
//    return 0;
//}

TRACEPOINT_PROBE(fdmfs, fdmfs_rw_begin) {
    bpf_trace_printk("fdmfs_rw_begin %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

TRACEPOINT_PROBE(fdmfs, fdmfs_rw_middle) {
    bpf_trace_printk("fdmfs_rw_middle %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

TRACEPOINT_PROBE(fdmfs, fdmfs_rw_end) {
    bpf_trace_printk("fdmfs_rw_end %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

TRACEPOINT_PROBE(fdmfs, fdmfs_copy_file_range_begin) {
    bpf_trace_printk("fdmfs_copy_file_range_begin %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

TRACEPOINT_PROBE(fdmfs, fdmfs_copy_file_range_end) {
    bpf_trace_printk("fdmfs_copy_file_range_end %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

TRACEPOINT_PROBE(nvme, nvme_setup_cmd) {
    u64 id = args->qid;
    bpf_trace_printk("nvme_setup_cmd %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

// NVMe complete request
TRACEPOINT_PROBE(nvme, nvme_complete_rq) {
    u64 id = args->qid;
    u64 *tsp, delta;

    bpf_trace_printk("nvme_complete_rq %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

// io_uring enter
TRACEPOINT_PROBE(io_uring, io_uring_submit_req) {
    bpf_trace_printk("io_uring_submit_rq %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

// io_uring exit
TRACEPOINT_PROBE(io_uring, io_uring_complete) {
    u64 tid = bpf_get_current_pid_tgid();
    u64 *tsp, delta;

    bpf_trace_printk("io_uring_complete %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

// iomap begin
TRACEPOINT_PROBE(iomap, iomap_dio_rw_begin) {
    bpf_trace_printk("iomap_dio_rw_begin %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

// iomap end
TRACEPOINT_PROBE(iomap, iomap_dio_complete) {
    bpf_trace_printk("iomap_dio_complete %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

TRACEPOINT_PROBE(block, block_bio_queue) {
    bpf_trace_printk("block_bio_queue %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

TRACEPOINT_PROBE(block, block_bio_complete) {
    bpf_trace_printk("block_bio_complete %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

TRACEPOINT_PROBE(block, block_rq_issue) {
    bpf_trace_printk("block_rq_issue %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

//TRACEPOINT_PROBE(nvme, nvme_prep_rq) {
//    if (args->nsid == 2 || args->nsid == 3)
//        bpf_trace_printk("nvme_prep_rq: opcode %d, nsid %d, %llu\\n", args->opcode, args->nsid, bpf_ktime_get_boot_ns());
//    return 0;
//}

TRACEPOINT_PROBE(block, block_rq_complete) {
    bpf_trace_printk("block_rq_complete %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

TRACEPOINT_PROBE(ext4, ext4_iomap_begin) {
    bpf_trace_printk("ext4_iomap_begin %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

TRACEPOINT_PROBE(ext4, ext4_iomap_end) {
    bpf_trace_printk("ext4_iomap_end %llu\\n", bpf_ktime_get_boot_ns());
    return 0;
}

//TRACEPOINT_PROBE(ext4, ext4_read_folio) {
//    bpf_trace_printk("ext4_read_folio %llu\\n", bpf_ktime_get_boot_ns());
//    return 0;
//}
"""

# Initialize BPF
b = BPF(text=bpf_program)
# b.attach_kprobe(event="vfs_read", fn_name="trace_vfs_read")
b.attach_kprobe(event="vfs_copy_file_range", fn_name="trace_vfs_copy_file_range")

print("Starting...")
if len(sys.argv) > 1:
    b.trace_print()

is_iou = True

start = False
time_map = {}
iomap_lat_total = 0;
ext4_lat_total = 0;
fdmfs_lat_total = 0;
nvm_lat_total = 0;
block_lat_total = 0;
iou_lat_total = 0;
lat_total = 0;
stage = 0;
stage_name = ['input', 'compute', 'output']
stage_lat = [0, 0, 0]
# # Process events
while True:
    try:
        # Read trace pipe
        # for (_, _, _, _, _, msg) in b.trace_fields():
        #     print(msg)
        while True:
            (task, pid, cpu, flags, ts, msg) = b.trace_fields()
            func, time = msg.decode("utf-8").split(" ")
            # print(func, time)
            if not start and is_iou and func == "io_uring_submit_rq":
                start = True
            if not start and not is_iou and func == 'iomap_dio_rw_begin':
                start = True
            if start:
                time_map[func] = int(time)
            if func == "io_uring_complete" and start:
                start = False
                nvm_lat = time_map['nvme_complete_rq'] - time_map['block_rq_issue']
                block_lat = time_map['block_rq_complete'] - time_map['block_rq_issue']
                iou_lat = time_map['io_uring_complete'] - time_map['io_uring_submit_rq']
                total_lat = iou_lat
                iomap_lat = block_lat
                if 'iomap_dio_complete' in time_map:
                    iomap_lat = time_map['iomap_dio_complete'] - time_map['iomap_dio_rw_begin']
                    iou_before = time_map['iomap_dio_rw_begin'] - time_map['io_uring_submit_rq']
                    iou_after = time_map['io_uring_complete'] - time_map['iomap_dio_complete']
                else:
                    iou_before = time_map['nvme_setup_cmd'] - time_map['io_uring_submit_rq']
                    iou_after = time_map['io_uring_complete'] - time_map['block_rq_complete']
                fdmfs_lat = 0
                fdmfs_before = 0
                fdmfs_after = 0
                if 'fdmfs_rw_end' in time_map:
                    fdmfs_before = time_map['iomap_dio_rw_begin'] - time_map['fdmfs_rw_begin']
                    fdmfs_after = time_map['fdmfs_rw_end'] - time_map['fdmfs_rw_middle']
                    fdmfs_lat = fdmfs_before + fdmfs_after
                    iou_before = time_map['fdmfs_rw_begin'] - time_map['io_uring_submit_rq']
                if 'fdmfs_copy_file_range_end' in time_map and 'fdmfs_copy_file_range_begin' in time_map:
                    fdmfs_lat = time_map['fdmfs_copy_file_range_end'] - time_map['fdmfs_copy_file_range_begin']
                    iou_lat -= fdmfs_lat
                    iou_lat -= iomap_lat
                else:
                    iou_lat -= iomap_lat
                ext4_lat = 0
                if 'ext4_iomap_end' in time_map:
                    ext4_lat = time_map['ext4_iomap_end'] - time_map['ext4_iomap_begin']
                iomap_lat -= block_lat
                block_lat -= nvm_lat

                print(f"{stage_name[stage]} latency: total {total_lat}, nvm {nvm_lat}, block {block_lat}, iomap {iomap_lat}, ext4 {ext4_lat}, fdmfs {fdmfs_lat}, fdmfs before {fdmfs_before}, fdmfs after {fdmfs_after}, iou {iou_lat}, iou before {iou_before}, iou after {iou_after}")
                stage_lat[stage] += total_lat
                stage = (stage + 1) % len(stage_name)
                lat_total += total_lat
                nvm_lat_total += nvm_lat
                block_lat_total += block_lat
                iomap_lat_total += iomap_lat
                ext4_lat_total += ext4_lat
                fdmfs_lat_total += fdmfs_lat
                iou_lat_total += iou_lat
                time_map = {}
            if func == 'iomap_dio_complete' and not is_iou and start:
                start = False
                nvm_lat = time_map['nvme_complete_rq'] - time_map['block_rq_issue']
                block_lat = time_map['block_rq_complete'] - time_map['block_rq_issue']
                iomap_lat = time_map['iomap_dio_complete'] - time_map['iomap_dio_rw_begin']
                print(f"Latency: total {iomap_lat}, nvm {nvm_lat}, block {block_lat - nvm_lat}, iomap {iomap_lat - block_lat}")
                total_lat += iou_lat
                nvm_lat_total += nvm_lat
                block_lat_total += block_lat - nvm_lat
                iomap_lat_total += iomap_lat - block_lat
                iou_lat_total += iou_lat - iomap_lat
                time_map = {}
    except KeyboardInterrupt:
        print()
        print(f'total: {lat_total/1000}, input: {stage_lat[0]/1000}, compute: {stage_lat[1]/1000}, output: {stage_lat[2]/1000}, nvm: {nvm_lat_total/1000}, block: {block_lat_total/1000}, iomap: {iomap_lat_total/1000}, ext4: {ext4_lat_total/1000}, fdmfs: {fdmfs_lat_total/1000}, iou: {iou_lat_total/1000}')
        exit()
