#!/usr/bin/env python3
"""Generate shell job commands for a ChampSim binary from a .tlist file."""

import argparse
import os
import re
import shlex
from pathlib import Path


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("benchmark_file", type=Path)
    parser.add_argument("binary", type=Path, help="generated ChampSim binary")
    parser.add_argument("warmup_instructions", type=int)
    parser.add_argument("simulation_instructions", type=int)
    parser.add_argument("--trace-dir", type=Path,
                        help="replace any $(..._TRACE) prefix (default: TRACE_DIR or HERMES_TRACE)")
    parser.add_argument("--output-dir", type=Path,
                        help="log directory (default: binary filename)")
    return parser.parse_args()


def read_traces(path):
    if not path.is_file():
        raise SystemExit(f"benchmark file not found: {path}")
    traces = []
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if line.startswith("TRACE="):
            trace = line.split("=", 1)[1].strip().strip('"\'')
            if not trace:
                raise SystemExit(f"empty TRACE at {path}:{number}")
            traces.append(trace)
    if not traces:
        raise SystemExit(f"no TRACE= entries found in {path}")
    return traces


def main():
    args = arguments()
    binary = args.binary.resolve()
    if not binary.is_file():
        raise SystemExit(f"binary not found: {binary}")
    trace_dir = args.trace_dir or os.environ.get("TRACE_DIR") or os.environ.get("HERMES_TRACE")
    output = args.output_dir or Path(binary.name)
    print(f"mkdir -p {shlex.quote(str(output))}")
    for trace in read_traces(args.benchmark_file):
        if trace_dir:
            trace = re.sub(r"^\$\([A-Za-z0-9_]*TRACE\)", str(trace_dir), trace)
        elif re.match(r"^\$\(", trace):
            raise SystemExit("trace placeholder needs --trace-dir, TRACE_DIR, or HERMES_TRACE")
        filename = Path(trace).name
        workload = filename.split(".champsim", 1)[0]
        command = [str(binary), "-warmup_instructions", str(args.warmup_instructions),
                   "-simulation_instructions", str(args.simulation_instructions),
                   "-traces", trace]
        print(" ".join(map(shlex.quote, command)) + " > " +
              shlex.quote(str(output / (workload + ".log"))) + " 2>&1")


if __name__ == "__main__":
    main()
