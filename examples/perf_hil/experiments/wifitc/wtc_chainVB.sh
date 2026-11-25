#!/usr/bin/env bash
# shellcheck disable=SC2086
# ADAPTIVE under time-varying bandwidth: base link M1 (5 ms one way, 1 % loss, queue 100), the netem rate follows a schedule from wtc_schedules.py (applied by the rate-step
# driver of wtc_run.sh on both taps, step markers on the sender Pi's monotonic clock), 0.25 Hz, 1 MiB and 128 KiB, arms tickle-ad (ADAPTIVE, no prior), tickle-p95 (fixed
# 28.2 Mbit/s), cyclonedds, fastdds, 3 runs per cell, per-sample traces for every arm. Order: P7 P5a P5b P6 P3 P4 (most informative first). Usage: OUT_BASE=<dir> ./wtc_chainVB.sh [patterns]
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${OUT_BASE:?OUT_BASE}
PATTERNS=${*:-"P7 P5a P5b P6 P3 P4"}
export CELLS=rel_large ARMS=M1 COPIES=A ROUNDS=${ROUNDS:-3} FIRST_ROUND=1 LARGE_BIN=large_trace DDS_TRACE=1 FWS="tickle-ad tickle-p95 cyclonedds fastdds"
mkdir -p "$B/schedules"
for p in $PATTERNS; do
    read -r dur steps < <(python3 "$S/wtc_schedules.py" "$p")
    echo "$p duration=${dur}s seed=20261126 schedule: $steps" | tee "$B/schedules/$p.txt"
    for sz in 1048576:rl1m 131072:rl128k; do
        RATE_STEP="$steps" DUR_LARGE=$dur OUT=$B/$p/${sz##*:} LARGE_SZ=${sz%%:*} "$S/wtc_run.sh"
    done
    echo "PATTERN_DONE $p"
done
echo VB_DONE
