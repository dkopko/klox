#!/bin/bash

BIN="${1:-c/BUILD/RelWithDebInfo/klox}"

BENCHMARKS=()
BENCHMARKS+=(test/benchmark/method_call.lox)
BENCHMARKS+=(test/benchmark/equality.lox)
BENCHMARKS+=(test/benchmark/fib.lox)
BENCHMARKS+=(test/benchmark/trees.lox)
#BENCHMARKS+=(test/benchmark/string_equality.lox) FAILING in both clox and klox
BENCHMARKS+=(test/benchmark/zoo.lox)
BENCHMARKS+=(test/benchmark/properties.lox)
BENCHMARKS+=(test/benchmark/invocation.lox)
#BENCHMARKS+=(test/benchmark/instantiation.lox)  Uses too large of a cb ring to keep in memory on my laptop.
#BENCHMARKS+=(test/benchmark/instantiation_2GB_limit.lox)  Sometimes gets killed by oom-killer
BENCHMARKS+=(test/benchmark/instantiation_1GB_limit.lox)
#BENCHMARKS+=(test/benchmark/binary_trees.lox)  Uses too large of a cb ring to keep in memory on my laptop.
#BENCHMARKS+=(test/benchmark/binary_trees_2GB_limit.lox)  Sometimes gets killed by oom-killer
BENCHMARKS+=(test/benchmark/binary_trees_1GB_limit.lox)

export KLOX_RING_SIZE=1073741824  # Sufficiently pre-sized for all benchmark tests.

for b in "${BENCHMARKS[@]}"
do
  rm -rf map-* gc-*
  echo "${b}"
  time "${BIN}" "${b}"
  ls -lh map-* gc-*
done

