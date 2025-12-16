#!/bin/bash
device=8

# run the test
for ((i=0;i<$device;i++)); do
	echo "Run $((i+1)) CSD tests"
    echo "=========================================="
    echo "             $((i+1)) CSD tests"
    echo "=========================================="
    dev_list=$(seq 0 $((i)) | paste -sd,)
    ./build/cemu_benchmark -l ./build/lz4.so -n lz4 -e 1.0 -p 1 -c 16 -s 4 -d "$dev_list"
    echo ""
done

