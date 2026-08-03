#!/usr/bin/env python3
"""
analyze.py — parse ChampSim-SC Statistics/ into a CSV, a summary table and
             the four figures.

Usage:
    python3 analyze.py                          # reads ChampSim-SC/Statistics
    python3 analyze.py --stats DIR              # read a different Statistics dir
    python3 analyze.py --out DIR                # where to write csv + figures
    python3 analyze.py --no-figures             # table only, no matplotlib

Every number printed here is parsed from the raw .txt output files. Nothing is
hardcoded. (The previous version of this project shipped a plot with literal
numbers typed into the source; that is exactly what this script exists to
prevent.)
"""

import argparse
import glob
import math
import os
import re
import sys

SHORT = {
    "nonofp":                   "baseline",
    "morriganPTfp":             "morrigan",
    "morriganPT_tagefp_tage":   "tirip_v1",
    "morriganPT_tage2fp_tage2": "tirip_v2",
}

ALL_CONFIGS = [
    ("nonofp",                   "Baseline (no prefetcher)"),
    ("morriganPTfp",             "Morrigan (IRIP+SDP)"),
    ("morriganPT_tagefp_tage",   "Morrigan + T-IRIP v1"),
    ("morriganPT_tage2fp_tage2", "Morrigan + T-IRIP v2"),
]
# Filled in at runtime with whichever config folders actually exist.
CONFIGS = list(ALL_CONFIGS)


def parse_file(path):
    """Pull every metric we care about out of one ChampSim-SC output file."""
    try:
        txt = open(path, errors="replace").read()
    except FileNotFoundError:
        return None

    d = {}

    # IPC: take the LAST cumulative IPC, which is end-of-ROI (not warmup).
    ipcs = re.findall(r"cumulative IPC:\s*([\d.]+)", txt)
    if not ipcs:
        return None
    d["ipc"] = float(ipcs[-1])

    def grab(key, pattern, cast=float):
        m = re.search(pattern, txt)
        d[key] = cast(m.group(1)) if m else None

    grab("istlb_misses", r"I-STLB MISSES:\s*(\d+)")
    grab("dstlb_misses", r"D-STLB MISSES:\s*(\d+)")
    grab("pq_hits",      r"PQ hits\s*:\s*(\d+)")
    grab("pq_misses",    r"PQ misses\s*:\s*(\d+)")

    m = re.search(r"STLB PREFETCH\s+REQUESTED:\s*(\d+)\s+ISSUED:\s*(\d+)", txt)
    d["pf_requested"] = float(m.group(1)) if m else 0.0
    d["pf_issued"]    = float(m.group(2)) if m else 0.0

    # T-IRIP block (only present in the tage config)
    grab("tage_preds",   r"T-IRIP predictions\s*:\s*(\d+)")
    grab("tage_installs", r"T-IRIP installs\s*:\s*(\d+)")
    grab("tage_total_misses", r"Total iSTLB misses\s*:\s*(\d+)")
    grab("tage_evicts",   r"T-IRIP evictions\s*:\s*(\d+)")
    grab("tage_correct", r"T-IRIP correct\s*:\s*(\d+)")
    grab("irip_preds",   r"IRIP  predictions\s*:\s*(\d+)")

    # Per-table breakdown, if the fixed prefetcher produced it
    per_tab = re.findall(r"H(\d+)\s+preds:\s*(\d+)\s+correct:\s*(\d+)", txt)
    for hlen, preds, corr in per_tab:
        d["h%s_preds" % hlen] = float(preds)
        d["h%s_correct" % hlen] = float(corr)

    return d


