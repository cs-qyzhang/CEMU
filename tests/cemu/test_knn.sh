#!/bin/bash
chunks=(1 2 4)
parallel=3

echo "=========================================="
echo "            Single CSD tests"
echo "=========================================="
# run the test
for ((i=0;i<$parallel;i++)); do
    echo "----------- $((chunks[i])) concurrent chunks -----------"
    ./build/cemu_benchmark -l ./build/knn.so -n knn -e 0.13 -r 2000000 -o 1 -p "${chunks[i]}" -c 16 -s 16 -d 0
    echo ""
done

echo "=========================================="
echo "            Three CSDs tests"
echo "=========================================="
for ((i=0;i<$parallel;i++)); do
    echo "----------- $((chunks[i])) concurrent chunks -----------"
    ./build/cemu_benchmark -l ./build/knn.so -n knn -e 0.13 -r 2000000 -o 1 -p "${chunks[i]}" -c 16 -s 5.33 -d 0,1,2
    echo ""
done
