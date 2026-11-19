#!/usr/bin/env bash
# shellcheck disable=SC2086
# Stage C phase 1 (PLAN.md section 10): pacing sweep, rel_large 128 KiB (4 rate arms) and 1 MiB (4 rate arms + one larger-burst arm), 4 profiles,
# 4 rounds x copies A/A2 = 8 runs per arm and profile, into the Stage A directories. Usage: OUT_BASE=<Stage A dir> ./wtc_chainC1.sh
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${OUT_BASE:?OUT_BASE}
export ROUNDS=4 FIRST_ROUND=1
OUT=$B/rl128k CELLS=rel_large LARGE_SZ=131072 FWS="tickle-p75 tickle-p85 tickle-p90 tickle-p95" "$S/wtc_run.sh"
OUT=$B/rl1m CELLS=rel_large LARGE_SZ=1048576 FWS="tickle-p75 tickle-p85 tickle-p90 tickle-p95 tickle-p90-b24608" "$S/wtc_run.sh"
echo WTC_CHAINC1_DONE
