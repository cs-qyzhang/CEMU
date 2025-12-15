# pip install cffi
import json
import os
import re
from collections import defaultdict
from multiprocessing import shared_memory
from cffi import FFI

LUN_PER_CH = 8
NR_CH = 8
NR_LUN = LUN_PER_CH * NR_CH
PAGE_SIZE = 4096
c = None

opcodes_ = {}
shm_ = None

def MB(bytes):
    return bytes / 1024 / 1024

class DataPiece:
    def __init__(self, is_nvm, offset, size=0, slba=0, nlb=0):
        self.is_nvm = is_nvm
        self.offset = offset
        if is_nvm:
            self.slba = slba
            self.nlb = nlb
        else:
            self.size = size

class AFDM:
    def __init__(self, afdm_id, size):
        self.afdm_id = afdm_id
        self.size = size
        self.data = []

    def write_from_nvm(self, offset, slba, nlb):
        self.data.append(DataPiece(True, offset, slba=slba, nlb=nlb))

    def write_from_user(self, offset, size):
        self.data.append(DataPiece(False, offset, size=size))

class Execution:
    def __init__(self, in_afdm, out_afdm, exec_lat, host_lat, arm_lat=0):
        self.in_afdm = in_afdm
        self.out_afdm = out_afdm
        self.exec_lat = exec_lat
        self.host_lat = host_lat
        self.arm_lat = arm_lat

class CSF:
    def __init__(self, csf_id, jit):
        self.csf_id = csf_id
        self.jit = jit
        self.execution = []

    def exec(self, in_afdm, out_afdm, exec_lat, host_lat, arm_lat=0):
        self.execution.append(Execution(in_afdm, out_afdm, exec_lat, host_lat, arm_lat))

class FlashStat:
    def __init__(self):
        self.read_bytes = 0
        self.write_bytes = 0

    def total(self):
        return self.read_bytes + self.write_bytes

    def add_read(self, req):
        self.read_bytes += req.data_size

    def add_write(self, req):
        self.write_bytes += req.data_size

    def __dict__(self):
        return {
            'read_bytes': self.read_bytes,
            'write_bytes': self.write_bytes
        }

class PerEpochStat:
    def __init__(self, stime=0, epoch_time=1000000000):
        self.epoch_time = epoch_time    # nano seconds
        self.scale = 1000000000.0 / self.epoch_time
        self.stime = stime
        self.etime = 0
        # read/write
        self.rw = FlashStat()
        # csf
        self.afdm_rw = FlashStat()
        # total
        self.processed = 0
        self.late_ios = 0
        self.cmd_cnt = defaultdict(int)
        self.p2p_bytes = [[0 for _ in range(16)] for _ in range(16)]    # [a][b] means drive a (ssd) to drive b (memory)

    def __dict__(self):
        p2p_io = 0
        for i in range(16):
            for j in range(16):
                p2p_io += self.p2p_bytes[i][j]
        p2p_io *= 2
        return {
            'epoch_time': self.epoch_time,
            'stime': self.stime,
            'etime': self.etime,
            'processed': self.processed,
            'late_ios': self.late_ios,
            'rw': self.rw.__dict__(),
            'afdm_rw': self.afdm_rw.__dict__(),
            'p2p_bytes': p2p_io
        }

    def normal_io_cnt(self):
        global c
        return self.cmd_cnt[c.NVME_CMD_READ] + self.cmd_cnt[c.NVME_CMD_WRITE]

    def compute_io_cnt(self):
        global c
        return self.cmd_cnt[c.NVME_CMD_CSD_ALLOCATE_FDM] +\
               self.cmd_cnt[c.NVME_CMD_CSD_DEALLOC_AFDM] +\
               self.cmd_cnt[c.NVME_CMD_CSD_DOWNLOAD] +\
               self.cmd_cnt[c.NVME_CMD_CSD_NVM_TO_AFDM] +\
               self.cmd_cnt[c.NVME_CMD_CSD_READ_AFDM] +\
               self.cmd_cnt[c.NVME_CMD_CSD_WRITE_AFDM]

    def pcie_normal_io(self):
        '''PCIe normal IO bytes'''
        return self.rw.read_bytes + self.rw.write_bytes

    def pcie_compute_io(self):
        '''PCIe compute IO bytes'''
        return self.afdm_rw.read_bytes + self.afdm_rw.write_bytes

    def pcie_total_bytes(self):
        '''PCIe total bytes'''
        return self.pcie_normal_io() + self.pcie_compute_io()

    def __str__(self) -> str:
        pcie_total = self.pcie_total_bytes()
        pcie_io = self.pcie_normal_io()
        pcie_compute = self.pcie_compute_io()
        if pcie_total == 0:
            pcie_compute_percent = 0
        else:
            pcie_compute_percent = pcie_compute * 100.0 / pcie_total

        normal_io = self.rw.total()
        compute_io = self.afdm_rw.total()
        total_io = normal_io + compute_io
        if total_io == 0:
            compute_io_percent = 0
        else:
            compute_io_percent = compute_io * 100.0 / total_io

        s = f'''\
{self.processed} IOPS ({self.late_ios} late, {0 if self.processed == 0 else self.late_ios*100.0/self.processed:.2f}%)
    normal: read {MB(self.rw.read_bytes*self.scale):.2f}MB/s, write {MB(self.rw.write_bytes*self.scale):.2f}MB/s
    csf:    read {MB(self.afdm_rw.read_bytes*self.scale):.2f}MB/s, write {MB(self.afdm_rw.write_bytes*self.scale):.2f}MB/s
    io:     normal IO {MB(normal_io*self.scale):.2f}MB/s, compute IO {MB(compute_io*self.scale):.2f}MB/s, {compute_io_percent:.2f}%
    pcie:   normal IO {MB(pcie_io*self.scale):.2f}MB/s, compute IO {MB(pcie_compute*self.scale):.2f}MB/s, {pcie_compute_percent:.2f}%
'''
        for i in range(16):
            for j in range(16):
                if self.p2p_bytes[i][j] > 0:
                    s += f'    drive {i} -> drive {j}: {MB(self.p2p_bytes[i][j]):.2f}MB/s\n'
        return s

    def __repr__(self) -> str:
        return self.__str__()

