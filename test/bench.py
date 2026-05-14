#!/usr/bin/env python3
"""Benchmark bench_c and any candidate variant. Build with `make` first.

Usage:
    ./bench.py [N ...]            # run and save, e.g. 1m  or  1m 5m 10m
    ./bench.py --trials 30 1m 10m
    ./bench.py --list             # dump all saved rows
    ./bench.py --compare          # latest vs previous run per N (iteration diff)
    ./bench.py --promote [VAR]    # cp bench_VAR -> bench_c, rm bench_VAR (default: cand)
    ./bench.py --discard [VAR]    # rm bench_VAR (default: cand)

N accepts: raw int, K suffix, M suffix (e.g. 500k, 1m, 10m).
Drop a bench_cand binary next to bench_c to auto-compare on the next run.
"""
import argparse, subprocess, statistics, sys, os, csv, datetime, shutil

COLS   = ["insert", "hit", "miss", "batch", "erase"]
LABELS = {
    "insert": "insert:",
    "hit":    "lookup-hit:",
    "miss":   "lookup-miss:",
    "batch":  "find_batch:",
    "erase":  "erase-half:",
}
LOG = "results.csv"


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def get_variants():
    variants = ["c"]
    for f in sorted(os.listdir(".")):
        if f.startswith("bench_") and os.access(f, os.X_OK) and f != "bench_c":
            name = f[len("bench_"):]
            if name not in variants:
                variants.append(name)
    return variants


def check_binaries(variants):
    missing = [v for v in variants if not os.access(f"./bench_{v}", os.X_OK)]
    if missing:
        die(f"missing: {', '.join(f'bench_{v}' for v in missing)}. Run: make")


def run_one(variant, n):
    out = subprocess.check_output([f"./bench_{variant}", str(n)], text=True)
    row = {}
    for line in out.splitlines():
        for col, lbl in LABELS.items():
            if lbl in line:
                row[col] = float(line.split()[1])
    if len(row) != len(COLS):
        die(f"unexpected output from bench_{variant}")
    return row


def collect(variants, n, trials):
    data = {v: {c: [] for c in COLS} for v in variants}
    for _ in range(trials):
        for v in variants:
            row = run_one(v, n)
            for c in COLS:
                data[v][c].append(row[c])
    return {v: {c: statistics.median(data[v][c]) for c in COLS} for v in variants}


def fmt_n(n):
    if n >= 1_000_000:
        v = n / 1_000_000
        return f"{v:.0f}M" if v == int(v) else f"{v:.1f}M"
    if n >= 1_000:
        v = n / 1_000
        return f"{v:.0f}K" if v == int(v) else f"{v:.1f}K"
    return str(n)


def parse_n(s):
    s = s.strip().lower()
    if s.endswith("m"):
        return int(float(s[:-1]) * 1_000_000)
    if s.endswith("k"):
        return int(float(s[:-1]) * 1_000)
    return int(s)


def _print_section(variants, medians, label, val_fn):
    base_vals = {c: val_fn(medians["c"][c]) for c in COLS}
    hdr = f"  {'variant':<12}" + "".join(f"{c:>8}" for c in COLS)
    print(f"\n  {label}")
    print(hdr)
    print(f"  {'-' * (len(hdr) - 2)}")
    for v in variants:
        vals = {c: val_fn(medians[v][c]) for c in COLS}
        nums = "".join(f"{vals[c]:>8.2f}" for c in COLS)
        if v == "c":
            print(f"  {v:<12}{nums}")
        else:
            pcts = "  " + " ".join(f"{((vals[c]/base_vals[c])-1)*100:>+6.1f}%" for c in COLS)
            print(f"  {v:<12}{nums}{pcts}")


def print_table(variants, n, trials, medians):
    print(f"\nN={fmt_n(n)}  {trials} trials")
    _print_section(variants, medians, "ns/op  (lower-better)", lambda x: x)
    _print_section(variants, medians, "Mops/s  (higher-better)", lambda x: 1000.0 / x)


def append_log(variants, n, trials, medians):
    exists = os.path.exists(LOG)
    ts = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
    with open(LOG, "a", newline="") as f:
        w = csv.writer(f)
        if not exists:
            w.writerow(["timestamp", "N", "trials", "variant"] + COLS)
        for v in variants:
            m = medians[v]
            w.writerow([ts, n, trials, v] + [f"{m[c]:.4f}" for c in COLS])


