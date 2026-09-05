#!/usr/bin/env bash
# Friendly entry point for INI-driven ChampSim builds.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$ROOT/configure_binary.py" "$@"
