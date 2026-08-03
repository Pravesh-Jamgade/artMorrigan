#!/bin/bash
# Runs one binary on one trace and writes Statistics/<config>/<trace>.txt
#
# TRACE_DIR is taken from the environment if set, otherwise from the default
# below. Override it like:  export TRACE_DIR=/path/to/traces
TRACE_DIR="${TRACE_DIR:-$HOME/mtp/micro-arch/old-work/traces}"

binary=${1}
n_warm=${2}
n_sim=${3}
trace=${4}
option=${5}
extra=${6}

DESTINATION_FOLDER='Statistics'
mkdir -p ${DESTINATION_FOLDER}/${option}

if [ ! -f "${TRACE_DIR}/${trace}.champsimtrace.xz" ]; then
    echo "[ERROR] missing trace: ${TRACE_DIR}/${trace}.champsimtrace.xz" >&2
    exit 1
fi

(./bin/${binary} -warmup_instructions ${n_warm}000000 \
                 -simulation_instructions ${n_sim}000000 ${extra} \
                 -traces ${TRACE_DIR}/${trace}.champsimtrace.xz) \
    &> ${DESTINATION_FOLDER}/${option}/${trace}.txt
