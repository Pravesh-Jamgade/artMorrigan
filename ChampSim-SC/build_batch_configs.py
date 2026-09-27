#!/usr/bin/env python3
"""
Batch compile ChampSim binaries from a text file listing configuration file paths,
and optionally generate job run commands for each compiled binary using jobs_gen.py.

Usage:
    python3 build_batch_configs.py configs_list.txt [options]
    python3 build_batch_configs.py configs_list.txt --gen-jobs BENCHMARK.tlist [options]

Examples:
    python3 build_batch_configs.py configs_list.txt
    python3 build_batch_configs.py configs_list.txt --gen-jobs benchmarks.tlist --warmup 50000000 --sim 100000000 --jobs-file my_batch_jobs.list
"""

from __future__ import annotations

import argparse
import sys
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = ROOT.parent


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Batch build ChampSim binaries given multiple configuration file paths from an input file, and optionally generate job lists.",
        epilog="Example input file (configs_list.txt):\n"
               "  /media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/configs/stlb_study/sparsity_12kb_4pte.ini\n"
               "  configs/test_12kb_1pte.ini\n"
               "  configs/tage2.ini\n",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "input_file",
        nargs="?",
        type=Path,
        help="Path to text file containing list of config paths (one per line).",
    )
    parser.add_argument(
        "-f", "--file", "--list",
        dest="file_list",
        type=Path,
        help="Path to text file containing list of config paths (alternative syntax).",
    )
    parser.add_argument(
        "-j", "--jobs",
        type=int,
        default=1,
        help="Number of parallel build jobs (default: 1).",
    )
    parser.add_argument(
        "--continue-on-error",
        action="store_true",
        help="Continue building remaining configs if a config fails.",
    )
    parser.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="SECTION.KEY=VALUE",
        help="Override INI values across all configs (passed to configure_binary.py).",
    )

    # Job Generator arguments
    job_group = parser.add_argument_group("Job Generation Options")
    job_group.add_argument(
        "--gen-jobs",
        type=Path,
        metavar="BENCHMARK.tlist",
        help="Generate simulation job commands for built binaries using specified benchmark .tlist file.",
    )
    job_group.add_argument(
        "--jobs-file",
        type=Path,
        metavar="JOBS_FILE",
        help="File to append generated job commands (default: stlb_matrix_jobs.list in project root).",
    )
    job_group.add_argument(
        "--warmup",
        type=int,
        default=50000000,
        help="Warmup instructions for job generator (default: 50000000).",
    )
    job_group.add_argument(
        "--sim",
        type=int,
        default=100000000,
        help="Simulation instructions for job generator (default: 100000000).",
    )
    job_group.add_argument(
        "--trace-dir",
        type=Path,
        help="Directory containing traces (passed to jobs_gen.py).",
    )

    return parser.parse_args()


def load_config_list(list_path: Path) -> list[Path]:
    if not list_path.is_file():
        raise FileNotFoundError(f"Input file not found: {list_path}")

    configs: list[Path] = []
    with open(list_path, "r", encoding="utf-8") as f:
        for line_num, line in enumerate(f, 1):
            raw = line.strip()
            if not raw or raw.startswith("#"):
                continue
            config_path = Path(raw)
            if not config_path.is_absolute():
                config_path = (list_path.parent / config_path).resolve()
            
            if not config_path.is_file():
                print(f"[WARNING] Line {line_num}: Config file does not exist: {raw}", file=sys.stderr)
            configs.append(config_path)

    return configs


def build_single_config(config_path: Path, set_overrides: list[str]) -> tuple[Path, bool, str, list[str]]:
    cmd = [sys.executable, str(ROOT / "configure_binary.py"), str(config_path)]
    for override in set_overrides:
        cmd.extend(["--set", override])

    try:
        res = subprocess.run(
            cmd,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=True,
        )
        return config_path, True, res.stdout, res.stdout.splitlines()
    except subprocess.CalledProcessError as e:
        return config_path, False, e.stdout, []


def extract_binary_from_output(output_lines: list[str], config_path: Path) -> Path | None:
    bin_dir = ROOT / "bin"
    # 1. Search for binary location directly in configure output logs
    for line in reversed(output_lines):
        if "bin/" in line or str(bin_dir) in line:
            parts = line.split()
            for part in parts:
                p = Path(part)
                if p.is_file() and p.parent == bin_dir:
                    return p
    
    # 2. Fallback: match binary by config name stem or newest file in bin/
    cfg_stem = config_path.stem
    matching_bins = list(bin_dir.glob(f"*{cfg_stem}*"))
    if matching_bins:
        return max(matching_bins, key=lambda f: f.stat().st_mtime)
    
    all_bins = [f for f in bin_dir.iterdir() if f.is_file()]
    if all_bins:
        return max(all_bins, key=lambda f: f.stat().st_mtime)
    
    return None


