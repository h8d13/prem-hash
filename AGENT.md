# prem-hash

C port of `emhash8::HashMap` (fork of [ktprime/emhash](https://github.com/ktprime/emhash)).
Single header, stb-style include-twice, MIT.

## Layout

```
hash_table8.h     header-only library, 1 TU
test/
  bench_c.c       insert / hit / miss / find_batch / erase-half harness
  bench.py        run a bench binary, T trials, print median ns/op + Mops/s
  test_strkeys.c  owned-key (strdup) lifecycle + leak balance
  test_lazy_ctrl.c rev_bucket staleness regression + erase stress
  op_gen.c        deterministic op stream for replay testing
  Makefile        gcc -O3 -march=native
  profile.sh      perf stat + perf record (pinned CPU0)
```

## Build / test

```
cd test
make           # build all
make test     # run test_strkeys + test_lazy_ctrl + op_gen smoke
make bench    # build + run bench.py at SIZES (default 1m)
make profile  # debug-symbols rebuild for perf record
```

## Perf flags

Global defines, set before first include. See `hash_table8.h:10-18` for the canonical list.

| flag                   | effect                                                       | cost                |
|------------------------|--------------------------------------------------------------|---------------------|
| `EMH_HOIST_FP`         | hoist fingerprint out of probe loop                         | none, currently set in `bench_c.c` |
| `EMH_OPT_ALIGNED_ALLOC`| 64B-align `_index`/`_pairs` + assume_aligned                | requires C11 `aligned_alloc` |
| `EMH_HASH_AESNI`       | AES-NI 2-round hash for u64/u32 keys                        | needs `-maes` / `__AES__` |
| `EMH_SIMD_PROBE_GROUPS`| how many SIMD groups to scan in `find_empty_bucket`         | default 4 |
| `EMH_SIZE_T`           | bucket index width, default `uint32_t`                      | use `uint64_t` for > 2^31 entries |
| `EMH_MALLOC`/`EMH_FREE`| custom allocator                                            |  |
| `EMH_KEY_COPY`/`EMH_KEY_DESTROY`, `EMH_VAL_*` | ownership hooks (per specialization)         | strdup'd keys, etc. |

## Bench

`bench.py` runs one binary (or two with `--base` for inline A/B), appends
medians to `results.csv`, and prints per-N ns/op + Mops/s tables plus a
cache-pressure degradation table across the lowest -> highest N.

**Inline A/B (one shot, deltas in the same output):**
```
cd test
make bench_c                                  # baseline
cp bench_c bench_c.base                       # snapshot
<edit ../hash_table8.h or bench_c.c>
make bench_c                                  # candidate
./bench.py --base ./bench_c.base 1m 5m 15m    # base + curr, %delta vs base

# accept:  rm bench_c.base ; git add -p ; git commit
# reject:  git restore ../hash_table8.h ; make bench_c ; rm bench_c.base
```

**Regression tracking across commits (CSV history):**
```
./bench.py 1m 5m 15m                          # appends rows for bench_c
<commit, edit, rebuild>
./bench.py 1m 5m 15m                          # appends new rows
./bench.py --compare                          # latest vs previous per N
./bench.py --list                             # dump all rows
./bench.py 1m --no-save                       # one-off, skip log
```

Sizes: `1m`, `500k`, raw int. Default `1m 5m 15m`. Trials default 10 (`-t N`).
`--compare VARIANT` switches the compared row tag (default `bench_c`).
Commit `results.csv` alongside code changes so the log lives in git too.

## Conventions

- Branch: `dot-c` is the C port (current). `main` is the older C++ patch series.
- Commits: `type(scope): subject`. Squash WIP before merging dot-c -> main.
- POD K/V assumed (memcpy on move). For owned types use `EMH_KEY_COPY` / `EMH_KEY_DESTROY`. Rehash is unsafe for non-trivially-movable types; pre-`_reserve` if you store anything refcounted or self-referential.
- OOM = `abort` via `assert(p)` on alloc. Note: `NDEBUG` builds will deref NULL instead; if you ship this, surface alloc failure properly.

## Known gaps

- No CI.
- No fuzz / no cross-impl replay harness (op_gen output is generated but no consumer left after the C++ removal).
- No seeded hash; `emh_hash_str` is HashDoS-able for attacker-controlled keys.
- `find_empty_bucket` `csize` param is unused.
