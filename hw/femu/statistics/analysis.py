#!/usr/bin/python3

# run this script before running FEMU
import json
import time
import sys
from datetime import datetime
import socket
import pickle
from threading import Thread
from femu_stat import *
import pandas as pd

epoch_time = 1000000000

## global variables
stat = None         # FemuStat
c = None            # defines in C code
per_epoch_stat = []
num_stat = 0
csfs = {}
afdms = {}
femu_stime = 0
late_ios = 0
max_late = 0
behind = 0
max_proceed = 0
total_proceed = 0
max_behind = 0
rw_io_size = []
afdm_io_size = []
p2p_bytes = [[0 for _ in range(16)] for _ in range(16)]
host_io_bytes = 0
internal_io_bytes = 0
read_lat = []
read_size = []

plot_port = 5424
kill_plot_server = False
server_thread = None

class ReqBuf:
    def __init__(self, capacity):
        self.capacity = capacity
        self.buf = [None] * capacity
        self.size = 0
        self.tail = 0

    def push(self, req):
        # self.buf[self.tail] = copy.deepcopy(req)
        self.tail = (self.tail + 1) % self.capacity
        self.size = max(self.capacity, self.size + 1)

    def get(self, idx):
        return self.buf[idx]

req_buf = ReqBuf(1024)
target_idx = 0
left_req = 0
rd = 100000
other = 0

def init():
    global per_epoch_stat, num_stat, csfs, afdms, c, femu_stime, late_ios, max_late
    global behind, total_proceed, max_proceed, max_behind, kill_plot_server
    global rw_io_size, afdm_io_size, p2p_bytes, host_io_bytes, internal_io_bytes
    per_epoch_stat = []
    num_stat = 0
    csfs = {}
    afdms = {}
    c = None
    femu_stime = 0
    late_ios = 0
    max_late = 0
    behind = 0
    max_proceed = 0
    total_proceed = 0
    max_behind = 0
    afdm_io_size = []
    rw_io_size = []
    host_io_bytes = 0
    internal_io_bytes = 0
    p2p_bytes = [[0 for _ in range(16)] for _ in range(16)]

def print_req(req):
    if req.opcode == c.NVME_CMD_CSD_EXEC:
        csf = req.csf
        print(f'{op_name(req)}: csf {csf.csf_id} in {csf.in_afdm_id} out {csf.out_afdm_id}, reqlat {req.reqlat}, exec lat {csf.csf_exec_lat}, host exec {csf.host_exec_lat}, arm exec {csf.arm_exec_lat}')
    else:
        print(f'{op_name(req)}: reqlat {req.reqlat}, nand lat {req.rw.nand_lat}, pcie lat {req.pcie_lat}')

def plot_server(drive_id):
    global kill_plot_server
    '''send stat to frontend to do real-time plotting'''
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('0.0.0.0', plot_port+drive_id))
        print('plot server listening')
        s.listen(1)
        s.setblocking(False)
        s.settimeout(1)
        while not kill_plot_server:
            try:
                conn, addr = s.accept()
                with conn:
                    print(f'plot client connected {addr}')
                    send_stat = 0   # send from start
                    send_stat = num_stat   # send from connection
                    while True:
                        heartbeat = conn.recv(64)
                        if heartbeat == b'exit':
                            print('plot client disconnected')
                            break
                        if kill_plot_server:
                            msg = int(4).to_bytes(4, 'little') + b'exit'
                            conn.sendall(msg)
                            break
                        if send_stat < num_stat:
                            d = pickle.dumps(per_epoch_stat[send_stat])
                            msg = len(d).to_bytes(4, 'little') + d
                            conn.sendall(msg)
                            send_stat += 1
                        time.sleep(0.1)
            except socket.timeout:
                continue
            except Exception as e:
                print(e)

def kill_server():
    global kill_plot_server, server_thread
    kill_plot_server = True
    server_thread.join()
    server_thread = None
    kill_plot_server = False

