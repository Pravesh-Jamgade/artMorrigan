#!/usr/bin/env python3
"""Configure ChampSim source knobs from an INI file and build a binary."""

from __future__ import annotations

import argparse
import configparser
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DEFAULT_CONFIG = ROOT / "configs" / "default.ini"
ORIGINAL_SOURCES: dict[Path, str] = {}

# CLI compatibility keeps existing automation working while INI files become
# the documented interface. Values here are config keys, not defaults.
LEGACY_OPTIONS = {
    "--stlb_pref": "build.stlb_prefetcher", "-pref": "build.stlb_prefetcher",
    "--pq_size": "simulator.pq_size", "-pq": "simulator.pq_size",
    "--free_prefetching": "simulator.free_prefetching", "-fp": "simulator.free_prefetching",
    "--free_prefetching_prefetch": "simulator.free_prefetching_prefetch", "-fpp": "simulator.free_prefetching_prefetch",
    "--pc_table": "simulator.pc_table", "-pct": "simulator.pc_table",
    "--pc_table_assoc": "simulator.pc_table_assoc", "-pca": "simulator.pc_table_assoc",
    "--stlb_size": "simulator.stlb_sets", "-stlbs": "simulator.stlb_sets",
    "--stlb_assoc": "simulator.stlb_assoc", "-stlba": "simulator.stlb_assoc",
    "--stlb_lat": "simulator.stlb_latency", "-stlblat": "simulator.stlb_latency",
    "--stlb_mode": "simulator.stlb_mode",
    "--asap": "simulator.asap", "-asap": "simulator.asap",
    "--ideal": "simulator.ideal", "-ideal": "simulator.ideal",
    "--p2tlb": "simulator.prefetch_to_tlb", "-p2tlb": "simulator.prefetch_to_tlb",
    "--page_size": "simulator.page_size", "-pgs": "simulator.page_size",
    "--markov_sets": "simulator.markov_sets", "-mrs": "simulator.markov_sets",
    "--markov_assoc": "simulator.markov_assoc", "-mra": "simulator.markov_assoc",
    "--lookahead_depth": "simulator.lookahead_depth", "-lad": "simulator.lookahead_depth",
    "--successors": "simulator.successors", "-sucs": "simulator.successors",
    "--reset_freq": "simulator.reset_frequency", "-resf": "simulator.reset_frequency",
    "--replacement_policy": "simulator.replacement_policy", "-rp": "simulator.replacement_policy",
    "--successor_rp": "simulator.successor_replacement_policy", "-rps": "simulator.successor_replacement_policy",
    "--llimit": "simulator.lookahead_limit", "-ll": "simulator.lookahead_limit",
    "--conf_bits": "simulator.confidence_bits", "-cnf": "simulator.confidence_bits",
    "--l1i_pref": "build.l1i_prefetcher", "-l1ip": "build.l1i_prefetcher",
    "--bp_bp": "simulator.bp_filter", "-bpbp": "simulator.bp_filter",
    "--s1_s": "morrigan.s1_sets", "-s1s": "morrigan.s1_sets",
    "--s1_a": "morrigan.s1_assoc", "-s1a": "morrigan.s1_assoc",
    "--s2_s": "morrigan.s2_sets", "-s2s": "morrigan.s2_sets",
    "--s2_a": "morrigan.s2_assoc", "-s2a": "morrigan.s2_assoc",
    "--s4_s": "morrigan.s4_sets", "-s4s": "morrigan.s4_sets",
    "--s4_a": "morrigan.s4_assoc", "-s4a": "morrigan.s4_assoc",
    "--s8_s": "morrigan.s8_sets", "-s8s": "morrigan.s8_sets",
    "--s8_a": "morrigan.s8_assoc", "-s8a": "morrigan.s8_assoc",
    "--optional": "build.name_suffix", "-opt": "build.name_suffix",
}


