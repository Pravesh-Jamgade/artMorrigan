# TAGE-inspired iSTLB Prefetcher (T-IRIP) on top of Morrigan

M.Tech project. Extends the Morrigan composite instruction-TLB prefetcher
(Vavouliotis et al., MICRO 2021) with a multi-history, TAGE-inspired predictor
called **T-IRIP**, evaluated on 45 QMM server traces in **ChampSim-SC**.


---

## Project history

- **v1** — T-IRIP added alongside IRIP. Initial 45-trace sweep showed +0.01 pp,
  i.e. nothing. Root cause found: the history tables were trained with a
  different `(index, tag)` key than they were read with, so they fired on 0.4%
  of misses. Diagnosis and fix in `BUGFIX.md` Part 1.
- **v1 fixed** — keys corrected. Predictor now demonstrably works
  (`tools/tage_selftest.cc` proves it), but still only 0.53% activity: the
  tables were far too small and were being written into three times per miss.
- **v2** — capacity 320 → 3,072 entries, plus proper TAGE allocation (one entry
  on misprediction rather than three every miss). Activity 0.53% → 58.4%,
  T-IRIP prediction accuracy 35.9% → 82.3%. This is the configuration that
  produced the result above.

All three prefetcher variants are kept in the tree so the progression can be
re-run and checked.

---

## Contents

```
tage-istlb/
├── README.md
├── BUGFIX.md                  bug analysis, fixes, verification
├── setup.sh                   installs prerequisites (Ubuntu)
├── build_all.sh               builds the 4 binaries
├── run_all.sh                 sweeps 45 traces × N configs, in parallel
├── analyze.py                 Statistics/ → CSV + summary + 4 figures
├── collect_handback.sh        bundles a small shareable digest of a sweep
├── tools/
│   └── tage_selftest.cc       standalone proof of the v1 bug and its fix
├── ChampSim-SC/               the simulator
│   ├── prefetcher/
│   │   ├── morriganPT.stlb_pref                stock Morrigan (untouched)
│   │   ├── morriganPT_tage.stlb_pref           T-IRIP v1 (key fix)
│   │   ├── morriganPT_tage2.stlb_pref          T-IRIP v2  <- the contribution
│   │   └── morriganPT_tage_BUGGY_ORIGINAL.stlb_pref.txt   pre-fix, for diffing
│   ├── inc/morriganPT.h       IRIP table geometry
│   ├── configs/               editable INI build configurations
│   ├── configure_binary.py    validates INI knobs and configures the simulator
│   ├── generate_binary.sh     user-friendly INI build entry point
│   └── run_champsim.sh        patched: TRACE_DIR from environment
├── results_latest/            data of the run that produced the table above
└── results_previous/          the original pre-fix run, kept for comparison
```

**Traces are not included** (~3 GB) — see step 2.

## Configurations

| Statistics folder | Binary prefix | What it is |
|---|---|---|
| `nonofp` | `no64nofp-…` | Baseline, no STLB prefetcher |
| `morriganPTfp` | `morriganPT64fp-…` | Stock Morrigan |
| `morriganPT_tagefp_tage` | `morriganPT_tage64fp_tage-…` | T-IRIP v1 |
| `morriganPT_tage2fp_tage2` | `morriganPT_tage264fp_tage2-…` | T-IRIP v2 |

---

# Running it

Ubuntu. Commands assume the project sits at
`~/mtp/micro-arch/old-work/tage-istlb`;

## 1. Prerequisites

```bash
sudo apt-get update
sudo apt-get install -y build-essential g++ make xz-utils python3 python3-pip python3-venv unzip bc

cd ~/mtp/micro-arch/old-work
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install pandas matplotlib numpy
```

`bash setup.sh` does all of the above in one command.

## 2. Traces

45 QMM server traces named `srv_<id>.champsimtrace.xz` (`srv_12`, `srv_128`, …,
`srv_s7`). From the Morrigan MICRO'21 artifact, Zenodo DOI
`10.5281/zenodo.5496052`.

```bash
mkdir -p ~/mtp/micro-arch/old-work/traces
# copy the 45 .champsimtrace.xz files there
export TRACE_DIR=~/mtp/micro-arch/old-work/traces
ls $TRACE_DIR/*.champsimtrace.xz | wc -l     # must print 45
```


## 3. Verify the bug fix no traces needed

Self-contained; does not touch ChampSim.

```bash
cd tools
g++ -O2 -std=c++11 -o tage_selftest tage_selftest.cc
./tage_selftest
cd ..
```


## 4. Build

```bash
bash build_all.sh all
ls -1 ChampSim-SC/bin/          # expect 4 binaries
```

Individually: `bash build_all.sh baseline | morrigan | tage | tage2`

### Build from an editable configuration

Simulator and binary-generation knobs are no longer buried in a shell script.
Copy the documented defaults, change only what you need, and build it:

```bash
cp ChampSim-SC/configs/default.ini my-experiment.ini
# Edit my-experiment.ini, including [build], [simulator], [morrigan], or [tage].
ChampSim-SC/generate_binary.sh my-experiment.ini
```

For a plain baseline **without Morrigan or TAGE**, run the generator with no
arguments (the default is the baseline), or use the explicit baseline preset:

```bash
ChampSim-SC/generate_binary.sh
# Equivalent, and useful when scripting experiments:
ChampSim-SC/generate_binary.sh ChampSim-SC/configs/baseline.ini
```

This selects `stlb_prefetcher = no`, disables free prefetching, and produces a
binary whose name starts with `no64nofp-`. The `[morrigan]` and `[tage]` values
in `default.ini` are only available defaults for those optional prefetchers;
they are not enabled or applied by a baseline build.
After compilation, the configurator restores the source files it temporarily
changed, so a build does not leak its settings into the next build or dirty the
working tree.

