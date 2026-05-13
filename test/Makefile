CC     = gcc
CXX    = g++
CFLAGS  = -std=c11  -D_POSIX_C_SOURCE=200809L -O3 -Wall -Wextra -I..
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -I..
HEADER  = ../hash_table8.h
HPPHEADER = ../hash_table8.hpp

.PHONY: all test bench clean

all: bench_c bench_cpp test_strkeys op_gen

bench_c: bench_c.c $(HEADER)
	$(CC) $(CFLAGS) $< -o $@

bench_cpp: bench_cpp.cpp $(HPPHEADER)
	$(CXX) $(CXXFLAGS) $< -o $@

test_strkeys: test_strkeys.c $(HEADER)
	$(CC) $(CFLAGS) $< -o $@

op_gen: op_gen.c
	$(CC) $(CFLAGS) $< -o $@

# SIZES: space-separated N values passed to bench.py, e.g. make bench SIZES="1m 10m"
SIZES ?= 1m
bench: bench_c bench_cpp
	./bench.py $(SIZES)

test: test_strkeys op_gen
	./op_gen /tmp/emh_ops.bin 50000 10000
	./test_strkeys
	@echo "Tests passed."

clean:
	rm -f bench_c bench_cpp test_strkeys op_gen