def cli() -> tuple[Path, dict[str, str]]:
    parser = argparse.ArgumentParser(
        description="Apply an INI configuration and compile ChampSim.",
        epilog="Example: ./generate_binary.sh configs/tage2.ini",
    )
    parser.add_argument("config", nargs="?", type=Path, help="INI file (default: configs/default.ini)")
    parser.add_argument("--config", dest="named_config", type=Path, help="INI file")
    parser.add_argument("--set", action="append", default=[], metavar="SECTION.KEY=VALUE",
                        help="override one INI value; may be repeated")
    known, extra = parser.parse_known_args()
    overrides: dict[str, str] = {}
    for item in known.set:
        if "=" not in item or "." not in item.split("=", 1)[0]:
            parser.error("--set must have the form SECTION.KEY=VALUE")
        key, value = item.split("=", 1)
        overrides[key.lower()] = value
    i = 0
    while i < len(extra):
        if extra[i] == "--end":
            i += 1
            continue
        if extra[i] not in LEGACY_OPTIONS or i + 1 == len(extra):
            parser.error(f"unknown or incomplete option: {extra[i]}")
        overrides[LEGACY_OPTIONS[extra[i]]] = extra[i + 1]
        i += 2
    return (known.named_config or known.config or DEFAULT_CONFIG), overrides


def load_config(selected: Path, overrides: dict[str, str]) -> configparser.ConfigParser:
    config = configparser.ConfigParser(interpolation=None)
    files = [DEFAULT_CONFIG]
    selected = selected.resolve()
    if selected != DEFAULT_CONFIG.resolve():
        files.append(selected)
    missing = [str(path) for path in files if not path.is_file()]
    if missing:
        raise ValueError(f"configuration file not found: {', '.join(missing)}")
    config.read(files)
    for dotted, value in overrides.items():
        section, key = dotted.split(".", 1)
        if not config.has_section(section) or not config.has_option(section, key):
            raise ValueError(f"unknown configuration knob: {dotted}")
        config.set(section, key, value)
    return config


def value(config: configparser.ConfigParser, section: str, key: str) -> str:
    raw = config.get(section, key).strip()
    if not re.fullmatch(r"[A-Za-z0-9_+.-]*", raw):
        raise ValueError(f"invalid value for {section}.{key}: {raw!r}")
    return raw


def replace_define(relative: str, macro: str, new_value: str) -> None:
    path = ROOT / relative
    source = path.read_text()
    ORIGINAL_SOURCES.setdefault(path, source)
    updated, count = re.subn(
        rf"(?m)^([ \t]*#define[ \t]+{re.escape(macro)}[ \t]+)\S+",
        rf"\g<1>{new_value}", source, count=1,
    )
    if count != 1:
        raise ValueError(f"could not find #define {macro} in {relative}")
    path.write_text(updated)


