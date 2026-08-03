#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# setup.sh — install everything needed to build and run this project on Ubuntu
# Usage:  bash setup.sh
# ─────────────────────────────────────────────────────────────────────────────
set -e

echo "==> Installing system packages (requires sudo)"
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    g++ \
    make \
    xz-utils \
    python3 \
    python3-pip \
    python3-venv \
    bc

echo
echo "==> Setting up a Python virtual environment for the analysis scripts"
HERE="$(cd "$(dirname "$0")" && pwd)"
python3 -m venv "$HERE/.venv"
# shellcheck disable=SC1091
source "$HERE/.venv/bin/activate"
pip install --upgrade pip
pip install pandas matplotlib numpy

echo
echo "==> Versions"
g++ --version | head -1
make --version | head -1
python3 --version
python3 -c "import pandas, matplotlib, numpy; print('pandas', pandas.__version__, '| matplotlib', matplotlib.__version__, '| numpy', numpy.__version__)"

echo
echo "==> Done."
echo "    Activate the venv before running analyze.py:"
echo "      source $HERE/.venv/bin/activate"
