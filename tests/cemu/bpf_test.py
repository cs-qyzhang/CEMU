#!/usr/bin/python3
from bcc import BPF

bpf_text = """
BPF_HASH(start, u64);

TRACEPOINT_PROBE(block, block_rq_issue) {
    u64 ts = bpf_ktime_get_ns();
    u64 sector = args->sector;
    start.update(&sector, &ts);
    return 0;
}

TRACEPOINT_PROBE(block, block_rq_complete) {
    u64 *tsp, delta;
    u64 ts = bpf_ktime_get_ns();
    u64 sector = args->sector;

    tsp = start.lookup(&sector);
    if (tsp != 0) {
        delta = ts - *tsp;
        bpf_trace_printk("block_rq_complete: sector=%d, duration=%d ns\\n", sector, delta);
        start.delete(&sector);
    }
    return 0;
}
"""

b = BPF(text=bpf_text)
b.trace_print()
