#!/usr/bin/env bash
# shellcheck disable=SC2086
# Rate-step stage, exact version: M1 stepped 30 -> 4 (45 s) -> 30 Mbit/s (90 s), 135 s runs, 0.25 Hz, 5 runs per arm, 1 MiB then 128 KiB; arms tickle-ad (ADAPTIVE,
# no prior), tickle-p95 (fixed 28.2 Mbit/s), cyclonedds, fastdds; per-sample traces for every arm (large_trace LAT lines, DDS nodes -L files); the step
# times are recorded on the sender Pi's monotonic clock (WTC_CLI_STEP_MONO_NS in <run>.steps). Usage: OUT_BASE=<dir> ./wtc_chainRS2.sh
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${OUT_BASE:?OUT_BASE}
export CELLS=rel_large ARMS=M1 COPIES=A ROUNDS=5 FIRST_ROUND=1 DUR_LARGE=135 RATE_STEP="45:4 90:30" LARGE_BIN=large_trace DDS_TRACE=1
export FWS="tickle-ad tickle-p95 cyclonedds fastdds"
OUT=$B/rl1m LARGE_SZ=1048576 "$S/wtc_run.sh"
OUT=$B/rl128k LARGE_SZ=131072 "$S/wtc_run.sh"
echo RS2_DONE
