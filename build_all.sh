#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# build_all.sh — build the three ChampSim-SC binaries used in this study.
#
#   nonofp                  baseline, no STLB prefetcher
#   morriganPTfp            stock Morrigan (MICRO'21)
#   morriganPT_tagefp_tage  Morrigan + T-IRIP v1
#   morriganPT_tage2fp_tage2  Morrigan + T-IRIP v2 (bigger tables, TAGE alloc)
#
# Usage:  bash build_all.sh [config]
#         config = all (default) | baseline | morrigan | tage | tage2
# ─────────────────────────────────────────────────────────────────────────────
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
CS="$HERE/ChampSim-SC"
WHAT="${1:-all}"

cd "$CS"

build() {
    local name="$1"; shift
    echo
    echo "=========================================================="
    echo " Building: $name"
    echo "=========================================================="
    ./generate_binary.sh "$@" --end
}

# ── Baseline: no prefetcher, no free prefetching ─────────────────────────────
if [ "$WHAT" = "all" ] || [ "$WHAT" = "baseline" ]; then
    build "nonofp (baseline)" \
        --stlb_pref no \
        --pq_size 64 \
        --free_prefetching 0 \
        --free_prefetching_prefetch 0
fi

# ── Stock Morrigan ───────────────────────────────────────────────────────────
if [ "$WHAT" = "all" ] || [ "$WHAT" = "morrigan" ]; then
    build "morriganPTfp (stock Morrigan)" \
        --stlb_pref morriganPT \
        --pq_size 64 \
        --free_prefetching 0 \
        --free_prefetching_prefetch 1 \
        --replacement_policy 1 \
        --conf_bits 3
fi

# ── Morrigan + T-IRIP ────────────────────────────────────────────────────────
if [ "$WHAT" = "all" ] || [ "$WHAT" = "tage" ]; then
    build "morriganPT_tagefp_tage (Morrigan + T-IRIP)" \
        --stlb_pref morriganPT_tage \
        --pq_size 64 \
        --free_prefetching 0 \
        --free_prefetching_prefetch 1 \
        --replacement_policy 1 \
        --conf_bits 3 \
        --optional _tage
fi

# ── Morrigan + T-IRIP v2 (bigger tables + TAGE allocation policy) ────────────
if [ "$WHAT" = "all" ] || [ "$WHAT" = "tage2" ]; then
    build "morriganPT_tage2fp_tage2 (Morrigan + T-IRIP v2)" \
        --stlb_pref morriganPT_tage2 \
        --pq_size 64 \
        --free_prefetching 0 \
        --free_prefetching_prefetch 1 \
        --replacement_policy 1 \
        --conf_bits 3 \
        --optional _tage2
fi

echo
echo "=========================================================="
echo " Binaries in $CS/bin :"
ls -1 "$CS/bin" 2>/dev/null || echo "  (none - build failed)"
echo "=========================================================="
