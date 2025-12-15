#!/usr/bin/python3
from bcc import BPF
import ctypes
import time

# eBPF program
bpf_program = """
#include <uapi/linux/ptrace.h>
#include <linux/blkdev.h>
#include <linux/nvme.h>

TRACEPOINT_PROBE(nvme, nvme_prep_rq) {
    if (args->nsid == 2 || args->nsid == 3)
        bpf_trace_printk("nvme_prep_rq opcode%d, nsid%d, %llu\\n", args->opcode, args->nsid, bpf_ktime_get_boot_ns());
    return 0;
}

TRACEPOINT_PROBE(nvme, nvme_complete_rq) {
    if (args->nsid == 2 || args->nsid == 3)
        bpf_trace_printk("nvme_complete_rq opcode%d, nsid%d, %llu\\n", args->opcode, args->nsid, bpf_ktime_get_boot_ns());
    return 0;
}
"""

# Initialize BPF
b = BPF(text=bpf_program)

print("Starting...")
# b.trace_print()

start = False
input_time = 0
output_time = 0
kernel_time = 0
start_time = 0;
end_time = 0;
# # Process events
last_opcode = 0;
last_nsid = 0;
while True:
    try:
        while True:
            (task, pid, cpu, flags, ts, msg) = b.trace_fields()
            func, opcode, nsid, time = msg.decode("utf-8").split(" ")
            opcode = int(opcode[6])
            nsid = int(nsid[4])
            if not start and func == "nvme_prep_rq":
                start = True
            if func == "nvme_prep_rq" and start:
                start_time = int(time)
                last_nsid = nsid
                last_opcode = opcode
            if func == 'nvme_complete_rq' and start:
                end_time = int(time)
                if nsid == last_nsid and opcode == last_opcode:
                    dur = end_time - start_time
                    if nsid == 2 and opcode == 1:
                        # memory copy
                        print(f"memory copy: {dur}")
                        input_time += dur;
                    elif nsid == 2 and opcode == 2:
                        # memory read
                        print(f"memory read: {dur}")
                        output_time += dur;
                    elif nsid == 3:
                        # compute
                        print(f"compute: {dur}")
                        kernel_time += dur;
                time_map = {}
    except KeyboardInterrupt:
        print()
        print(f"input time: {input_time / 1000}")
        print(f"kernel time: {kernel_time / 1000}")
        print(f"output time: {output_time / 1000}")
        print(f"total: {(input_time + output_time + kernel_time) / 1000}")
        exit()