def dump_stat(filename):
    global per_epoch_stat, num_stat
    if len(per_epoch_stat) <= 2:
        return
    df = pd.json_normalize([t.__dict__() for t in per_epoch_stat])
    df.to_csv(filename, index=False)
    # with open(filename, 'w+') as f:
    #     json.dump(per_epoch_stat, f, indent=4, cls=PerEpochStatEncoder)

def process_download(req, epoch_stat: PerEpochStat):
    csf_id = req.csf.csf_id
    csf = CSF(csf_id, req.csf.jit > 0)
    csfs[csf_id] = csf

def process_exec(req, epoch_stat: PerEpochStat):
    csf_id = req.csf.csf_id
    csf = csfs[csf_id]
    in_afdm = afdms[req.csf.in_afdm_id]
    out_afdm = afdms[req.csf.out_afdm_id]
    e = Execution(in_afdm, out_afdm, req.csf.csf_exec_lat, req.csf.host_exec_lat, req.csf.arm_exec_lat)
    csf.execution.append(e)

def process_alloc(req, epoch_stat: PerEpochStat):
    print(f'alloc {req.afdm.afdm_id}')
    afdm_id = req.afdm.afdm_id
    afdm = AFDM(afdm_id, req.afdm.size)
    afdms[afdm_id] = afdm

def process_dealloc(req, epoch_stat: PerEpochStat):
    afdm_id = req.afdm.afdm_id
    del afdms[afdm_id]

def process_nvm_to_afdm(req, epoch_stat: PerEpochStat):
    afdm_io_size.append(req.data_size)
    # afdm_id = req.afdm.afdm_id
    # offset = req.afdm.offset
    # afdm = afdms[afdm_id]
    # afdm.write_from_nvm(offset, req.rw.slba, req.rw.nlb)
    epoch_stat.afdm_rw.add_write(req) # read nvm to afdm
    # epoch_stat.rw.add_read(req)

def process_afdm_to_nvm(req, epoch_stat: PerEpochStat):
    epoch_stat.afdm_rw.add_read(req) # read nvm to afdm
    afdm_io_size.append(req.data_size)
    # epoch_stat.rw.add_write(req)

def process_afdm_to_afdm(req, epoch_stat: PerEpochStat):
    assert(0)
    epoch_stat.afdm_rw.add_read(req) # read nvm to afdm
    epoch_stat.afdm_rw.add_write(req) # read nvm to afdm

def process_read_afdm(req, epoch_stat: PerEpochStat):
    # epoch_stat.afdm_rw.add_read(req) # read nvm to afdm
    pass

def process_write_afdm(req, epoch_stat: PerEpochStat):
    # epoch_stat.afdm_rw.add_write(req) # read nvm to afdm
    pass

