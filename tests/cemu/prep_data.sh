#!/bin/bash
cd /mnt/fdm0
for ((i=0;i<4;i++)); do
	if [ ! -f $i ]; then
		touch $i
		fallocate -l 16m $i
	fi
done
