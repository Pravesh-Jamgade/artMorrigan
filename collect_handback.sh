#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# collect_handback.sh — bundle a small, shareable digest of a completed sweep.
#
# Usage:  bash collect_handback.sh [label]
#         label defaults to "run"
#
# Produces  handback_<label>.tar.gz  (a few hundred KB) containing:
#   summary.txt        the analyze.py summary table
#   results.csv        per-trace IPC / speedup / coverage / accuracy
#   tirip_stats.txt    the T-IRIP block from every trace, per config
#   config/            the CONFIG.txt provenance files
# ─────────────────────────────────────────────────────────────────────────────
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
LABEL="${1:-run}"
OUT="$HERE/handback_$LABEL"
STATS="$HERE/ChampSim-SC/Statistics"

rm -rf "$OUT"; mkdir -p "$OUT/config"

echo "==> analyze.py"
python3 "$HERE/analyze.py" --stats "$STATS" --out "$OUT" --no-figures \
    2>&1 | tee "$OUT/summary.txt"

echo "==> T-IRIP per-trace blocks"
: > "$OUT/tirip_stats.txt"
for cfg in "$STATS"/*/; do
    name="$(basename "$cfg")"
    case "$name" in *tage*) ;; *) continue ;; esac
    # skip folders with no trace results (stray/empty dirs)
    ls "$cfg"srv_*.txt >/dev/null 2>&1 || continue
    echo "################ CONFIG: $name ################" >> "$OUT/tirip_stats.txt"
    for f in "$cfg"srv_*.txt; do
        [ -f "$f" ] || continue
        echo "=== $(basename "$f" .txt) ===" >> "$OUT/tirip_stats.txt"
        sed -n '/T-IRIP Statistics/,/^=========================/p' "$f" >> "$OUT/tirip_stats.txt"
    done
done

echo "==> provenance"
for f in "$STATS"/*/CONFIG.txt; do
    [ -f "$f" ] || continue
    cp "$f" "$OUT/config/$(basename "$(dirname "$f")").txt"
done

cd "$HERE"
tar czf "handback_$LABEL.tar.gz" "handback_$LABEL"
echo
echo "=========================================================="
echo " Created: $HERE/handback_$LABEL.tar.gz"
ls -lh "$HERE/handback_$LABEL.tar.gz"
echo "=========================================================="