def generate_jobs_for_binary(
    benchmark_file: Path,
    binary_path: Path,
    warmup: int,
    sim: int,
    trace_dir: Path | None,
    jobs_output_file: Path
) -> bool:
    jobs_gen_script = PROJECT_ROOT / "jobs_gen.py"
    if not jobs_gen_script.is_file():
        jobs_gen_script = ROOT / "jobs_gen.py"

    if not jobs_gen_script.is_file():
        print(f"[ERROR] jobs_gen.py not found at {jobs_gen_script}", file=sys.stderr)
        return False

    benchmark_file = benchmark_file.resolve()
    if not benchmark_file.is_file():
        print(f"[ERROR] Benchmark file not found: {benchmark_file}", file=sys.stderr)
        return False

    cmd = [
        sys.executable,
        str(jobs_gen_script),
        str(benchmark_file),
        str(binary_path),
        str(warmup),
        str(sim),
    ]
    if trace_dir:
        cmd.extend(["--trace-dir", str(trace_dir)])

    try:
        res = subprocess.run(
            cmd,
            cwd=PROJECT_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=True,
        )
        with open(jobs_output_file, "a", encoding="utf-8") as jf:
            jf.write(res.stdout)
        return True
    except subprocess.CalledProcessError as e:
        print(f"[ERROR] Job generation failed for {binary_path.name}:\n{e.stderr}", file=sys.stderr)
        return False


def main() -> int:
    args = parse_args()
    input_path = args.file_list or args.input_file

    if not input_path:
        print("[ERROR] Please provide an input file containing config paths.", file=sys.stderr)
        print("Usage: python3 build_batch_configs.py <configs_list.txt>", file=sys.stderr)
        return 1

    try:
        config_paths = load_config_list(input_path)
    except Exception as e:
        print(f"[ERROR] Failed to load config list: {e}", file=sys.stderr)
        return 1

    if not config_paths:
        print(f"[INFO] No valid config paths found in {input_path}.")
        return 0

    jobs_output_path = args.jobs_file or (PROJECT_ROOT / "stlb_matrix_jobs.list")
    if args.gen_jobs:
        # Truncate / initialize jobs file
        jobs_output_path.parent.mkdir(parents=True, exist_ok=True)
        jobs_output_path.write_text("")
        print(f"[INFO] Job generation enabled. Jobs will be saved to: {jobs_output_path}")

    print(f"=== Starting batch build for {len(config_paths)} configuration(s) (Jobs: {args.jobs}) ===")
    for idx, cfg in enumerate(config_paths, 1):
        print(f"  [{idx}] {cfg}")
    print("=" * 65 + "\n")

    failed: list[Path] = []
    successful: list[tuple[Path, Path | None]] = []

    if args.jobs == 1:
        for idx, cfg in enumerate(config_paths, 1):
            print(f"[{idx}/{len(config_paths)}] Building {cfg.name}...")
            cfg_path, success, output_text, output_lines = build_single_config(cfg, args.set)
            if success:
                binary = extract_binary_from_output(output_lines, cfg_path)
                print(f"  --> SUCCESS: {cfg_path.name} (Binary: {binary.name if binary else 'unknown'})")
                successful.append((cfg_path, binary))
                if args.gen_jobs and binary:
                    gen_ok = generate_jobs_for_binary(
                        args.gen_jobs, binary, args.warmup, args.sim, args.trace_dir, jobs_output_path
                    )
                    if gen_ok:
                        print(f"      --> Jobs generated for {binary.name}")
            else:
                print(f"  --> FAILED: {cfg_path.name}")
                print(output_text, file=sys.stderr)
                failed.append(cfg_path)
                if not args.continue_on_error:
                    print("\n[ERROR] Stopping batch build due to failure (use --continue-on-error to proceed).", file=sys.stderr)
                    break
    else:
        with ThreadPoolExecutor(max_workers=args.jobs) as executor:
            future_to_cfg = {
                executor.submit(build_single_config, cfg, args.set): cfg for cfg in config_paths
            }
            for future in as_completed(future_to_cfg):
                cfg_path, success, output_text, output_lines = future.result()
                if success:
                    binary = extract_binary_from_output(output_lines, cfg_path)
                    print(f"[SUCCESS] {cfg_path.name} (Binary: {binary.name if binary else 'unknown'})")
                    successful.append((cfg_path, binary))
                    if args.gen_jobs and binary:
                        generate_jobs_for_binary(
                            args.gen_jobs, binary, args.warmup, args.sim, args.trace_dir, jobs_output_path
                        )
                else:
                    print(f"[FAILED] {cfg_path.name}\n{output_text}", file=sys.stderr)
                    failed.append(cfg_path)

    print("\n" + "=" * 65)
    print(f"Batch build completed: {len(successful)} successful, {len(failed)} failed out of {len(config_paths)} total.")
    if args.gen_jobs and successful:
        print(f"Job list written to: {jobs_output_path.resolve()}")
    if failed:
        print("Failed configurations:")
        for f in failed:
            print(f"  - {f}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