def load_log():
    if not os.path.exists(LOG):
        return []
    with open(LOG) as f:
        return list(csv.DictReader(f))


def cmd_compare():
    rows = load_log()
    if not rows:
        print("No results saved yet.")
        return

    from collections import defaultdict
    groups = defaultdict(list)
    for r in rows:
        groups[(int(r["N"]), r["variant"])].append(r)

    seen_n = []
    for n, _v in groups:
        if n not in seen_n:
            seen_n.append(n)

    all_variants = sorted({v for _, v in groups})

    any_printed = False
    for n in seen_n:
        b_runs = groups.get((n, "c"), [])
        if len(b_runs) < 2:
            continue

        any_printed = True
        prev_ts = b_runs[-2]["timestamp"]
        curr_ts = b_runs[-1]["timestamp"]
        print(f"\nN={fmt_n(n)}  prev={prev_ts}  curr={curr_ts}")
        hdr = f"  {'variant':<12}" + "".join(f"{c:>8}" for c in COLS)
        print(hdr)
        print(f"  {'-' * (len(hdr) - 2)}")

        for v in all_variants:
            runs = groups.get((n, v), [])
            if len(runs) < 2:
                print(f"  {v:<12}  (only one run)")
                continue
            prev = {c: float(runs[-2][c]) for c in COLS}
            curr = {c: float(runs[-1][c]) for c in COLS}
            curr_nums = "".join(f"{curr[c]:>8.2f}" for c in COLS)
            deltas = "  " + " ".join(
                f"{((curr[c]/prev[c])-1)*100:>+6.1f}%" for c in COLS
            )
            print(f"  {v:<12}{curr_nums}{deltas}")

    if not any_printed:
        print("Need at least 2 runs per N to compare. Run bench again after a change.")


def cmd_list():
    rows = load_log()
    if not rows:
        print("Log is empty.")
        return

    print(f"{'timestamp':<22} {'N':>8} {'trials':>6} {'variant':<12}" +
          "".join(f"{c:>8}" for c in COLS))
    for r in rows:
        print(f"{r['timestamp']:<22} {fmt_n(int(r['N'])):>8} {r['trials']:>6} "
              f"{r['variant']:<12}" + "".join(f"{float(r[c]):>8.2f}" for c in COLS))


def cmd_promote(var):
    src = f"./bench_{var}"
    if not os.access(src, os.X_OK):
        die(f"bench_{var} not found or not executable")
    shutil.copy2(src, "./bench_c")
    os.remove(src)
    print(f"Promoted bench_{var} -> bench_c, removed bench_{var}")


def cmd_discard(var):
    src = f"./bench_{var}"
    if not os.path.exists(src):
        die(f"bench_{var} not found")
    os.remove(src)
    print(f"Discarded bench_{var}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sizes", nargs="*", default=["1m", "5m", "15m", "45m"],
                    help="N values to test, e.g. 1m 5m 10m")
    ap.add_argument("--trials", "-t", type=int, default=10,
                    help="trials per variant per N (default: 10)")
    ap.add_argument("--no-save", action="store_true",
                    help="skip appending to results.csv")
    ap.add_argument("--list", "-l", action="store_true",
                    help="dump all saved rows")
    ap.add_argument("--compare", "-c", action="store_true",
                    help="latest vs previous run per N (iteration diff)")
    ap.add_argument("--promote", nargs="?", const="cand", metavar="VAR",
                    help="cp bench_VAR -> bench_c and remove it (default VAR: cand)")
    ap.add_argument("--discard", nargs="?", const="cand", metavar="VAR",
                    help="rm bench_VAR (default VAR: cand)")
    args = ap.parse_args()

    if args.list:
        cmd_list()
        return

    if args.compare:
        cmd_compare()
        return

    if args.promote:
        cmd_promote(args.promote)
        return

    if args.discard:
        cmd_discard(args.discard)
        return

    variants = get_variants()
    check_binaries(variants)
    sizes = [parse_n(s) for s in args.sizes]

    for n in sizes:
        label = fmt_n(n)
        print(f"Running {len(variants)} variant(s) x {args.trials} trials @ N={label} ...",
              end="", flush=True)
        medians = collect(variants, n, args.trials)
        print(" done")
        print_table(variants, n, args.trials, medians)
        if not args.no_save:
            append_log(variants, n, args.trials, medians)

    if not args.no_save and sizes:
        print(f"\nSaved to {LOG}  (--list to review, --no-save to skip)")


if __name__ == "__main__":
    main()
