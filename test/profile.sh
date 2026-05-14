#!/usr/bin/env bash
# Usage: ./profile.sh [N]   (default: 5m)
# Requires: perf, bench_c built with debug symbols (make profile)
N=${1:-5m}
BIN=./bench_c

if ! command -v perf &>/dev/null; then
    echo "error: perf not found" >&2
    exit 1
fi
if [ ! -x "$BIN" ]; then
    echo "error: $BIN not found. Run: make" >&2
    exit 1
fi

# Parse N suffix so bench_c gets a raw integer.
parse_n() {
    local s="${1,,}"
    if [[ "$s" == *m ]]; then echo $(( ${s%m} * 1000000 ))
    elif [[ "$s" == *k ]]; then echo $(( ${s%k} * 1000 ))
    else echo "$s"
    fi
}
N_RAW=$(parse_n "$N")

# Pin to CPU 0 (P-core on hybrid Intel) so counters land on the right PMU.
PIN="taskset -c 0"

echo "=== perf stat (cache counters) N=$N ==="
perf stat -e cycles,instructions,cache-references,cache-misses,\
L1-dcache-loads,L1-dcache-load-misses,\
LLC-loads,LLC-load-misses \
$PIN "$BIN" "$N_RAW"

echo ""
echo "=== perf record + report (hottest functions) N=$N ==="
perf record -g --call-graph dwarf -o /tmp/perf_bench.data $PIN "$BIN" "$N_RAW"
perf report --stdio -i /tmp/perf_bench.data
rm -f /tmp/perf_bench.data
