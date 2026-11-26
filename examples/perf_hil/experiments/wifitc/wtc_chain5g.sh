#!/usr/bin/env bash
# shellcheck disable=SC2086
# Synthetic 5G-like patterns N1-N4 (wtc_schedules5g.py, schedules in results/5g_schedules/), played by wtc_netem_player.py on both taps; arms tickle-ad (ADAPTIVE, no prior),
# fixed (pacing_rate 95 % of the schedule's time-weighted mean rate, tickle-k<kbit>), cyclonedds, fastdds; 1 MiB and 128 KiB at 0.25 Hz; 180 s; 3 runs per cell; per-sample traces.
# Usage: OUT_BASE=<dir> ./wtc_chain5g.sh [patterns]      96 runs for the four patterns
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${OUT_BASE:?OUT_BASE}
PATTERNS=${*:-"N1 N2 N3 N4"}
export CELLS=rel_large COPIES=A ROUNDS=3 FIRST_ROUND=1 LARGE_BIN=large_trace DDS_TRACE=1 DUR_LARGE=180
for p in $PATTERNS; do
    K=$(python3 -c "import json,sys; print(int(0.95*json.load(open('$S/results/5g_schedules/$p.json'))['mean_rate_kbit']))")
    echo "$p fixed arm: tickle-k$K (95 % of the mean rate)"
    for sz in 1048576:rl1m 131072:rl128k; do
        NETEM_SCHEDULE=$p ARMS=$p FWS="tickle-ad tickle-k$K cyclonedds fastdds" OUT=$B/$p/${sz##*:} LARGE_SZ=${sz%%:*} "$S/wtc_run.sh"
    done
    echo "PATTERN_DONE $p"
done
echo FIVEG_DONE
