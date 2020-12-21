#!/bin/bash

flush() {
  sync
  echo 3 > /proc/sys/vm/drop_caches
  echo 1 > /proc/sys/vm/compact_memory
  echo 3 > /proc/sys/vm/drop_caches
  echo 1 > /proc/sys/vm/compact_memory
}

set -eo pipefail

for i in 1 2 3 4; do
  for s in 4 16 64 256; do
    echo "${i} devices ${s}K stripe size"
    insmod cheedon.ko
    echo $((64 * 1024 * 1024 * 1024)) > /sys/block/cheedon0/disksize

    gcc -O3 -s -DNUM_DEVICE=$i -DSTRIPE_K=$s user.c 2>/dev/null
    #gcc -O3 -s -DNUM_DEVICE=$i -DSTRIPE_K=$s uring.c -luring 2>/dev/null
    ./build.sh
    ./a.out &
    sleep 0.5

    cd fio
    ls */* | while read a; do
      flush
      echo -n $a; fio $a | grep 'B/s' | tail -n1; echo
    done
    cd ..

    killall a.out
    wait
    rmmod cheedon

    echo
  done
done
