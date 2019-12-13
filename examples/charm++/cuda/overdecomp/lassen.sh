#!/bin/bash

vector_size=16777216

for sp in 1 2 4 8 16 32 64 128 256
do
  jsrun -n1 -a1 -c8 -g1 -K1 -r1 nvprof -o vecadd-charm-16m-sp"$sp".nvvp ./vecadd -n $vector_size -s $sp ++ppn 8 +setcpuaffinity +showcpuaffinity
done
