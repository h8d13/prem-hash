#!/usr/bin/env python3
"""Run bench binaries at N values, T trials. Print median ns/op + Mops/s.

Single run, logged to results.csv:
    ./bench.py 1m 5m 15m

A/B compare in one shot (deltas inline vs --base):
    make bench_c                                  # baseline build
    cp bench_c bench_c.base                       # snapshot
    <edit ../hash_table8.h or bench_c.c>
    make bench_c                                  # candidate
    ./bench.py --base ./bench_c.base 1m 5m 15m    # base + curr, %delta vs base

Regression tracking across commits:
    ./bench.py 1m 5m 15m                          # appends rows
    <commit, change, rebuild>
    ./bench.py 1m 5m 15m                          # appends new rows
    ./bench.py --compare                          # latest vs prev per N, %delta

History dump:
    ./bench.py --list                             # all rows

History lives in git (commit results.csv) plus the log itself.
"""
import argparse, subprocess, statistics, sys, os, csv, datetime
from collections import defaultdict

COLS   = ["insert", "hit", "miss", "batch", "erase"]
LABELS = {"insert": "insert:", "hit": "lookup-hit:", "miss": "lookup-miss:",
          "batch": "find_batch:", "erase": "erase-half:"}
LOG    = "results.csv"


def parse_n(s):
    s = s.strip().lower()
    if s.endswith("m"): return int(float(s[:-1]) * 1_000_000)
    if s.endswith("k"): return int(float(s[:-1]) * 1_000)
    return int(s)


def fmt_n(n):
    if n >= 1_000_000:
        v = n / 1_000_000
        return f"{v:.0f}M" if v == int(v) else f"{v:.1f}M"
    if n >= 1_000:
        v = n / 1_000
        return f"{v:.0f}K" if v == int(v) else f"{v:.1f}K"
    return str(n)


def run_one(binary, n):
    out = subprocess.check_output([binary, str(n)], text=True)
    row = {}
    for line in out.splitlines():
        for col, lbl in LABELS.items():
            if lbl in line:
                row[col] = float(line.split()[1])
    if len(row) != len(COLS):
        sys.exit(f"error: unexpected output from {binary}")
    return row


def collect(binaries, n, trials):
    data = {b: {c: [] for c in COLS} for b in binaries}
    for _ in range(trials):
        for b in binaries:
            row = run_one(b, n)
            for c in COLS: data[b][c].append(row[c])
    return {b: {c: statistics.median(data[b][c]) for c in COLS} for b in binaries}


def print_section(binaries, labels, medians, header, val_fn):
    base = binaries[0]
    base_vals = {c: val_fn(medians[base][c]) for c in COLS}
    print(f"\n  {header}")
    hdr = f"  {'bin':<14}" + "".join(f"{c:>9}" for c in COLS)
    print(hdr)
    print(f"  {'-' * (len(hdr) - 2)}")
    for b, lbl in zip(binaries, labels):
        vals = {c: val_fn(medians[b][c]) for c in COLS}
        nums = "".join(f"{vals[c]:>9.2f}" for c in COLS)
        if len(binaries) == 1 or b == base:
            print(f"  {lbl:<14}{nums}")
        else:
            pcts = "  " + " ".join(f"{((vals[c]/base_vals[c])-1)*100:>+6.1f}%" for c in COLS)
            print(f"  {lbl:<14}{nums}{pcts}")


def print_degradation(binaries, labels, sizes, all_medians):
    lo, hi = sizes[0], sizes[-1]
    print(f"\n  cache-pressure degradation  ({fmt_n(lo)} -> {fmt_n(hi)}, +%=worse)")
    hdr = f"  {'bin':<14}" + "".join(f"{c:>9}" for c in COLS)
    print(hdr)
    print(f"  {'-' * (len(hdr) - 2)}")
    for b, lbl in zip(binaries, labels):
        lo_m, hi_m = all_medians[lo][b], all_medians[hi][b]
        vals = "".join(f"{((hi_m[c]/lo_m[c])-1)*100:>+8.1f}%" for c in COLS)
        print(f"  {lbl:<14}{vals}")


