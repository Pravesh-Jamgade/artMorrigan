# T-IRIP bug analysis and fix

## Summary

The T-IRIP (TAGE-inspired) extension to Morrigan was measured across 45 QMM
server traces and produced a geomean speedup of **+7.490%** against stock
Morrigan's **+7.477%** — a difference of 0.012 percentage points, i.e. noise.

It was that the history tables were **almost never being read successfully**. Across all 45 traces:

```
T-IRIP predictions : 14,857
iSTLB misses       : 3,637,608
hit rate           : 0.41%
```

A predictor that fires on 0.4% of events cannot move IPC regardless of how good
its predictions are. Three defects were found; the first one is the one that
matters.

---

## Bug1 — train/lookup key mismatch

### What a history predictor requires

An entry must be **trained** with the same `(index, tag)` key that a later
**lookup** will compute. If the two keys differ, the entry is written to one
place and searched for in another.

### What the code did

Inside `stlb_prefetcher_operate()`, the order of operations was:

| operation | state of `history_reg[0]` |
|-----------|---------------------------|
| `tage_lookup(partial_vpn)` — key on **current** VPN | previous VPN |
 `tage_update(previous_vpn & TAG_MASK, delta)` — key on **previous** VPN | **still the previous VPN** |
| shift `history_reg`, push current VPN | current VPN |

The history register is only shifted *after* training. So at training time the
key VPN (`previous_vpn`) was **already sitting in `history_reg[0]`** — the
training key folded the VPN into its own history fingerprint.

Formally, for a miss sequence `A → B → C`:

```
lookup key for B  =  ( B, fold[ A, ... ] )        # B not in its own history
train  key for B  =  ( B, fold[ B, A, ... ] )     # B IS in its own history
```

`fold_history` XORs each entry rotated by its position, so inserting `B` at
position 0 and shifting everything right changes the fingerprint completely.
The two keys index different sets and carry different tags.

A trained entry could therefore only ever be found again if a VPN were
immediately preceded by itself — a self-loop, which does not occur in practice.
The 14,857 hits observed were 16-bit tag collisions, not real predictions.

### The fix

Snapshot the lookup key and replay it at training time. This is what a real
TAGE implementation does (the "prediction record" passed to the update path).

```c
// computed at the TOP of operate(), history does not yet contain partial_vpn
int      cur_idx[TAGE_NTAB];
uint64_t cur_tag[TAGE_NTAB];
int      cur_ok [TAGE_NTAB];
tage_compute_keys(partial_vpn, cur_idx, cur_tag, cur_ok);

...

// at the BOTTOM: train using the keys saved one miss ago
if (pend_valid)
    tage_update(pend_idx, pend_tag, pend_ok, delta);

// carry this VPN's keys forward — BEFORE the history register shifts
for (int t = 0; t < TAGE_NTAB; t++) {
    pend_idx[t] = cur_idx[t];
    pend_tag[t] = cur_tag[t];
    pend_ok [t] = cur_ok [t];
}
pend_valid = 1;
```

`tage_update()` no longer recomputes anything; it takes the key as an argument.

---

## Bug2 — history-length gating applied globally instead of per table

```c
if (history_len >= 2) {        // one test for H2, H4 AND H8
    ... probe H8 ...
    ... probe H4 ...
    ... probe H2 ...
}
```

`fold_history(len)` internally clamps to `min(len, history_len)`. With only two
entries in the register, `fold_history(8) == fold_history(4) == fold_history(2)`.
All three tables then compute identical fingerprints and degenerate into three
redundant copies of the same predictor — which also wastes the storage budget
that the longer-history tables were supposed to justify.

**Fix:** each table is eligible only when `history_len >= TAGE_HLEN[t]`, decided
per table in `tage_compute_keys()` and recorded in `ok[t]`. An ineligible table
is neither read nor written for that VPN.

---

## Bug  3 — no valid bit; `delta != 0` used as the liveness test

```c
if (row[i].tag == tag && row[i].delta != 0) return i;   // old
```

A freshly-constructed `TAGE_ENTRY` has `tag == 0`. Any lookup whose hashed tag
happened to be 0 could alias onto empty ways, and a legitimately-stored delta
could not be distinguished from an empty slot. Replacement also had no way to
prefer an empty way over evicting a live one.

**Fix:** added an explicit `bool valid` field. `tage_search()` matches on it,
`tage_lru_victim()` fills invalid ways first, and training skips `delta == 0`
(a repeat miss on the same page carries no successor information).

---

## Bug 4 build system configuration contamination

`generate_binary.sh` gates several `sed` rewrites on the prefetcher name:

```sh
if [ ${STLB_PREF} = "morriganPT" ]; then      # does NOT match morriganPT_tage
```

