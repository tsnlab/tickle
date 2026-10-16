#!/bin/bash
# Seeds <first>..<last> of the collapse-false-positive sweep binary (collapse_fp_patch.py builds its tree; build it
# with cc -O2 from <out>/tree/tests). One process per seed, under a 2 GB address-space limit and a timeout.
#
#   collapse_fp_sweep.sh <binary> <out_dir> <label> <first> <last> <opt> [tests]
#
# Writes <out_dir>/<label>/<seed>.log (the run's stderr + stdout: CFP_D decisions, CFP_T per-test failures and time,
# CFP_R scenario results, the core's log lines) and <out_dir>/<label>.verdicts (seed rc), then "CFP_SWEEP_DONE <label>"
# as its last line, whatever happened before. Extra environment (CFP_LOSS, CFP_SIZE, ...) passes through.
set -u
bin=$1
out=$2
label=$3
first=$4
last=$5
opt=$6
tests=${7:-}
mkdir -p "$out/$label"
: > "$out/$label.verdicts"
trap 'echo "CFP_SWEEP_DONE $label" >> "$out/$label.verdicts"' EXIT
for seed in $(seq "$first" "$last"); do
    if [ -n "$tests" ]; then
        (ulimit -v 2097152; CFP_SEED=$seed CFP_OPT=$opt CFP_TESTS=$tests timeout 600 "$bin" > "$out/$label/$seed.log" 2>&1)
    else
        (ulimit -v 2097152; CFP_SEED=$seed CFP_OPT=$opt timeout 600 "$bin" > "$out/$label/$seed.log" 2>&1)
    fi
    echo "$seed $?" >> "$out/$label.verdicts"
done