def append_log(binaries, labels, n, trials, medians):
    exists = os.path.exists(LOG)
    ts = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
    with open(LOG, "a", newline="") as f:
        w = csv.writer(f)
        if not exists:
            w.writerow(["timestamp", "N", "trials", "variant"] + COLS)
        for b, lbl in zip(binaries, labels):
            m = medians[b]
            w.writerow([ts, n, trials, lbl] + [f"{m[c]:.4f}" for c in COLS])


def load_log():
    if not os.path.exists(LOG): return []
    with open(LOG) as f:
        return list(csv.DictReader(f))


def cmd_list():
    rows = load_log()
    if not rows:
        print("log empty")
        return
    print(f"{'timestamp':<22} {'N':>8} {'trials':>6} {'variant':<14}" +
          "".join(f"{c:>9}" for c in COLS))
    for r in rows:
        print(f"{r['timestamp']:<22} {fmt_n(int(r['N'])):>8} {r['trials']:>6} "
              f"{r['variant']:<14}" + "".join(f"{float(r[c]):>9.2f}" for c in COLS))


def cmd_compare(variant):
    rows = load_log()
    if not rows:
        print("log empty")
        return
    groups = defaultdict(list)
    for r in rows:
        if r["variant"] == variant:
            groups[int(r["N"])].append(r)
    if not groups:
        print(f"no rows for variant={variant}")
        return
    any_printed = False
    for n in sorted(groups):
        runs = groups[n]
        if len(runs) < 2: continue
        any_printed = True
        prev, curr = runs[-2], runs[-1]
        print(f"\nN={fmt_n(n)}  prev={prev['timestamp']}  curr={curr['timestamp']}")
        hdr = f"  {'metric':<14}" + "".join(f"{c:>9}" for c in COLS)
        print(hdr)
        print(f"  {'-' * (len(hdr) - 2)}")
        p = {c: float(prev[c]) for c in COLS}
        c_ = {c: float(curr[c]) for c in COLS}
        print(f"  {'prev':<14}" + "".join(f"{p[c]:>9.2f}" for c in COLS))
        print(f"  {'curr':<14}" + "".join(f"{c_[c]:>9.2f}" for c in COLS))
        deltas = "".join(f"{((c_[c]/p[c])-1)*100:>+8.1f}%" for c in COLS)
        print(f"  {'delta':<14} {deltas}")
    if not any_printed:
        print(f"need >=2 rows per N for variant={variant}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sizes", nargs="*", default=["1m", "5m", "15m"],
                    help="N values, e.g. 1m 5m 15m")
    ap.add_argument("--trials", "-t", type=int, default=10)
    ap.add_argument("--bin", default="./bench_c", help="candidate binary (default ./bench_c)")
    ap.add_argument("--base", default=None, help="baseline binary for inline A/B")
    ap.add_argument("--no-save", action="store_true", help="skip appending to results.csv")
    ap.add_argument("--list", "-l", action="store_true", help="dump results.csv")
    ap.add_argument("--compare", "-c", nargs="?", const="bench_c", metavar="VARIANT",
                    help="latest vs previous per N (default variant: bench_c)")
    args = ap.parse_args()

    if args.list:    cmd_list();    return
    if args.compare: cmd_compare(args.compare); return

    binaries = [args.bin]
    labels = [os.path.basename(args.bin)]
    if args.base:
        binaries = [args.base, args.bin]
        labels = [os.path.basename(args.base), os.path.basename(args.bin)]
    for b in binaries:
        if not os.access(b, os.X_OK):
            sys.exit(f"error: {b} not executable. Run: make")

    sizes = [parse_n(s) for s in args.sizes]
    all_medians = {}
    for n in sizes:
        print(f"\nN={fmt_n(n)}  {args.trials} trials", flush=True)
        medians = collect(binaries, n, args.trials)
        all_medians[n] = medians
        print_section(binaries, labels, medians, "ns/op  (lower-better)", lambda x: x)
        print_section(binaries, labels, medians, "Mops/s  (higher-better)", lambda x: 1000.0 / x)
        if not args.no_save:
            append_log(binaries, labels, n, args.trials, medians)

    if len(sizes) > 1:
        print_degradation(binaries, labels, sizes, all_medians)

    if not args.no_save and sizes:
        print(f"\nappended to {LOG}  (--list to review, --no-save to skip, --compare to diff vs prev)")


if __name__ == "__main__":
    main()