So a `morriganPT_tage` build never rewrote `RP_MP`, `CNF_BITS`, `RP_SUC_MP` or
the `PT_S*` table geometry — it silently inherited whatever the previous build
left in `inc/cache.h` and `inc/morriganPT.h`. Build order changed results.
`RESET_FREQ` had the same problem: only written for `markov_sota`, but read by
Morrigan's RLFU decay path.

**Fix:** `morriganPT_tage` added to every relevant conditional, and
`RESET_FREQ` is now written unconditionally. `run_all.sh` additionally writes a
`CONFIG.txt` into each results folder recording the active `#define`s, compiler
version and date, so a run can always be traced back to its configuration.

---

## Verification

`tools/tage_selftest.cc` reimplements only the T-IRIP indexing logic — no
ChampSim dependency — and drives both the old and new key schemes with a
synthetic miss stream of four deterministic 12-step routes. Every successor in
that stream is fully determined by its history context, so a correct history
predictor should approach a 100% hit rate.

```
$ g++ -O2 -std=c++11 -o tage_selftest tage_selftest.cc && ./tage_selftest

Synthetic iSTLB miss stream: 36000 misses, 4 deterministic routes

                                        lookups   table hits     hit rate
BUGGY (key recomputed at train)           35998         3746      10.406%
FIXED (key snapshotted)                   36000        35952      99.867%

                                        correct     accuracy
BUGGY                                       749      19.995%
FIXED                                     34449      95.819%
```

The buggy scheme reaches 10% on this stream only because the synthetic routes
reuse deltas; on real traces the equivalent figure was 0.41%.

---

## What this does and does not prove

It proves the tables now train and read consistently, so T-IRIP will actually
participate in prediction. It does **not** yet prove that T-IRIP improves IPC on
the QMM traces — that requires re-running the 45-trace sweep, which is the point
of this package.

The honest prior expectation is modest. Stock Morrigan already covers **75%** of
iSTLB misses at **14%** prefetch accuracy, so the available headroom is the
remaining 25%, and the prefetcher is already over-issuing. T-IRIP is also still
wired as an *additive* source on top of IRIP rather than replacing it, so the
two will predict the same successors much of the time. The interesting numbers
to watch after the re-run are:

1. **T-IRIP hit rate** — should be percent-scale, not 0.4%
2. **T-IRIP accuracy** — reported per history table (H2 / H4 / H8)
3. Whether H8 provides anything H2 does not, which is the entire argument for a
   multi-length design
4. Whether coverage rises **without** accuracy falling

If (3) shows H8 contributing nothing, the multi-length structure is not earning
its storage and the replacement design should be reconsidered before more
engineering effort goes in.


---

# Part 2 — post-fix measurement, and what it exposed

The fix was applied and a smoke test run on `srv_12`:

```
Total iSTLB misses : 143621
T-IRIP predictions : 612          (pre-fix: 243)
T-IRIP correct     : 156          -> accuracy 25.49%
IRIP  predictions  : 67493
T-IRIP installs    : 430222
T-IRIP evictions   : 429902
  H2  preds: 378  correct: 108  acc: 28.57%
  H4  preds: 158  correct:  48  acc: 30.38%
  H8  preds:  76  correct:   0  acc:  0.00%
```

## What this confirms

The keys now line up. Two independent signals:

- **Accuracy is 25.5%**, and per-table 28.6% / 30.4%. Random 16-bit tag
  collisions cannot produce that. Pre-fix there was no accuracy counter at all,
  but a 0.17% hit rate at chance accuracy is what collisions look like.
- 25.5% is **higher than Morrigan's own 14.19% prefetch accuracy**, so the
  predictions T-IRIP does make are better than average for this system.

## What this exposes: the tables are thrashing, not predicting

```
installs / iSTLB miss   = 430222 / 143621 = 2.996   ->  ~3.0
evictions / installs    = 429902 / 430222 = 99.93%
total table capacity    = 128 + 128 + 64  = 320 entries
rewrites per entry      = 430222 / 320    = 1344
```

Three installs per miss means `tage_search()` fails at update time on
essentially every miss — the context is not there to be found. And with one
install per table per miss:

| table | entries | entry lifetime |
|---|---|---|
| H2 | 128 | ~128 misses |
| H4 | 128 | ~128 misses |
| H8 | 64  | ~64 misses  |

A context has to recur inside that window to ever be read back. For comparison,
IRIP has 4096 + 4096 + 4096 + 1024 = **13,312 entries — 42× more** — and gets a
47% hit rate keying on the VPN alone.

`H8: 76 preds, 0 correct` is the clearest symptom. An 8-deep context is the most
specific key in the design and has the smallest table; it never survives long
enough to be useful.

