#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# run_all.sh — sweep all 45 QMM server traces across the three configurations.
#
# Usage:
#   bash run_all.sh                    # all three configs, 8 parallel jobs
#   bash run_all.sh tage               # just the T-IRIP config
#   JOBS=12 bash run_all.sh            # override parallelism
#   TRACE_DIR=/path/to/traces bash run_all.sh
#
# Output lands in ChampSim-SC/Statistics/<config>/<trace>.txt , which is the
# same layout as results_previous/Statistics so the two can be compared.
# ─────────────────────────────────────────────────────────────────────────────
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
CS="$HERE/ChampSim-SC"

export TRACE_DIR="${TRACE_DIR:-$HOME/mtp/micro-arch/old-work/traces}"
JOBS="${JOBS:-8}"
WARMUP=50        # in millions
SIM=100          # in millions
WHAT="${1:-all}"

BIN_BASE="hashed_perceptron-next_line-next_line-spp_dev-lru-1core"

# config-name : binary-prefix
declare -A BINMAP=(
  [nonofp]="no64nofp-$BIN_BASE"
  [morriganPTfp]="morriganPT64fp-$BIN_BASE"
  [morriganPT_tagefp_tage]="morriganPT_tage64fp_tage-$BIN_BASE"
  [morriganPT_tage2fp_tage2]="morriganPT_tage264fp_tage2-$BIN_BASE"
)

case "$WHAT" in
  all)      CONFIGS=(nonofp morriganPTfp morriganPT_tagefp_tage) ;;
  all2)     CONFIGS=(nonofp morriganPTfp morriganPT_tagefp_tage morriganPT_tage2fp_tage2) ;;
  tage2)    CONFIGS=(morriganPT_tage2fp_tage2) ;;
  baseline) CONFIGS=(nonofp) ;;
  morrigan) CONFIGS=(morriganPTfp) ;;
  tage)     CONFIGS=(morriganPT_tagefp_tage) ;;
  *) echo "Unknown config '$WHAT' (use: all|all2|baseline|morrigan|tage|tage2)"; exit 1 ;;
esac

TRACES=(
  srv_12  srv_128 srv_194 srv_207 srv_21  srv_222 srv_225 srv_255 srv_259
  srv_276 srv_287 srv_32  srv_364 srv_408 srv_41  srv_426 srv_442 srv_48
  srv_495 srv_504 srv_526 srv_537 srv_540 srv_551 srv_575 srv_582 srv_61
  srv_617 srv_641 srv_669 srv_702 srv_706 srv_715 srv_727 srv_73  srv_743
  srv_764 srv_771 srv_85  srv_s0  srv_s10 srv_s60 srv_s61 srv_s69 srv_s7
)

cd "$CS"

echo "=========================================================="
echo " Trace dir : $TRACE_DIR"
echo " Configs   : ${CONFIGS[*]}"
echo " Traces    : ${#TRACES[@]}"
echo " Parallel  : $JOBS jobs"
echo " Warmup/Sim: ${WARMUP}M / ${SIM}M instructions"
echo "=========================================================="

# ── sanity checks ────────────────────────────────────────────────────────────
missing=0
for t in "${TRACES[@]}"; do
    [ -f "$TRACE_DIR/${t}.champsimtrace.xz" ] || { echo "MISSING TRACE: $t"; missing=1; }
done
[ "$missing" -eq 1 ] && { echo "Fix TRACE_DIR and retry."; exit 1; }

for c in "${CONFIGS[@]}"; do
    [ -x "bin/${BINMAP[$c]}" ] || { echo "MISSING BINARY for $c: bin/${BINMAP[$c]}"; echo "Run: bash build_all.sh"; exit 1; }
done
echo "All traces and binaries present."
echo

# ── snapshot the active build configuration for provenance ───────────────────
snapshot_config() {
    local cfg="$1"
    mkdir -p "Statistics/$cfg"
    {
        echo "config      : $cfg"
        echo "binary      : ${BINMAP[$cfg]}"
        echo "date        : $(date -Is)"
        echo "host        : $(hostname)"
        echo "g++         : $(g++ --version 2>/dev/null | head -1)"
        echo "warmup/sim  : ${WARMUP}M / ${SIM}M"
        echo "--- active #defines in inc/cache.h ---"
        grep -E '#define (ENABLE_FP|ENABLE_PREF_FP|RP_MP|RP_SUC_MP|CNF_BITS|RESET_FREQ|LLIMIT|STLB_SET|STLB_WAY|STLB_LATENCY|STLB_PQ_SIZE|P2TLB)' inc/cache.h
        echo "--- inc/morriganPT.h ---"
        grep -E '#define PT_' inc/morriganPT.h
    } > "Statistics/$cfg/CONFIG.txt"
}

START=$(date +%s)
for cfg in "${CONFIGS[@]}"; do
    echo "---------- $cfg ----------"
    snapshot_config "$cfg"
    n=0
    for t in "${TRACES[@]}"; do
        if [ -s "Statistics/$cfg/${t}.txt" ]; then
            echo "  [skip] $t"
            continue
        fi
        ./run_champsim.sh "${BINMAP[$cfg]}" "$WARMUP" "$SIM" "$t" "$cfg" &
        n=$((n+1))
        if [ "$((n % JOBS))" -eq 0 ]; then wait; echo "  ... $n/${#TRACES[@]} done"; fi
    done
    wait
    echo "  $cfg complete."
done
END=$(date +%s)

echo
echo "=========================================================="
echo " Sweep finished in $(( (END-START)/60 )) min $(( (END-START)%60 )) s"
echo " Results: $CS/Statistics/"
echo " Next:    python3 analyze.py"
echo "=========================================================="
