#!/usr/bin/env bash
# shellcheck disable=SC2086
# Rate-step stage (acceptance A4): M1 (5 ms each way, 1 % loss) stepped 30 -> 4 (at 45 s) -> 30 Mbit/s (at 90 s), 135 s runs, 0.25 Hz, 3 runs per arm;
# arms: tickle-ad (no prior), tickle-ad-c30 (prior 30, wrong after the step), tickle-p95 (fixed 28.2 Mbit/s), tickle-k3770 (fixed 3.77 Mbit/s), CycloneDDS, Fast DDS.
# Usage: OUT_BASE=<dir> ./wtc_chainRS.sh
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${OUT_BASE:?OUT_BASE}
export CELLS=rel_large ARMS=M1 COPIES=A ROUNDS=3 FIRST_ROUND=1 DUR_LARGE=135 RATE_STEP="45:4 90:30" LARGE_BIN=large_trace
export FWS="tickle-ad tickle-ad-c30 tickle-p95 tickle-k3770 cyclonedds fastdds"
OUT=$B/rl1m LARGE_SZ=1048576 "$S/wtc_run.sh"
OUT=$B/rl128k LARGE_SZ=131072 "$S/wtc_run.sh"
echo RS_DONE