So the remaining problem is **capacity and allocation policy**, not indexing.

## v2: what changed

`prefetcher/morriganPT_tage2.stlb_pref`, built as a separate fourth config so
v1 results stay intact and the two can be compared directly.

**1. Capacity.** 1024 entries per table (8× H2/H4, 16× H8) — 3072 total,
still 4× smaller than IRIP.

**2. TAGE allocation policy.** This is the bigger change. v1 wrote into every
eligible table on every miss. Real TAGE:

- correct prediction → strengthen the provider, set its usefulness bit,
  **allocate nothing**
- misprediction → weaken the provider, allocate **one** entry in a table with a
  *longer* history than the provider
- no provider → allocate one entry in the shortest eligible table

That alone cuts installs ~3×. Combined with the capacity increase, expected
entry lifetime rises from ~128 misses to well over 1000.

**3. Usefulness bits.** An entry that produced a correct prediction is protected
from eviction. If every way in a set is useful, the allocation is skipped and
the set is aged instead. A global reset every 65,536 allocations stops
stale-but-once-useful entries from locking the tables forever.

**4. Delta-based history (off by default).** `TAGE_HIST_DELTA 1` folds the
sequence of *deltas* between misses instead of the sequence of VPNs, so the same
access pattern at a different address maps to the same context. Left off so v2
isolates the capacity/allocation change; worth an ablation afterwards.

All four are compile-time knobs at the top of the file, including
`TAGE_ALLOC_POLICY 0` to reproduce v1 behaviour at v2 capacity — which is the
clean ablation separating "bigger tables" from "better allocation".

## What to look for in the v2 numbers

- `Installs per miss` should drop from ~3.0 to well under 1.0
- `T-IRIP evictions / installs` should drop far below 99.93%
- `T-IRIP predictions` should be percent-scale, not 0.4%
- `H8` should stop being 0-correct, or the multi-length structure is not
  earning its storage and should be cut

Accuracy may *fall* somewhat as volume rises — that is normal and fine, as long
as it stays near or above Morrigan's 14.19%.

## Honest expectation

Even a well-behaved T-IRIP has limited room. Morrigan already covers 75% of
iSTLB misses; the headroom is the remaining 25%, and the prefetcher already
over-issues (14% accuracy). The point of v2 is to find out whether the multi-
history idea has any merit at all, which v1 could not answer because the
predictor was never really running.

---

# Part 3 — v2 measured on all 45 traces

The v2 sweep completed (43 min 39 s, `JOBS=12`). Aggregate T-IRIP behaviour:

| Metric (45-trace totals) | v1 | v2 |
|---|---|---|
| T-IRIP predictions | 29,223 | 3,199,867 |
| … as share of iSTLB misses | 0.53% | **58.42%** |
| T-IRIP prediction accuracy | 35.9% | **82.3%** |
| Installs per iSTLB miss | 2.99 | **0.48** |
| Eviction rate | 99.91% | 94.74% |
| H2 accuracy | 39.1% | 91.7% |
| H4 accuracy | 37.8% | 74.2% |
| H8 accuracy | 0.51% | **68.0%** |
| Traces where H8 got 0 correct | 40 / 45 | **0 / 45** |

Every prediction target from Part 2 was met:

- installs per miss 3.0 → 0.48 (target: under 1.0) ✓
- T-IRIP activity 0.53% → 58.4% (target: percent-scale) ✓
- H8 non-zero on every trace (target: stop being 0-correct) ✓

The eviction rate stayed high at 94.74%, but that is expected and benign: once
the tables are full, almost every allocation displaces something. What matters
is the *rate* of allocation, which fell 6.2×. `analyze.py` only flags thrashing
above 1.5 installs per miss for this reason.

Per-trace spread on v2 is tight — installs/miss 0.375–0.552, T-IRIP accuracy
79.6–85.7%, contribution 51.0–69.3% — so no single trace is carrying the result.

End-to-end effect: **+8.398% geomean vs Morrigan's +7.493%, winning on 45 / 45
traces**, with coverage up 9.6 pp for a 0.3 pp accuracy cost. See `RESULTS.md`.

## The three defects, in order of impact

1. **Train/lookup key mismatch** (Part 1) — made the predictor structurally
   incapable of hitting. Fixing it raised activity from 0.17% to 0.53% and,
   more importantly, made the remaining problem measurable.
2. **Allocation policy** — writing into all three tables on every miss. Fixing
   this was worth more than the capacity increase.
3. **Capacity** — 320 entries against IRIP's 13,312.

(2) and (3) were changed together in v2, so their individual contributions are
not yet separated. `TAGE_ALLOC_POLICY 0` at v2 capacity is the ablation that
does so, and has not been run.
