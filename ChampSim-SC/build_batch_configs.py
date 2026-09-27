#!/usr/bin/env python3
"""
Batch compile ChampSim binaries from a text file listing configuration file paths.

Usage:
    python3 build_batch_configs.py configs_list.txt [options]
    python3 build_batch_configs.py --list configs_list.txt [options]

Features:
- Reads config paths (absolute or relative) line by line from an input file.
- Skips empty lines and comments (lines starting with #).
- Compiles binaries sequentially or in parallel (`-j` / `--jobs`).
- Supports passing additional overrides via `--set` or legacy ChampSim flags.
"""

from __future__ import annotations

import argparse
import sys
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Batch build ChampSim binaries given multiple configuration file paths from an input file.",
        epilog="Example input file (configs_list.txt):\n"
               "  /media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/configs/stlb_study/sparsity_12kb_4pte.ini\n"
               "  configs/stlb_study/sparsity_12kb_1pte.ini\n"
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


def build_single_config(config_path: Path, set_overrides: list[str]) -> tuple[Path, bool, str]:
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
        return config_path, True, res.stdout
    except subprocess.CalledProcessError as e:
        return config_path, False, e.stdout


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

    print(f"=== Starting batch build for {len(config_paths)} configuration(s) (Jobs: {args.jobs}) ===")
    for idx, cfg in enumerate(config_paths, 1):
        print(f"  [{idx}] {cfg}")
    print("=" * 65 + "\n")

    failed: list[Path] = []
    successful: list[Path] = []

    if args.jobs == 1:
        for idx, cfg in enumerate(config_paths, 1):
            print(f"[{idx}/{len(config_paths)}] Building {cfg.name}...")
            cfg_path, success, output = build_single_config(cfg, args.set)
            if success:
                print(f"  --> SUCCESS: {cfg_path.name}")
                successful.append(cfg_path)
            else:
                print(f"  --> FAILED: {cfg_path.name}")
                print(output, file=sys.stderr)
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
                cfg_path, success, output = future.result()
                if success:
                    print(f"[SUCCESS] {cfg_path.name}")
                    successful.append(cfg_path)
                else:
                    print(f"[FAILED] {cfg_path.name}\n{output}", file=sys.stderr)
                    failed.append(cfg_path)

    print("\n" + "=" * 65)
    print(f"Batch build completed: {len(successful)} successful, {len(failed)} failed out of {len(config_paths)} total.")
    if failed:
        print("Failed configurations:")
        for f in failed:
            print(f"  - {f}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
