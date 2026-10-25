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
    "--stlb_ptes": "simulator.stlb_ptes_per_block",
    "--bound": "simulator.bound",
    "--ptw_bound": "simulator.bound",
    "--latency": "simulator.latency",
    "--latency_mode": "simulator.latency",
    "--ptw_latency": "simulator.latency",
    "--traffic_only_packets": "simulator.traffic_only_packets",
    "--ptw_traffic_packets": "simulator.traffic_only_packets",
    "--stlb_oracle": "simulator.stlb_oracle",
    "--ptw_start_level": "simulator.ptw_start_level",
    "--flush_on_end": "simulator.flush_on_end",
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


def cli() -> tuple[list[Path], dict[str, str]]:
    parser = argparse.ArgumentParser(
        description="Apply one or more INI configurations and compile ChampSim.",
        epilog="Example: ./generate_binary.sh configs/tage2.ini",
    )
    parser.add_argument("configs", nargs="*", type=Path, help="INI file(s) applied in order (default: configs/default.ini)")
    parser.add_argument("--config", dest="named_configs", action="append", default=[], type=Path, help="INI file (may be repeated)")
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
    selected_list = [*known.configs, *known.named_configs]
    if not selected_list:
        selected_list = [DEFAULT_CONFIG]
    return selected_list, overrides


def resolve_include_path(raw_path: str, parent_file: Path) -> Path:
    candidate = Path(raw_path.strip().strip("\"'"))
    if candidate.is_absolute():
        return candidate.resolve()
    search_dirs = [parent_file.parent, ROOT / "configs", ROOT]
    for base in search_dirs:
        resolved = (base / candidate).resolve()
        if resolved.is_file():
            return resolved
    return (parent_file.parent / candidate).resolve()


def collect_config_chain(config_path: Path, visited: set[Path] | None = None) -> list[Path]:
    if visited is None:
        visited = set()
    resolved = config_path.resolve()
    if resolved in visited:
        raise ValueError(f"circular config include detected for: {resolved}")
    if not resolved.is_file():
        raise ValueError(f"configuration file not found: {resolved}")
    visited.add(resolved)

    raw_text = resolved.read_text()
    include_specs: list[str] = []

    # Support top-level 'include = ...' or '#include ...' before any section header
    body_lines: list[str] = []
    in_section = False
    for line in raw_text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            in_section = True
        if not in_section:
            m = re.match(r"^(?:include|extends|#include)\s*(?:=|\s)\s*(.+)$", stripped, re.IGNORECASE)
            if m:
                include_specs.append(m.group(1))
                continue
        body_lines.append(line)

    temp_parser = configparser.ConfigParser(interpolation=None)
    temp_parser.read_string("\n".join(body_lines), source=str(resolved))

    for sec in temp_parser.sections():
        for key in ("include", "extends"):
            if temp_parser.has_option(sec, key):
                include_specs.append(temp_parser.get(sec, key))

    chain: list[Path] = []
    for spec in include_specs:
        for part in re.split(r"[,\s]+", spec.strip()):
            if not part:
                continue
            inc_path = resolve_include_path(part, resolved)
            chain.extend(collect_config_chain(inc_path, visited.copy()))
    chain.append(resolved)
    return chain


def read_single_ini(config: configparser.ConfigParser, path: Path) -> None:
    body_lines: list[str] = []
    in_section = False
    for line in path.read_text().splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            in_section = True
        if not in_section and re.match(r"^(?:include|extends|#include)\s*(?:=|\s)\s*(.+)$", stripped, re.IGNORECASE):
            continue
        body_lines.append(line)
    config.read_string("\n".join(body_lines), source=str(path))
    for sec in config.sections():
        for key in ("include", "extends"):
            if config.has_option(sec, key):
                config.remove_option(sec, key)


def load_config(selected_list: list[Path], overrides: dict[str, str]) -> configparser.ConfigParser:
    config = configparser.ConfigParser(interpolation=None)
    files: list[Path] = [DEFAULT_CONFIG.resolve()]
    for selected in selected_list:
        for path in collect_config_chain(selected):
            if path not in files or path != DEFAULT_CONFIG.resolve():
                files.append(path)
    for path in files:
        read_single_ini(config, path)
    if config.has_option("simulator", "latency_mode"):
        config.set("simulator", "latency", config.get("simulator", "latency_mode"))
    for dotted, value in overrides.items():
        section, key = dotted.split(".", 1)
        if section == "simulator" and key == "latency_mode":
            key = "latency"
        if not config.has_section(section):
            config.add_section(section)
        config.set(section, key, value)
    return config


def value(config: configparser.ConfigParser, section: str, key: str) -> str:
    raw = config.get(section, key).strip()
    if not re.fullmatch(r"[A-Za-z0-9_+.,-]*", raw):
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


def generate_config_header(c: configparser.ConfigParser, config_path: Path) -> None:
    path = ROOT / "inc" / "config_params.h"
    if path.exists():
        ORIGINAL_SOURCES.setdefault(path, path.read_text())
    lines = [
        '#ifndef CONFIG_PARAMS_H',
        '#define CONFIG_PARAMS_H',
        '',
        '#include <iostream>',
        '',
        'inline void print_config_file_params() {',
        '    std::cout << "=== CONFIG FILE KEY-VALUES ===" << std::endl;',
        f'    std::cout << "config_file: {config_path.name}" << std::endl;',
    ]
    for section in c.sections():
        for key, val in c.items(section):
            lines.append(f'    std::cout << "[{section}] {key} = {val}" << std::endl;')
    lines.append('    std::cout << "==============================" << std::endl << std::endl;')
    lines.append('}')
    lines.append('#endif')
    lines.append('')
    path.write_text('\n'.join(lines))


