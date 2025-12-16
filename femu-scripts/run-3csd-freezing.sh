#!/bin/bash

# copy this script to ./build-femu
# this script uses DBIQ image:
#   https://www.giovannimascellani.eu/dqib-debian-quick-image-baker.html
#   https://gitlab.com/api/v4/projects/giomasce%2Fdqib/jobs/artifacts/master/download?job=convert_amd64-pc
#
# use virtfs to mount the tests directory to the VM, this feature
# needs to be enabled when building qemu:
#   sudo apt install libcap-ng-dev libattr-dev
#   ../configure --enable-virtfs
#
# use ssh port 2222 to connect guest:
#   ssh -p 2222 root@localhost
sudo ./x86_64-softmmu/qemu-system-x86_64 \
    -name "CEMU-DBIQ" \
    -smp 8 \
    -m 8G \
    -kernel ../../linux-cemu/vmlinux \
    -initrd ../../dqib_amd64-pc/initrd \
    -drive file=../../dqib_amd64-pc/image.qcow2 \
    -device femu,config_file=./cemu_config.json \
    -device femu,config_file=./cemu_config.json \
    -device femu,config_file=./cemu_config.json \
    -net user,hostfwd=tcp::2222-:22 \
    -net nic,model=virtio \
    -append "root=LABEL=rootfs console=ttyS0" \
    -nographic \
    -fsdev local,path=../,security_model=none,id=cemusrc \
    -device virtio-9p-pci,fsdev=cemusrc,mount_tag=cemusrc \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log