def process(req, epoch_stat: PerEpochStat):
    """process nvme request"""
    global c, per_epoch_stat, num_stat, req_buf, target_idx, left_req, rd, other, p2p_bytes, host_io_bytes, internal_io_bytes
    global read_lat, read_size
    epoch_stat.processed += 1
    epoch_stat.cmd_cnt[req.opcode] += 1
    req_buf.push(req)
    if left_req:
        left_req -= 1
        if req.opcode == 0x02:
            rd += 1
        else:
            other += 1
        if left_req == 0:
            # print('left_req == 0')
            # ranges = [x for x in range(req_buf.tail, req_buf.capacity)] +\
            #          [x for x in range(0, req_buf.tail)]
            # print_req(req_buf.get(target_idx))
            # rd = 0
            # other = 0
            # for i in ranges:
            #     r = req_buf.get(i)
            print(f'rd: {rd}, other: {other}')

    if req.opcode == 0x02:  # read
        # if left_req == 0 and req.etime - req.stime > 1000000:
        #     print("READ lat > 1ms")
        #     left_req = req_buf.capacity / 2
        #     print_req(req)
        #     target_idx = req_buf.tail - 1
        #     rd = 0
        #     other = 0
        read_lat.append((req.etime - req.stime)/1000.0)
        read_size.append(req.data_size)
        rw_io_size.append(req.data_size)
        epoch_stat.rw.add_read(req)
        host_io_bytes += req.data_size
    elif req.opcode == 0x01:    # write
        epoch_stat.rw.add_write(req)
        rw_io_size.append(req.data_size)
        host_io_bytes += req.data_size
    # elif req.opcode == 0x31:    # execute
    #     process_exec(req, epoch_stat)
    # elif req.opcode == c.NVME_CMD_CSD_DOWNLOAD:
    #     process_download(req, epoch_stat)
    elif req.opcode == 0x22:    # read afdm
        host_io_bytes += req.data_size
        process_read_afdm(req, epoch_stat)
    elif req.opcode == 0x25:    # write afdm
        host_io_bytes += req.data_size
        process_write_afdm(req, epoch_stat)
    elif req.opcode == 0x23:    # nvm to afdm
        internal_io_bytes += req.data_size
        process_nvm_to_afdm(req, epoch_stat)
    elif req.opcode == 0x24:    # afdm to nvm
        internal_io_bytes += req.data_size
        process_afdm_to_nvm(req, epoch_stat)
    elif req.opcode == 0x21:    # afdm to afdm
        # internal_io_bytes += req.data_size
        process_afdm_to_afdm(req, epoch_stat)
    elif req.opcode == 0x26:    # p2p
        epoch_stat.p2p_bytes[req.src_drive][req.dst_drive] += req.data_size
        p2p_bytes[req.src_drive][req.dst_drive] += req.data_size
    elif req.opcode == 0x27:
        # p2p ssd side
        pass
    # elif req.opcode == c.NVME_CMD_CSD_ALLOC_FDM:
    #     process_alloc(req, epoch_stat)
    # elif req.opcode == c.NVME_CMD_CSD_DEALLOC_AFDM:
    #     process_dealloc(req, epoch_stat)

def plot_dist(io_sizes, bins=None):
    # 自定义区间（例如：[0-64], [65-128], [129-256], [257-512]）
    if bins == None:
        bins = [0, 4096, 16384, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152, 4194304, 8388608]

    # 初始化每个区间的计数器
    histogram = [0] * (len(bins) - 1)

    # 计算每个区间的频率
    for size in io_sizes:
        for i in range(1, len(bins)):
            if bins[i-1] < size <= bins[i]:
                histogram[i-1] += 1
                break

    # 计算总的请求数
    total_requests = len(io_sizes)

    # 输出柱状图到命令行，基于百分比
    print(f"Total IO: {total_requests}")
    print(f"{'Range'.ljust(17)} | {'Percentage'.ljust(10)} | Histogram")
    print("-" * 50)
    for i in range(len(histogram)):
        if bins[i+1] > 1024:
            range_label = f"({bins[i]/1024}k-{bins[i+1]/1024}k]".ljust(17)
        else:
            range_label = f"({bins[i]}-{bins[i+1]}]".ljust(17)
        percentage = 0 if histogram[i] == 0 else (histogram[i] / total_requests) * 100
        bar = '*' * int(percentage / 5)  # 每个 '*' 代表 5%
        print(f"{range_label} | {f'{percentage:.2f}%'.ljust(10)} | {bar}")
    print("-" * 50)

