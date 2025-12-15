#!/bin/bash
chunks=1
devices=3
filesize=$(awk "BEGIN{print 16/$devices}")

# prepare the input files
pids=()
for ((i=0;i<$devices;i++)); do
    path=/mnt/nvme$i/test
    if [ ! -e "$path" ]; then
        fio --name=write --rw=write --bs=128k --filename=$path --size=16G &
        pids+=($!)
    fi
done
for pid in ${pids[@]}; do
    wait $pid
done

# run the test
pids=()
for ((i=0;i<$devices;i++)); do
    ./build/test_parallel_chunks_threads -e 0.13 -o 1 -c 16 -d $i -s $filesize -p $chunks -r 2250000 &
    pids+=($!)
done
for pid in ${pids[@]}; do
    wait $pid
done
