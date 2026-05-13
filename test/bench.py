#!/usr/bin/env python3
"""Compare bench_c (C port) vs bench_cpp (C++ original). Build with `make` first.

Usage:
    ./bench.py [N ...]            # run and save, e.g. 1m  or  1m 5m 10m
    ./bench.py --trials 30 1m 10m
    ./bench.py --list             # dump all saved rows
    ./bench.py --compare          # latest vs previous run per N (iteration diff)

N accepts: raw int, K suffix, M suffix (e.g. 500k, 1m, 10m).
"""
import argparse, subprocess, statistics, sys, os, csv, datetime

VARIANTS = ["c", "cpp"]
COLS     = ["insert", "hit", "miss", "batch", "erase"]
LABELS   = {
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


def check_binaries():
    missing = [v for v in VARIANTS if not os.access(f"./bench_{v}", os.X_OK)]
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


def collect(n, trials):
    data = {v: {c: [] for c in COLS} for v in VARIANTS}
    for _ in range(trials):
        for v in VARIANTS:
            row = run_one(v, n)
            for c in COLS:
                data[v][c].append(row[c])
    return {v: {c: statistics.median(data[v][c]) for c in COLS} for v in VARIANTS}


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


def print_table(n, trials, medians):
    base = medians["c"]
    print(f"\nN={fmt_n(n)}  {trials} trials  (median ns/op, lower=better)")
    hdr = f"  {'variant':<12}" + "".join(f"{c:>8}" for c in COLS)
    print(hdr)
    print(f"  {'-' * (len(hdr) - 2)}")
    for v in VARIANTS:
        m = medians[v]
        nums = "".join(f"{m[c]:>8.2f}" for c in COLS)
        if v == "c":
            print(f"  {v:<12}{nums}")
        else:
            pcts = "  " + " ".join(f"{((m[c]/base[c])-1)*100:>+6.1f}%" for c in COLS)
            print(f"  {v:<12}{nums}{pcts}")


def append_log(n, trials, medians):
    exists = os.path.exists(LOG)
    ts = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
    with open(LOG, "a", newline="") as f:
        w = csv.writer(f)
        if not exists:
            w.writerow(["timestamp", "N", "trials", "variant"] + COLS)
        for v in VARIANTS:
            m = medians[v]
            w.writerow([ts, n, trials, v] + [f"{m[c]:.4f}" for c in COLS])


def load_log():
    if not os.path.exists(LOG):
        return []
    with open(LOG) as f:
        return list(csv.DictReader(f))


def cmd_compare():
    """Show latest vs previous run per (N, variant), with per-col deltas."""
    rows = load_log()
    if not rows:
        print("No results saved yet.")
        return

    # Group by (N, variant), preserving insertion order (= time order).
    from collections import defaultdict
    groups = defaultdict(list)
    for r in rows:
        groups[(int(r["N"]), r["variant"])].append(r)

    # Collect distinct N values in order of first appearance.
    seen_n = []
    for n, _v in groups:
        if n not in seen_n:
            seen_n.append(n)

    any_printed = False
    for n in seen_n:
        # Need at least two runs of the C port to show a diff.
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

        for v in VARIANTS:
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
        print("Need at least 2 runs per N to compare. Run `make bench` again after a change.")


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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sizes", nargs="*", default=["1m"],
                    help="N values to test, e.g. 1m 5m 10m")
    ap.add_argument("--trials", "-t", type=int, default=15,
                    help="trials per variant per N (default: 15)")
    ap.add_argument("--no-save", action="store_true",
                    help="skip appending to results.csv")
    ap.add_argument("--list", "-l", action="store_true",
                    help="dump all saved rows")
    ap.add_argument("--compare", "-c", action="store_true",
                    help="latest vs previous run per N (iteration diff)")
    args = ap.parse_args()

    if args.list:
        cmd_list()
        return

    if args.compare:
        cmd_compare()
        return

    check_binaries()
    sizes = [parse_n(s) for s in args.sizes]

    for n in sizes:
        label = fmt_n(n)
        print(f"Running {len(VARIANTS)} variants x {args.trials} trials @ N={label} ...",
              end="", flush=True)
        medians = collect(n, args.trials)
        print(" done")
        print_table(n, args.trials, medians)
        if not args.no_save:
            append_log(n, args.trials, medians)

    if not args.no_save and sizes:
        print(f"\nSaved to {LOG}  (--list to review, --no-save to skip)")


if __name__ == "__main__":
    main()