class PerEpochStatEncoder(json.JSONEncoder):
    def default(self, obj):
        j = {}
        j['epoch_time'] = obj.epoch_time
        j['stime'] = obj.stime
        j['etime'] = obj.etime
        j['processed'] = obj.processed
        j['late_ios'] = obj.late_ios
        j['cmd_cnt'] = obj.cmd_cnt
        j['rw'] = obj.rw.__dict__
        j['afdm_rw'] = obj.afdm_rw.__dict__
        j['read_afdm_bytes'] = obj.read_afdm_bytes
        j['write_afdm_bytes'] = obj.write_afdm_bytes
        return j

def open_shm(drive_id):
    ffi = FFI()
    cdef = ''
    lines = []
    with open('./statistics.h', 'r') as f:
        lines += f.readlines()
    with open('../nvme-def.h', 'r') as f:
        lines += f.readlines()

    start = False       # definitions are surrounded by `/* START */` and `/* END */`
    is_enum_cmd = False # extract nvme command names of `enum NvmeIoCommands`
    shm_name = ''       # shared memory region name
    for line in lines:
        if line.strip() == '/* START */':
            start = True
        elif line.strip() == '/* END */':
            start = False
        elif start:
            if is_enum_cmd and line.strip() == '};':
                is_enum_cmd = False
            elif is_enum_cmd:
                m = re.match(r'\s*(\w+)\s*=\s*(0x[0-9a-f]+).*', line)
                if m:
                    opcodes_[int(m.group(2), 16)] = m.group(1)
            elif line.startswith('enum NvmeIoCommands'):
                is_enum_cmd = True
            m = re.match(r'\s*#define\s+FEMU_STAT_SHM_NAME\s+"(.+)"\s+', line)
            if m:
                shm_name = m.group(1)
                continue
            cdef += line

    if shm_name == '':
        print('cannot find FEMU_STAT_SHM_NAME')
        exit(1)

    ffi.cdef(cdef)
    global c
    c = ffi.dlopen('c')

    shm_name = f'{shm_name}-{drive_id}'
    femu_stat_size = ffi.sizeof('FemuStat')
    print(f'sizeof(FemuStat): {femu_stat_size}')
    print(f'SHM name: {shm_name}')
    shm_file = f'/dev/shm/{shm_name}'
    if os.path.isfile(shm_file):
        os.remove(shm_file)

    global shm_
    shm_ = shared_memory.SharedMemory(name=shm_name, create=True, size=femu_stat_size)
    stat = ffi.cast('FemuStat*', ffi.from_buffer(shm_.buf))

    return stat, c


def op_name(req):
    """Return the name of the request"""
    global opcodes_
    if req.opcode in opcodes_:
        return opcodes_[req.opcode]
    else:
        return str(req.opcode)

def close_shm(unlink=False):
    global shm_
    shm_.close()
    if unlink:
        shm_.unlink()