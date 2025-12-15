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
    if (dev == 264241152 || dev == 271581184)
        bpf_trace_printk("vfs_read %llu\\n", bpf_ktime_get_boot_ns());

    return 0;
}

int trace_vfs_copy_file_range(struct pt_regs *ctx, struct file *file_in, loff_t *ppos_in, struct file *file_out, loff_t *ppos_out, size_t len, unsigned int flags) {
    dev_t in_dev = file_in->f_inode->i_sb->s_dev;
    dev_t out_dev = file_out->f_inode->i_sb->s_dev;
    //bpf_trace_printk("vfs_copy_file_range %d %d %llu\\n", in_dev, out_dev, bpf_ktime_get_boot_ns());
    bpf_trace_printk("vfs_copy_file_range %llu\\n", bpf_ktime_get_boot_ns());

    return 0;
}

TRACEPOINT_PROBE(fdmfs, fdmfs_rw_begin) {
    bpf_trace_printk("fdmfs_rw_begin %llu\\n", bpf_ktime_get_boot_ns());
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
"""

# Initialize BPF
b = BPF(text=bpf_program)
b.attach_kprobe(event="vfs_read", fn_name="trace_vfs_read")
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
            if not start and (func == "vfs_copy_file_range" or func == "vfs_read"):
                start = True
            if not start and not is_iou and func == 'iomap_dio_rw_begin':
                start = True
            if start:
                time_map[func] = int(time)
            if (func == "fdmfs_copy_file_range_end" or func == "fdmfs_rw_end") and start:
            # if func == "iomap_dio_complete" and start:
                start = False
                nvm_lat = time_map['nvme_complete_rq'] - time_map['block_rq_issue']
                block_lat = time_map['block_rq_complete'] - time_map['block_rq_issue']
                iomap_lat = block_lat
                if 'iomap_dio_complete' in time_map:
                    iomap_lat = time_map['iomap_dio_complete'] - time_map['iomap_dio_rw_begin']
                fdmfs_lat = 0
                if 'fdmfs_copy_file_range_end' in time_map and 'fdmfs_copy_file_range_begin' in time_map:
                    fdmfs_lat = time_map['fdmfs_copy_file_range_end'] - time_map['fdmfs_copy_file_range_begin']
                    total_lat = time_map['fdmfs_copy_file_range_end'] - time_map['vfs_copy_file_range']
                    fdmfs_lat -= iomap_lat
                elif 'fdmfs_rw_end' in time_map:
                    fdmfs_lat = time_map['fdmfs_rw_end'] - time_map['fdmfs_rw_begin']
                    total_lat = time_map['fdmfs_rw_end'] - time_map['vfs_read']
                    fdmfs_lat -= iomap_lat
                else:
                    total_lat = time_map['iomap_dio_complete'] - time_map['iomap_dio_rw_begin']
                ext4_lat = 0
                if 'ext4_iomap_end' in time_map:
                    ext4_lat = time_map['ext4_iomap_end'] - time_map['ext4_iomap_begin']
                iomap_lat -= block_lat
                block_lat -= nvm_lat
                stage_lat[stage] += total_lat
                stage = (stage + 1) % len(stage_name)
                lat_total += total_lat
                nvm_lat_total += nvm_lat
                block_lat_total += block_lat
                iomap_lat_total += iomap_lat
                ext4_lat_total += ext4_lat
                fdmfs_lat_total += fdmfs_lat

                print(f"latency: total {total_lat}, nvm {nvm_lat}, block {block_lat}, iomap {iomap_lat}, ext4 {ext4_lat}, fdmfs {fdmfs_lat}")
                time_map = {}
    except KeyboardInterrupt:
        print()
        print(f'total: {lat_total/1000}, input: {stage_lat[0]/1000}, compute: {stage_lat[1]/1000}, output: {stage_lat[2]/1000}, nvm: {nvm_lat_total/1000}, block: {block_lat_total/1000}, iomap: {iomap_lat_total/1000}, ext4: {ext4_lat_total/1000}, fdmfs: {fdmfs_lat_total/1000}')
        exit()