def configure(c: configparser.ConfigParser, config_path: Path) -> list[str]:
    generate_config_header(c, config_path)
    get = lambda section, key: value(c, section, key)
    page_size_str = get("simulator", "page_size").lower()
    if page_size_str.endswith("kb"):
        page_size_val = int(page_size_str[:-2]) * 1024
    elif page_size_str.endswith("mb"):
        page_size_val = int(page_size_str[:-2]) * 1024 * 1024
    elif page_size_str.endswith("b"):
        page_size_val = int(page_size_str[:-1])
    else:
        page_size_val = int(page_size_str)
        
    import math
    log2_page_size = int(math.log2(page_size_val))
    
    replace_define("inc/champsim.h", "PAGE_SIZE", str(page_size_val))
    replace_define("inc/champsim.h", "LOG2_PAGE_SIZE", str(log2_page_size))

    stlb_modes = {
        "analysis": "STLB_BLOCK_ANALYSIS",
        "detail": "STLB_BLOCK_DETAIL",
        "sparsity": "STLB_BLOCK_SPARSITY",
    }
    stlb_mode = get("simulator", "stlb_mode").lower()
    if stlb_mode not in stlb_modes:
        raise ValueError("simulator.stlb_mode must be analysis, detail, or sparsity")
    replace_define("inc/cache.h", "DEFAULT_STLB_BLOCK_MODE", stlb_modes[stlb_mode])

    stlb_ptes = get("simulator", "stlb_ptes_per_block")
    if stlb_ptes not in {"1", "2", "4", "8"}:
        raise ValueError("simulator.stlb_ptes_per_block must be 1, 2, 4, or 8")
    replace_define("inc/cache.h", "STLB_PTES_PER_BLOCK", stlb_ptes)

    bound_val = 0
    bound_str = get("simulator", "bound").lower()
    tokens = [t.strip() for t in re.split(r"[,+\s]+", bound_str) if t.strip()]
    if not tokens:
        tokens = ["none"]
    for t in tokens:
        if t == "none":
            pass
        elif t == "dram":
            bound_val |= 1
        elif t == "llc":
            bound_val |= 2
        elif t == "all":
            bound_val |= 3
        else:
            raise ValueError(f"unknown bound option: {t!r} in {bound_str!r}. Expected none, dram, llc, or combination like llc,dram")

    bound_mode_defines = {
        0: "PTW_BOUND_NONE",
        1: "PTW_BOUND_DRAM",
        2: "PTW_BOUND_LLC",
        3: "PTW_BOUND_ALL",
    }
    replace_define("inc/cache.h", "DEFAULT_PTW_BOUND_MODE", bound_mode_defines[bound_val])

    latency_modes = {
        "none": "PTW_LATENCY_NONE",
        "stlb": "PTW_LATENCY_STLB",
        "l2": "PTW_LATENCY_L2",
        "l2_llc": "PTW_LATENCY_L2_LLC",
        "l2+llc": "PTW_LATENCY_L2_LLC",
    }
    latency_mode = get("simulator", "latency").lower()
    if latency_mode not in latency_modes:
        raise ValueError("simulator.latency must be none, stlb, l2, l2_llc, or l2+llc")
    if (bound_val & 1) and latency_mode not in {"stlb", "l2", "l2_llc", "l2+llc"}:
        raise ValueError("when simulator.bound includes dram, simulator.latency must be stlb, l2, l2_llc, or l2+llc")
    if (bound_val & 2) and not (bound_val & 1) and latency_mode not in {"stlb", "l2"}:
        raise ValueError("when simulator.bound is only llc, simulator.latency must be stlb or l2")
    replace_define("inc/cache.h", "DEFAULT_PTW_LATENCY_MODE", latency_modes[latency_mode])

    ptw_start_levels = {"l1": "1", "l2": "2"}
    ptw_start_level = get("simulator", "ptw_start_level").lower()
    if ptw_start_level not in ptw_start_levels:
        raise ValueError("simulator.ptw_start_level must be l1 or l2")
    replace_define("inc/cache.h", "PTW_START_LEVEL", ptw_start_levels[ptw_start_level])

    cache_macros = {
        "STLB_SET": "stlb_sets", "STLB_WAY": "stlb_assoc", "STLB_LATENCY": "stlb_latency",
        "STLB_PQ_SIZE": "pq_size", "P2TLB": "prefetch_to_tlb", "ENABLE_FP": "free_prefetching",
        "ENABLE_PREF_FP": "free_prefetching_prefetch", "LA_DEPTH": "lookahead_depth",
        "SUCCESSORS": "successors", "RESET_FREQ": "reset_frequency", "RP_MP": "replacement_policy",
        "LLIMIT": "lookahead_limit", "CNF_BITS": "confidence_bits",
        "RP_SUC_MP": "successor_replacement_policy", "FLUSH_ON_END": "flush_on_end",
        "TRAFFIC_ONLY_PACKETS": "traffic_only_packets", "STLB_ORACLE": "stlb_oracle",
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
        selected_list, overrides = cli()
        config = load_config(selected_list, overrides)
        primary_config = selected_list[-1]
        args = configure(config, primary_config)
        print(f"Configuration: {', '.join(str(p.resolve()) for p in selected_list)}")
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
