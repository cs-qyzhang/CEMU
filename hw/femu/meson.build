softmmu_ss.add(when: 'CONFIG_FEMU_PCI', if_true: files(
  'dma.c', 'intr.c', 'nvme-util.c', 'nvme-admin.c', 'nvme-io.c', 'femu.c',
  'param.c','lib/pqueue.c', 'lib/rte_ring.c', 'lib/slab.c',
  'backend/backend.c', 'backend/dram.c', 'timing-model/timing.c',
  'nossd/nop.c', 'nand/nand.c',
  # TODO: open-channel support is dropped
  # 'ocssd/oc12.c', 'ocssd/oc20.c',
  'zns/zns.c',
  'bbssd/bb.c', 'bbssd/ftl.c',
  'csd/memory.c', 'csd/compute.c', 'csd/sched.c', 'csd/sched_fifo.c',
  'csd/sched_rr.c', 'csd/sched_grouped_fifo.c',
  'csd/sched_grouped_rr.c', 'statistics/statistics.c'))

ubpf_src = '../../../ubpf-cemu/'
ubpf_lib = '../../ubpf-cemu/build/lib'
libubpf_include = include_directories([ubpf_src + 'vm/inc', ubpf_src + 'build/vm'])
ubpf = declare_dependency(link_args: ['-L'+ubpf_lib, '-lubpf'], include_directories: libubpf_include)
softmmu_ss.add(when: 'CONFIG_FEMU_PCI', if_true: ubpf)

softmmu_ss.add(when: 'CONFIG_FEMU_PCI', if_true: cjson)
softmmu_ss.add(when: 'CONFIG_FEMU_PCI', if_true: libdl)
softmmu_ss.add(when: 'CONFIG_FEMU_PCI', if_true: librt)
