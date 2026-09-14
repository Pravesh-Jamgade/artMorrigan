#!/usr/bin/env bash
# Build every requested detail-mode STLB capacity/PTE-block combination.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
CONFIG_DIR="$ROOT/ChampSim-SC/configs/stlb_detail"

usage() {
    echo "Usage: $0 [all|12kb|24kb|36kb|48kb] [--jobs BENCHMARK.tlist [JOBLIST]]" >&2
}

size=all
if [[ ${1:-} != --* && $# -gt 0 ]]; then size=$1; shift; fi
case "$size" in all|12kb|24kb|36kb|48kb) ;; *) usage; exit 2;; esac
benchmark=""; joblist="$ROOT/stlb_matrix_jobs.list"
if [[ $# -gt 0 ]]; then
    [[ $1 == --jobs && $# -ge 2 && $# -le 3 ]] || { usage; exit 2; }
    benchmark=$2; [[ $# -eq 3 ]] && joblist=$3
    : > "$joblist"
fi

for config in "$CONFIG_DIR"/*.ini; do
    base=$(basename "$config" .ini)
    [[ $size == all || $base == "$size"_* ]] || continue
    "$ROOT/ChampSim-SC/generate_binary.sh" "$config"
    if [[ -n $benchmark ]]; then
        suffix="_detail_${base}"
        binary=$(find "$ROOT/ChampSim-SC/bin" -maxdepth 1 -type f -name "no64nofp${suffix}-*" -print -quit)
        [[ -n $binary ]] || { echo "Could not locate binary for $config" >&2; exit 1; }
        python3 "$ROOT/jobs_gen.py" "$benchmark" "$binary" 50000000 100000000 >> "$joblist"
    fi
done
[[ -z $benchmark ]] || echo "Job list: $joblist"
