#!/bin/bash
# p_mtu's bias, cell by cell (pmtu_bias_patch.py): one binary, seeds <first>..<last> of every cell below, one process a
# run under a 2 GB address-space limit and a 600 s timeout.
#
#   pmtu_bias_sweep.sh <binary> <out_file> <first> <last>
#
# Cells: frame loss 0.1%, 1%, 5% x {16 KB forced IP at n = 11 (one run and the tail), 16 KB forced IP at n <= 7 (two
# runs and the tail), 16 KB AUTO with the reader's cap on}. Appends each run's PMTU_R line (or "PMTU_FAIL <cell> rc")
# to <out_file>, then "PMTU_SWEEP_DONE" as its last line, whatever happened before. pmtu_bias_read.py reads it.
set -u
bin=$1
out=$2
first=$3
last=$4
: > "$out"
trap 'echo PMTU_SWEEP_DONE >> "$out"' EXIT
for ppm in 1000 10000 50000; do
    case $ppm in
    1000) secs=300 ;;
    10000) secs=120 ;;
    *) secs=60 ;;
    esac
    for cell in 0:0 7:0 0:1; do
        bound=${cell%:*}
        auto=${cell#*:}
        for seed in $(seq "$first" "$last"); do
            line=$(
                ulimit -v 2097152
                CFP_SEED=$seed CFP_TESTS=cfp_pmtu PMTU_PPM=$ppm PMTU_LEN=16384 PMTU_BOUND=$bound PMTU_AUTO=$auto \
                    PMTU_SECS=$secs timeout 600 "$bin" 2> /dev/null | grep '^PMTU_R'
            )
            if [ -n "$line" ]; then
                echo "$line" >> "$out"
            else
                echo "PMTU_FAIL ppm=$ppm bound=$bound auto=$auto seed=$seed" >> "$out"
            fi
        done
    done
done
