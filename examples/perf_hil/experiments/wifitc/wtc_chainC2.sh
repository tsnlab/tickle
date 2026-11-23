#!/usr/bin/env bash
# shellcheck disable=SC2086
# Stage C phase 2 (PLAN.md section 10.1): confirmation of the chosen arm tickle-p95 on fresh runs, 10 per cell and profile, into <base>/C/*.
# Usage: OUT_BASE=<run dir that holds A/> ./wtc_chainC2.sh
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${OUT_BASE:?OUT_BASE}
export ROUNDS=5 FIRST_ROUND=1 FWS="tickle-p95"
OUT=$B/C/rl1m CELLS=rel_large LARGE_SZ=1048576 "$S/wtc_run.sh"
OUT=$B/C/rl128k CELLS=rel_large LARGE_SZ=131072 "$S/wtc_run.sh"
OUT=$B/C/small CELLS="be_small rel_small" "$S/wtc_run.sh"
echo WTC_CHAINC2_DONE
