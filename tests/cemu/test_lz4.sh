#!/bin/bash
chunks=(1 2 4)
parallel=3

echo "=========================================="
echo "            Single CSD tests"
echo "=========================================="
# run the test
for ((i=0;i<$parallel;i++)); do
    echo "----------- $((chunks[i])) concurrent chunks -----------"
    ./build/cemu_benchmark -l ./build/lz4.so -n lz4 -e 1.0 -p "${chunks[i]}" -c 16 -s 16 -d 0
    echo ""
done

echo "=========================================="
echo "            Three CSDs tests"
echo "=========================================="
for ((i=0;i<$parallel;i++)); do
    echo "----------- $((chunks[i])) concurrent chunks -----------"
    ./build/cemu_benchmark -l ./build/lz4.so -n lz4 -e 1.0 -p "${chunks[i]}" -c 16 -s 5.33 -d 0,1,2
    echo ""
done