def configure(c: configparser.ConfigParser) -> list[str]:
    get = lambda section, key: value(c, section, key)
    page_sizes = {"4kb": ("4096", "12"), "2mb": ("2097152", "21")}
    page_size = get("simulator", "page_size").lower()
    if page_size not in page_sizes:
        raise ValueError("simulator.page_size must be 4kb or 2mb")
    replace_define("inc/champsim.h", "PAGE_SIZE", page_sizes[page_size][0])
    replace_define("inc/champsim.h", "LOG2_PAGE_SIZE", page_sizes[page_size][1])

    stlb_modes = {
        "analysis": "STLB_BLOCK_ANALYSIS",
        "detail": "STLB_BLOCK_DETAIL",
    }
    stlb_mode = get("simulator", "stlb_mode").lower()
    if stlb_mode not in stlb_modes:
        raise ValueError("simulator.stlb_mode must be analysis or detail")
    replace_define("inc/cache.h", "DEFAULT_STLB_BLOCK_MODE", stlb_modes[stlb_mode])

    cache_macros = {
        "STLB_SET": "stlb_sets", "STLB_WAY": "stlb_assoc", "STLB_LATENCY": "stlb_latency",
        "STLB_PQ_SIZE": "pq_size", "P2TLB": "prefetch_to_tlb", "ENABLE_FP": "free_prefetching",
        "ENABLE_PREF_FP": "free_prefetching_prefetch", "LA_DEPTH": "lookahead_depth",
        "SUCCESSORS": "successors", "RESET_FREQ": "reset_frequency", "RP_MP": "replacement_policy",
        "LLIMIT": "lookahead_limit", "CNF_BITS": "confidence_bits",
        "RP_SUC_MP": "successor_replacement_policy",
    }
    for macro, key in cache_macros.items():
        replace_define("inc/cache.h", macro, get("simulator", key))
    replace_define("inc/ooo_cpu.h", "ASAP", get("simulator", "asap"))
    replace_define("inc/ooo_cpu.h", "IDEAL", get("simulator", "ideal"))
    replace_define("inc/ooo_cpu.h", "BPBP_FILTER", get("simulator", "bp_filter"))

    stlb = get("build", "stlb_prefetcher")
    if stlb in ("morriganPT", "morriganPT_tage", "morriganPT_tage2"):
        for prefix in ("s1", "s2", "s4", "s8"):
            replace_define("inc/morriganPT.h", f"PT_{prefix.upper()}_SETS", get("morrigan", f"{prefix}_sets"))
            replace_define("inc/morriganPT.h", f"PT_{prefix.upper()}_ASSOC", get("morrigan", f"{prefix}_assoc"))
    if stlb in ("dp", "asp"):
        replace_define(f"prefetcher/{stlb}.stlb_pref", "TABLE_SIZE", get("simulator", "pc_table"))
        replace_define(f"prefetcher/{stlb}.stlb_pref", "ASSOC", get("simulator", "pc_table_assoc"))
    if stlb == "markov_sota":
        replace_define("prefetcher/markov_sota.stlb_pref", "MARKOV_TABLE_SETS", get("simulator", "markov_sets"))
        replace_define("prefetcher/markov_sota.stlb_pref", "MARKOV_ASSOC", get("simulator", "markov_assoc"))
    if stlb == "morriganPT_tage2":
        tage_macros = {
            "THT_H2_SETS": "h2_sets", "THT_H2_ASSOC": "h2_assoc",
            "THT_H4_SETS": "h4_sets", "THT_H4_ASSOC": "h4_assoc",
            "THT_H8_SETS": "h8_sets", "THT_H8_ASSOC": "h8_assoc",
            "TAGE_ALLOC_POLICY": "allocation_policy", "TAGE_USE_U_BIT": "usefulness_bit",
            "TAGE_U_RESET": "usefulness_reset", "TAGE_HIST_DELTA": "delta_history",
            "TAGE_CONF_THRESHOLD": "confidence_threshold",
        }
        for macro, key in tage_macros.items():
            replace_define("prefetcher/morriganPT_tage2.stlb_pref", macro, get("tage", key))

    fp = get("simulator", "free_prefetching_prefetch")
    build_name = get("simulator", "pq_size") + ("nofp" if fp == "0" else "fp") + get("build", "name_suffix")
    return [get("build", key) for key in (
        "branch_predictor", "l1i_prefetcher", "l1d_prefetcher", "l2c_prefetcher",
        "llc_prefetcher", "llc_replacement", "cores", "stlb_prefetcher",
    )] + [build_name]


def main() -> int:
    try:
        selected, overrides = cli()
        config = load_config(selected, overrides)
        args = configure(config)
        print(f"Configuration: {selected.resolve()}")
        subprocess.run([str(ROOT / "build_champsim.sh"), *args], cwd=ROOT, check=True)
    except (ValueError, configparser.Error, subprocess.CalledProcessError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return 1
    finally:
        # Knobs are compile-time inputs, not generated source changes. Restoring
        # them also makes failed and successive builds independent.
        for path, source in ORIGINAL_SOURCES.items():
            path.write_text(source)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