def clean_up(unlink=True):
    global late_ios, max_late, behind, total_proceed, max_proceed, max_behind, stat
    print(f'late_ios: {late_ios}')
    print(f'max_late: {max_late}')
    print(f'behind: {behind}')
    print(f'total_proceed: {total_proceed}')
    print(f'max_proceed: {max_proceed}')
    print(f'max_behind: {max_behind}')
    for core in range(4):
        core_stat = stat.cse_core_stat[core]
        if core_stat.idle_time + core_stat.exec_time == 0:
            continue
        print(f'core {core} idle time: {core_stat.idle_time}, exec time: {core_stat.exec_time}, exec percent: {core_stat.exec_time / (core_stat.idle_time + core_stat.exec_time)}')

    print('plotting io size distribution...')
    plot_dist(rw_io_size[2900:])    # first 2900 IO is for initialization
    print('plotting afdm io size distribution...')
    plot_dist(afdm_io_size)
    print('='*50)
    print('P2P bytes:')
    p2p_io_bytes = 0
    for i in range(16):
        for j in range(16):
            if p2p_bytes[i][j] > 0:
                p2p_io_bytes += p2p_bytes[i][j]
                print(f'  drive {i} -> {j}: {MB(p2p_bytes[i][j]):.2f}MB')
    print(f'P2P IO bytes: {MB(p2p_io_bytes*2):.2f}MB')
    print(f'Host IO bytes: {MB(host_io_bytes):.2f}MB')
    print(f'Internal IO bytes: {MB(internal_io_bytes):.2f}MB')
    close_shm(unlink=unlink)
    kill_server()
    stat_file = datetime.now().strftime('%Y-%m-%d-%H-%M-%S')
    stat_file = f'./stat/{stat_file}.csv'
    if not os.path.exists('./stat'):
        os.makedirs('./stat')
    dump_stat(stat_file)

def main(drive_id):
    init()

    global c, per_epoch_stat, num_stat, server_thread, stat
    global femu_stime, late_ios, max_late, behind, total_proceed, max_proceed, max_behind, read_lat, read_size

    ## init cffi and shared memory
    # stat is the area of shared memory, type: FemuStat
    stat, c = open_shm(drive_id)

    ## plot server, send data to frontend client
    server_thread = Thread(target=plot_server, args=(drive_id,))
    server_thread.start()

    ## wait for FEMU to start
    assert stat.alive == 0
    print('waiting for FEMU to start...')
    while True:
        if stat.alive == 1:
            print('FEMU is alive now')
            femu_stime = stat.start_time
            break

    ## FEMU is alive, retrive FEMU statistics and do analysis
    head = 0    # message queue head
    epoch_stat = PerEpochStat(time.time_ns(), epoch_time)
    next_epoch = epoch_stat.stime + epoch_time
    read_lat = []
    read_size = []
    while True:
        now = time.time_ns()
        # next epoch
        if now > next_epoch:
            epoch_stat.etime = now
            max_proceed = max(max_proceed, epoch_stat.processed)
            per_epoch_stat.append(epoch_stat)
            if epoch_stat.processed > 0:
                print(epoch_stat)
                num_stat += 1
                epoch_stat = PerEpochStat(now, epoch_time)
            else:
                epoch_stat = PerEpochStat(now, epoch_time)
            next_epoch = now + epoch_time
            if len(read_lat):
                plot_dist(read_lat, bins=[0, 2, 4, 8, 32, 64, 128, 256])
                plot_dist(read_size)
            read_lat = []
            read_size = []
            if stat.alive == 0 and head == stat.tail:
                print(f'FEMU is dead now')
                break
        if stat.tail != head:
            ahead = stat.tail - head if stat.tail > head else c.FEMU_STAT_CAPACITY - head + stat.tail
            max_behind = max(max_behind, ahead)
            if ahead > c.FEMU_STAT_CAPACITY / 2:
                behind += 1
                # print(f'WARNING: behind {ahead} requests')

            req = stat.req_stat[head]
            process(req, epoch_stat)
            total_proceed += 1

            # record late io
            late = req.etime - req.expire_time
            if  late > 1000:
                late_ios += 1
                max_late = max(max_late, late)
                epoch_stat.late_ios += 1

            # move head
            head = (head + 1) % c.FEMU_STAT_CAPACITY

    ## FEMU is dead
    clean_up(unlink=True)

if __name__ == '__main__':
    try:
        while True:
            drive_id = 0;
            if len(sys.argv) > 1:
                drive_id = int(sys.argv[1])
            main(drive_id)
            time.sleep(2)
            print('-'*50)
    except KeyboardInterrupt:
        print('interrupted by user')
        clean_up(unlink=True)
        exit(1)