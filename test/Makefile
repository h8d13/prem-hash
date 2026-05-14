CC     = gcc
CFLAGS  = -std=c11  -D_POSIX_C_SOURCE=200809L -O3 -Wall -Wextra -I..
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g
HEADER  = ../hash_table8.h

.PHONY: all test bench clean test-san

all: bench_c test_strkeys test_lazy_ctrl test_oom test_oom_ndebug test_replay op_gen

bench_c: bench_c.c $(HEADER)
	$(CC) $(CFLAGS) -march=native $< -o $@

test_strkeys: test_strkeys.c $(HEADER)
	$(CC) $(CFLAGS) $< -o $@

test_lazy_ctrl: test_lazy_ctrl.c $(HEADER)
	$(CC) $(CFLAGS) $< -o $@

test_oom: test_oom.c $(HEADER)
	$(CC) $(CFLAGS) $< -o $@

test_oom_ndebug: test_oom.c $(HEADER)
	$(CC) $(CFLAGS) -DNDEBUG $< -o $@

test_replay: test_replay.c $(HEADER)
	$(CC) $(CFLAGS) $< -o $@

op_gen: op_gen.c
	$(CC) $(CFLAGS) $< -o $@

test: test_strkeys test_lazy_ctrl test_oom test_oom_ndebug test_replay op_gen
	./op_gen /tmp/emh_ops.bin 500000 5000
	./test_strkeys
	./test_lazy_ctrl
	./test_oom
	./test_oom_ndebug
	./test_replay /tmp/emh_ops.bin
	@echo "Tests passed."

# Sanitizer pass: rebuild every test with ASAN+UBSAN, rerun. Slower (~10x)
# but catches use-after-free, double-free, oob, signed overflow, etc.
# Specifically validates is_main flag coherence through long op sequences.
test-san:
	$(CC) $(CFLAGS) $(SAN_FLAGS) test_strkeys.c   -o test_strkeys.san
	$(CC) $(CFLAGS) $(SAN_FLAGS) test_lazy_ctrl.c -o test_lazy_ctrl.san
	$(CC) $(CFLAGS) $(SAN_FLAGS) test_replay.c    -o test_replay.san
	$(CC) $(CFLAGS) op_gen.c -o op_gen
	./op_gen /tmp/emh_ops_san.bin 500000 5000
	./test_strkeys.san
	./test_lazy_ctrl.san
	./test_replay.san /tmp/emh_ops_san.bin
	@echo "Sanitizer tests passed."
	rm -f test_strkeys.san test_lazy_ctrl.san test_replay.san

# SIZES: space-separated N values passed to bench.py, e.g. make bench SIZES="1m 10m"
SIZES ?= 1m
bench: test bench_c
	./bench.py $(SIZES)

profile: bench_c.c $(HEADER)
	$(CC) $(CFLAGS) -march=native -g -fno-omit-frame-pointer $< -o bench_c

clean:
	rm -f bench_c test_strkeys test_lazy_ctrl test_oom test_oom_ndebug test_replay op_gen *.san