def geomean(xs):
    xs = [x for x in xs if x and x > 0]
    if not xs:
        return float("nan")
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--stats", default=os.path.join(here, "ChampSim-SC", "Statistics"))
    ap.add_argument("--out",   default=os.path.join(here, "analysis_out"))
    ap.add_argument("--no-figures", action="store_true")
    args = ap.parse_args()

    if not os.path.isdir(args.stats):
        sys.exit("No such Statistics directory: %s" % args.stats)
    os.makedirs(args.out, exist_ok=True)

    # Only analyse configs that were actually run.
    global CONFIGS
    CONFIGS = [(c, l) for c, l in ALL_CONFIGS
               if os.path.isdir(os.path.join(args.stats, c))]
    if len(CONFIGS) < 2:
        sys.exit("Need at least the baseline and one prefetcher config in %s"
                 % args.stats)
    print("Configs found: %s" % ", ".join(c for c, _ in CONFIGS))

    # Trace list = whatever the baseline folder contains
    base_dir = os.path.join(args.stats, CONFIGS[0][0])
    if not os.path.isdir(base_dir):
        sys.exit("Baseline folder missing: %s" % base_dir)
    traces = sorted(
        os.path.basename(p)[:-4]
        for p in glob.glob(os.path.join(base_dir, "*.txt"))
        if not p.endswith("CONFIG.txt")
    )
    if not traces:
        sys.exit("No result files found in %s" % base_dir)

    data = {}
    for cfg, _ in CONFIGS:
        data[cfg] = {}
        for t in traces:
            r = parse_file(os.path.join(args.stats, cfg, t + ".txt"))
            if r:
                data[cfg][t] = r

    complete = [t for t in traces if all(t in data[c] for c, _ in CONFIGS)]
    if len(complete) != len(traces):
        print("WARNING: %d/%d traces have all three configs; using those."
              % (len(complete), len(traces)))
    traces = complete
    if not traces:
        sys.exit("No trace has results for all three configs.")

    # ── results.csv ─────────────────────────────────────────────────────────
    # One block of columns per config that was actually run, so nothing is
    # silently dropped when a fourth variant is added.
    csv_path = os.path.join(args.out, "results.csv")
    with open(csv_path, "w") as f:
        cols = ["trace"]
        for cfg, _ in CONFIGS:
            n = SHORT.get(cfg, cfg)
            cols += ["%s_ipc" % n, "%s_speedup" % n,
                     "%s_coverage" % n, "%s_accuracy" % n]
            if "tage" in cfg:
                cols += ["%s_tirip_preds" % n, "%s_tirip_correct" % n,
                         "%s_tirip_share" % n, "%s_installs_per_miss" % n]
        f.write(",".join(cols) + "\n")

        for t in traces:
            b = data["nonofp"][t]
            row = [t]
            for cfg, _ in CONFIGS:
                r = data[cfg][t]
                cov = 100.0 * r["pq_hits"] / r["istlb_misses"] if r["istlb_misses"] else 0.0
                acc = 100.0 * r["pq_hits"] / r["pf_issued"] if r["pf_issued"] else 0.0
                row += ["%.6f" % r["ipc"],
                        "%.4f" % (100 * (r["ipc"] / b["ipc"] - 1)),
                        "%.2f" % cov, "%.2f" % acc]
                if "tage" in cfg:
                    tp = r.get("tage_preds") or 0
                    tc = r.get("tage_correct") or 0
                    ti = r.get("tage_installs") or 0
                    tm = r.get("tage_total_misses") or r["istlb_misses"] or 1
                    row += ["%d" % tp, "%d" % tc,
                            "%.4f" % (100.0 * tp / tm),
                            "%.4f" % (ti / tm)]
            f.write(",".join(row) + "\n")

    # ── summary ──────────────────────────────────────────────────────────────
    print()
    print("=" * 78)
    print(" SUMMARY  (%d traces)" % len(traces))
    print("=" * 78)
    print("%-26s %12s %11s %11s" % ("config", "geo speedup", "coverage", "accuracy"))
    print("-" * 78)
    for cfg, label in CONFIGS:
        ratios, covs, accs = [], [], []
        for t in traces:
            r, b = data[cfg][t], data["nonofp"][t]
            ratios.append(r["ipc"] / b["ipc"])
            if r["istlb_misses"]:
                covs.append(100.0 * r["pq_hits"] / r["istlb_misses"])
            accs.append(100.0 * r["pq_hits"] / r["pf_issued"] if r["pf_issued"] else 0.0)
        print("%-26s %+11.3f%% %10.2f%% %10.2f%%" % (
            label, 100 * (geomean(ratios) - 1),
            sum(covs) / len(covs) if covs else 0.0,
            sum(accs) / len(accs) if accs else 0.0))
    print("-" * 78)

    # ── per T-IRIP variant: gain over Morrigan, activity, table breakdown ────
    for cfg, label in CONFIGS:
        if "tage" not in cfg:
            continue
        print("-" * 78)
        deltas = [100 * (data[cfg][t]["ipc"] / data["morriganPTfp"][t]["ipc"] - 1)
                  for t in traces]
        print("%s vs Morrigan : geomean %+0.4f pp, wins %d / %d traces" % (
            label, 100 * (geomean([1 + d / 100 for d in deltas]) - 1),
            sum(1 for d in deltas if d > 0), len(deltas)))

        tp = sum(data[cfg][t].get("tage_preds") or 0 for t in traces)
        tc = sum(data[cfg][t].get("tage_correct") or 0 for t in traces)
        tm = sum((data[cfg][t].get("tage_total_misses")
                  or data[cfg][t]["istlb_misses"]) for t in traces)
        ti = sum(data[cfg][t].get("tage_installs") or 0 for t in traces)
        te = sum(data[cfg][t].get("tage_evicts") or 0 for t in traces)
        mm = tm
        print("  activity   : %d predictions on %d iSTLB misses = %.3f%% of misses"
              % (tp, tm, 100.0 * tp / tm if tm else 0))
        if tp:
            print("  accuracy   : %d correct = %.2f%%" % (tc, 100.0 * tc / tp))
        if ti:
            ipm = ti / mm if mm else 0
            print("  installs   : %d  (%.2f per iSTLB miss)  evict rate %.2f%%"
                  % (ti, ipm, 100.0 * te / ti))
            if ipm > 1.5:
                print("               >1.5 installs per miss means the tables are")
                print("               thrashing: contexts are evicted before they")
                print("               can be read back")
            else:
                print("               <1.5 installs per miss: allocation is under")
                print("               control. A high evict rate is normal once the")
                print("               tables are full and is not itself a problem.")

        tabs = [2, 4, 8]
        if any("h%d_preds" % h in data[cfg][traces[0]] for h in tabs):
            print("  per-history-table:")
            for h in tabs:
                pp = sum(data[cfg][t].get("h%d_preds" % h, 0) for t in traces)
                cc = sum(data[cfg][t].get("h%d_correct" % h, 0) for t in traces)
                print("    H%-2d  preds %9d   correct %9d   acc %6.2f%%" % (
                    h, pp, cc, 100.0 * cc / pp if pp else 0.0))
    print("=" * 78)
    print("Wrote %s" % csv_path)

    if args.no_figures:
        return

    # ── figures ──────────────────────────────────────────────────────────────
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("matplotlib/numpy not installed; skipping figures "
              "(pip install matplotlib numpy)")
        return

    x = np.arange(len(traces))
    ipc = {cfg: np.array([data[cfg][t]["ipc"] for t in traces]) for cfg, _ in CONFIGS}
    base = ipc["nonofp"]
    sp   = {cfg: 100 * (ipc[cfg] / base - 1) for cfg, _ in CONFIGS}
    geo  = {cfg: 100 * (geomean(ipc[cfg] / base) - 1) for cfg, _ in CONFIGS}

    PALETTE = {"nonofp": "#9ca3af", "morriganPTfp": "#3b82f6",
               "morriganPT_tagefp_tage": "#10b981",
               "morriganPT_tage2fp_tage2": "#ef4444"}
    pref = [(c, l) for c, l in CONFIGS if c != "nonofp"]
    W = max(12, len(traces) * 0.36)

    # fig1: speedup, every prefetcher config
    fig, ax = plt.subplots(figsize=(W, 5))
    w = 0.8 / len(pref)
    for i, (cfg, label) in enumerate(pref):
        off = (i - (len(pref) - 1) / 2) * w
        ax.bar(x + off, sp[cfg], w, label=label, color=PALETTE.get(cfg))
        ax.axhline(geo[cfg], ls="--", lw=1, color=PALETTE.get(cfg), alpha=.7)
    ax.set_ylabel("Speedup over baseline (%)")
    ax.set_xlabel("QMM Server Workload")
    ax.set_title("Figure 1: IPC speedup over baseline (%d workloads)  |  geomeans: %s"
                 % (len(traces), ", ".join("%s %.2f%%" % (SHORT.get(c, c), geo[c])
                                           for c, _ in pref)))
    ax.set_xticks(x); ax.set_xticklabels(traces, rotation=90, fontsize=6)
    ax.legend(fontsize=8); ax.grid(axis="y", ls=":", alpha=.4)
    fig.tight_layout(); fig.savefig(os.path.join(args.out, "fig1_speedup_comparison.png"), dpi=150)
    plt.close(fig)

    # fig2: gain over Morrigan, one panel per T-IRIP variant
    tage = [(c, l) for c, l in CONFIGS if "tage" in c]
    if tage:
        fig, axes = plt.subplots(len(tage), 1, figsize=(W, 3.6 * len(tage)), squeeze=False)
        for ax, (cfg, label) in zip(axes[:, 0], tage):
            d = sp[cfg] - sp["morriganPTfp"]
            ax.bar(x, d, color=["#10b981" if v > 0 else "#d97706" for v in d])
            ax.axhline(float(np.mean(d)), color="purple", ls="--", lw=1,
                       label="mean %+.3f pp" % np.mean(d))
            ax.axhline(0, color="k", lw=.8)
            ax.set_ylabel("delta vs Morrigan (pp)")
            ax.set_title("%s — wins %d/%d traces" % (label, int((d > 0).sum()), len(d)),
                         fontsize=10)
            ax.set_xticks(x); ax.set_xticklabels(traces, rotation=90, fontsize=6)
            ax.legend(fontsize=8); ax.grid(axis="y", ls=":", alpha=.4)
        fig.suptitle("Figure 2: per-workload gain over stock Morrigan", y=1.0)
        fig.tight_layout(); fig.savefig(os.path.join(args.out, "fig2_tage_gain.png"), dpi=150)
        plt.close(fig)

    # fig3: absolute IPC
    fig, ax = plt.subplots(figsize=(W, 5))
    w = 0.8 / len(CONFIGS)
    for i, (cfg, label) in enumerate(CONFIGS):
        off = (i - (len(CONFIGS) - 1) / 2) * w
        ax.bar(x + off, ipc[cfg], w, label=label, color=PALETTE.get(cfg))
    lo = min(base) * 0.9
    hi = max(max(v) for v in ipc.values()) * 1.05
    ax.set_ylim(lo, hi)
    ax.set_ylabel("Instructions Per Cycle (IPC)")
    ax.set_xlabel("QMM Server Workload")
    ax.set_title("Figure 3: Absolute IPC")
    ax.set_xticks(x); ax.set_xticklabels(traces, rotation=90, fontsize=6)
    ax.legend(fontsize=8); ax.grid(axis="y", ls=":", alpha=.4)
    fig.tight_layout(); fig.savefig(os.path.join(args.out, "fig3_absolute_ipc.png"), dpi=150)
    plt.close(fig)

    # fig4: geomean summary + coverage
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 5))
    labels = [l.replace(" (", "\n(").replace(" + ", "\n+ ") for _, l in CONFIGS]
    vals = [geo[c] for c, _ in CONFIGS]
    bars = a1.bar(labels, vals, color=[PALETTE.get(c) for c, _ in CONFIGS], width=.6)
    for bar, v in zip(bars, vals):
        a1.text(bar.get_x() + bar.get_width() / 2, v + .1, "%.2f%%" % v,
                ha="center", fontweight="bold", fontsize=9)
    a1.axhline(7.6, color="red", ls=":", lw=1.2, label="Morrigan paper: 7.6%")
    a1.set_ylabel("Geomean speedup over baseline (%)")
    a1.set_title("Geomean IPC speedup"); a1.legend(fontsize=8)
    a1.grid(axis="y", ls=":", alpha=.4)
    a1.tick_params(axis="x", labelsize=7)

    covs = []
    for cfg, _ in CONFIGS:
        c = [100.0 * data[cfg][t]["pq_hits"] / data[cfg][t]["istlb_misses"]
             for t in traces if data[cfg][t]["istlb_misses"]]
        covs.append(sum(c) / len(c) if c else 0.0)
    bars = a2.bar(labels, covs, color=[PALETTE.get(c) for c, _ in CONFIGS], width=.6)
    for bar, v in zip(bars, covs):
        a2.text(bar.get_x() + bar.get_width() / 2, v + .8, "%.1f%%" % v,
                ha="center", fontweight="bold", fontsize=9)
    a2.set_ylabel("iSTLB miss coverage (%)")
    a2.set_title("Mean iSTLB miss coverage")
    a2.grid(axis="y", ls=":", alpha=.4)
    a2.tick_params(axis="x", labelsize=7)
    fig.suptitle("Figure 4: Summary across %d QMM server workloads" % len(traces))
    fig.tight_layout(); fig.savefig(os.path.join(args.out, "fig4_geomean_summary.png"), dpi=150)
    plt.close(fig)

    print("Wrote figures to %s" % args.out)


if __name__ == "__main__":
    main()
