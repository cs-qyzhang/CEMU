#!/bin/bash

nr_dev=$(ls /sys/class/nvme | wc -l)
echo "Found $nr_dev NVMe Drives"

### NVMe Mount
pids=()
for ((i=0; i<$nr_dev; i++)); do
	nvm="/dev/nvme$((i))n1"
	dir="/mnt/nvme$((i))"
	if grep -qs $nvm /proc/mounts; then
		echo "$nvm already mounted!"
	else
		mkfs.ext4 $nvm
		mkdir -p $dir
		mount $nvm $dir
		echo "Mount $nvm to $dir"
		fio --name=write --rw=write --bs=128k --filename=$dir/test --size=16G --iodepth=128 --ioengine=io_uring --direct=1 &
        pids+=($!)
		fallocate -l 16G $dir/output
	fi
done
for pid in ${pids[@]}; do
    wait $pid
done

### FDM Mount
for ((i=0; i<$nr_dev; i++)); do
	fdm="/dev/nvme$((i))m2"
	dir="/mnt/fdm$((i))"
	if [[ -b $fdm ]]; then
		if grep -qs $fdm /proc/mounts; then
			echo "$fdm already mounted!"
		else
			mkdir -p $dir
			mount -t fdmfs $fdm $dir
			echo "Mount $fdm to $dir"
			for ((j=0; j<32; j++)); do
				touch $dir/$j
				fallocate -l 32m $dir/$j
			done
		fi
	fi
done
