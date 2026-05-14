# prem-hash

C port of `emhash8::HashMap` (fork of [ktprime/emhash](https://github.com/ktprime/emhash)).
Single header, stb-style include-twice, MIT.

## Layout

```
hash_table8.h        header-only library, 1 TU
test/
  bench_c.c          insert / hit / miss / find_batch / erase-half harness
  bench.py           run a bench binary, T trials, print median ns/op + Mops/s
  test_strkeys.c     owned-key (strdup) lifecycle + leak balance
  test_lazy_ctrl.c   erase chain-rewiring regression + stress
  test_oom.c         alloc failure aborts cleanly (run twice: default + -DNDEBUG)
  test_replay.c      replay op_gen output against independent reference
  op_gen.c           deterministic op stream generator
  Makefile           gcc -O3 -march=native
  profile.sh         perf stat + perf record (pinned CPU0)
```

## Build / test

```
cd test
make           # build all
make test      # full test suite (strkeys, lazy_ctrl, oom both modes, replay)
make test-san  # rebuild + run under ASAN+UBSAN (slower, catches UAF/oob/UB)
make bench     # build + run bench.py at SIZES (default 1m)
make profile   # debug-symbols rebuild for perf record
```

## Required per-specialization opt-in

Each map specialization MUST declare its memory model before the second include:

```c
#define EMH_NAME imap
#define EMH_KEY  uint32_t
#define EMH_VAL  uint32_t
#define EMH_HASH(k) emh_hash_u32(k)
#define EMH_POD_KV                       /* K and V are bitwise-copyable */
#include "hash_table8.h"
```

OR for owned types (strdup model):

```c
#define EMH_NAME smap
#define EMH_KEY  char*
#define EMH_VAL  uint32_t
#define EMH_HASH(k) emh_hash_str(k, strlen(k))
#define EMH_EQ(a,b) (strcmp(a,b) == 0)
#define EMH_KEY_COPY(d,s)    ((d) = strdup(s))
#define EMH_KEY_DESTROY(k)   free((char*)(k))
#include "hash_table8.h"
```

Omitting both is a compile error. Rationale: rehash moves pairs via raw
`memcpy` which is unsafe for types with self-pointers (SSO strings,
intrusive nodes, refcounted handles). The opt-in makes the invariant
load-bearing instead of a comment in the header.

## Perf flags

Global defines, set before first include. See `hash_table8.h` preamble for canonical list.

| flag                   | effect                                                       | cost                |
|------------------------|--------------------------------------------------------------|---------------------|
| `EMH_SIMD_PROBE_GROUPS`| how many SIMD groups to scan in `find_empty_bucket`         | default 4 |
| `EMH_SIZE_T`           | bucket index width, default `uint32_t`                      | use `uint64_t` for > 2^31 entries |
| `EMH_MALLOC`/`EMH_FREE`| custom allocator                                            |  |
| `EMH_KEY_COPY`/`EMH_KEY_DESTROY`, `EMH_VAL_*` | ownership hooks (per specialization)         | strdup'd keys, etc. |

AES-NI hash is auto-selected when `__AES__` is defined (compile with `-maes`
or `-march=native` on an AES-capable CPU). No explicit flag for it.

## OOM behavior

Allocation failure aborts via `abort()` (consistent across debug and NDEBUG
builds). If you need propagating error returns, wrap `EMH_MALLOC` to retry
or shed load before the table sees `NULL`, since past the `abort()` site we
do not recover. `test_oom.c` verifies the abort path works under both
`-DNDEBUG` and the default build.

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

- Branch: `main` is the C port (merged from `dot-c` in PR #1, 2026-05). The
  earlier C++ patch series lives in pre-merge history before that commit.
- Commits: `type(scope): subject`.

## Serial lookup: use `_prefetch` for ~30-40% wins

The single biggest hot spot at large N is the `_index[bucket]` cache miss on the
critical path (`perf annotate` shows the `test`/`je` consuming the idx load
accounts for ~37% of total cycles). The OoO core cannot speculate the address
ahead of time because each lookup hashes to an independent bucket.

A serial loop hides this by stride-prefetching the future key, the same trick
`find_batch` uses internally:

```c
enum { PF_STRIDE = 40 };
for (size_t i = 0; i < n; ++i) {
    if (i + PF_STRIDE < n) imap_prefetch(&m, keys[i + PF_STRIDE]);
    uint32_t v;
    if (imap_get(&m, keys[i], &v)) sink += v;
}
```

Measured on Raptor Lake i5-14600KF, N=15M splitmix u32 keys:

| op          | naive     | with prefetch | win    |
|-------------|-----------|---------------|--------|
| lookup-hit  | 10.21 ns  | 6.75 ns       | -33.9% |
| lookup-miss | 11.30 ns  | 6.62 ns       | -41.4% |

`miss-pf` cache-pressure degradation (1M -> 15M) collapses from +41% to +8% --
prefetched serial scans are nearly cache-flat. `find_batch` does the same
prefetch internally but pays array-return overhead; a simple `_prefetch + _get`
serial loop matches or beats it for in-flight code.

The standard bench measures the prefetched pattern under `lookup-hit:` / `lookup-miss:`
columns. Naive serial `_get` would be ~50% slower at large N.

## is_main flag (ctrl byte bit 0)

Inserts cache an `is_main` bit in `ctrl[bucket]`: bit 7 = empty, bit 0 = "occupant is
in its own main bucket". `find_or_allocate` and `find_unique_bucket` read this byte
to skip the `pairs[slot].first` load + hash recomputation that previously decided
"kick out or chain extend." Wins ~5-10% on int-key inserts, more on string-key inserts
where the saved hash is wyhash. Maintained at every emit / kickout / rehash site;
sanitizer-clean over the test suite.

## Known gaps

- No CI runner (Makefile targets exist; just no GitHub Actions workflow).
- No seeded hash; `emh_hash_str` uses fixed secrets, HashDoS-able for attacker-controlled keys.