`configs/default.ini` is always loaded first, so a smaller INI may contain only
overrides; `configs/tage2.ini` is an example. A one-off override can be supplied
without editing the file:

```bash
ChampSim-SC/generate_binary.sh ChampSim-SC/configs/tage2.ini \
  --set tage.h2_sets=2048 --set build.name_suffix=_h2_2048
```

Run `ChampSim-SC/generate_binary.sh --help` for the complete command syntax.
The old command-line flags remain accepted for compatibility with existing
automation, including `build_all.sh` and `micro_ae.sh`.

The shadow four-PTE STLB behavior is selected at build time with
`simulator.stlb_mode`: `analysis` preserves the regular STLB behavior while
collecting shadow-structure statistics, and `detail` makes accesses use the
four-PTE structure. The generated binary still accepts `--stlb_mode` to
override its configured default for an individual simulation.

### Page-walk cache level and cache-block footprint

`simulator.ptw_start_level` selects `l1` (the default) or `l2`. An L2-starting
page-table walker bypasses L1D lookup and allocation, and its reported walk
stall omits `L1D_LATENCY`; lower-level hit and miss costs are unchanged. The
same setting applies to demand and prefetch page walks. It can also be set for
a one-off build with `--set simulator.ptw_start_level=l2` or the compatibility
option `--ptw_start_level l2`.

Every 64-byte L1D, L2C, and LLC block is divided into eight 8-byte entries.
During the block's lifetime the simulator maintains independent masks for
load, RFO, prefetch, and page-table-translation accesses. When the block is
evicted or invalidated, each non-empty mask increments the histogram bin for
its number of distinct entries. Translation accesses are explicitly marked by
the page-table walker rather than inferred from the ordinary request type.
The masks and histograms are reset at the beginning of the ROI.

The block STLB similarly tracks four 8-byte PTE positions. Its four-bin
histogram is updated when a block is replaced or fully invalidated in either
`analysis` or `detail` mode.

## 5. test one trace 

```bash
cd ChampSim-SC
./run_champsim.sh morriganPT_tage264fp_tage2-hashed_perceptron-next_line-next_line-spp_dev-lru-1core \
                  50 100 srv_12 smoketest
sed -n '/T-IRIP Statistics/,/=====/p' Statistics/smoketest/srv_12.txt
cd ..
```


## 6. Full sweep — with `JOBS=12`

```bash
JOBS=12 bash run_all.sh all2
```

`JOBS` is the number of ChampSim processes run concurrently, one trace each.

Targets: `all` (3 configs) · `all2` (4) · `baseline` · `morrigan` · `tage` · `tage2`

Resumable — Under SSH:

```bash
tmux new -s sweep      # Ctrl-B then D to detach; tmux attach -t sweep to return
```

## 7. Analyse

```bash
source ~/mtp/micro-arch/old-work/.venv/bin/activate
python3 analyze.py
```

Writes `analysis_out/results.csv` and `fig1`–`fig4`, and prints the summary
table. Every value is parsed from raw output files; nothing is hardcoded.

---

## Reading the raw output

Per-trace files are `ChampSim-SC/Statistics/<config>/<trace>.txt`. CSV-formatted
statistics are printed directly to standard output between `CSV_STATS_BEGIN`
and `CSV_STATS_END`; no separate CSV file or command-line option is needed.
Scalar values use `key,value` rows. Histograms use two rows with the same key:
a bin-label row followed by its values.

For example:

```csv
CSV_STATS_BEGIN
Core_0_branch_pred_accuracy,92.89
Core_0_L1D_load_footprint,1,2,3,4,5,6,7,8
Core_0_L1D_load_footprint,120,80,30,15,8,4,2,1
Core_0_STLB_block_footprint,1,2,3,4
Core_0_STLB_block_footprint,40,150,300,500
LLC_translation_footprint,1,2,3,4,5,6,7,8
LLC_translation_footprint,10,20,30,40,50,60,70,80
CSV_STATS_END
```

The human-readable file includes:

```
I-STLB MISSES: 96064          instruction STLB misses
PQ hits  : 67501              prefetches later demanded  -> coverage
STLB PREFETCH REQUESTED / ISSUED   -> accuracy = PQ hits / ISSUED
CPU 0 cumulative IPC: ...     take the LAST one (end of ROI, not warmup)
=== T-IRIP Statistics ===     activity, accuracy, installs, per-table breakdown
```

- **coverage** = PQ hits ÷ I-STLB misses
- **accuracy** = PQ hits ÷ prefetches issued
- **speedup** = IPC(config) ÷ IPC(baseline)


---

## Tuning

Compile-time, in the `[tage]` section of `configs/default.ini`:

| Knob | Default | Effect |
|---|---|---|
| `THT_H2/H4/H8_SETS` | 1024 each | Total entries per history table |
| `TAGE_ALLOC_POLICY` | 1 | 1 = TAGE-style; 0 = v1 behaviour (allocate everywhere) |
| `TAGE_USE_U_BIT` | 1 | Usefulness bits protect proven entries |
| `TAGE_U_RESET` | 65536 | Allocations between global usefulness resets |
| `TAGE_HIST_DELTA` | 0 | 1 = fold delta sequence instead of VPN sequence |
| `TAGE_CONF_THRESHOLD` | 1 | Minimum confidence to issue a prefetch |

`TAGE_ALLOC_POLICY 0` at v2 capacity is the clean removal separating "bigger tables" from "better allocation".

---
