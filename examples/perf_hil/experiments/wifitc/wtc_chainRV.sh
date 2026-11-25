#!/usr/bin/env bash
# shellcheck disable=SC2086,SC2030,SC2031  # the rate-step part runs in a subshell on purpose (its ROUNDS and arms are local)
# Re-validation of an ADAPTIVE build: (1) the exact rate-step run for the new tickle-ad only (30 -> 4 at 45 s -> 30 Mbit/s at 90 s, 135 s runs, 5 runs, 1 MiB and
# 128 KiB; the other arms' data are reused from the earlier exact run), (2) a static test: arm A (tickle-ad, no prior) and arm B (right prior) on M1/M2 at 128 KiB and
# 1 MiB and on S1/S2 at 1 MiB, 3 runs each. Usage: OUT_BASE=<dir> ./wtc_chainRV.sh
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${OUT_BASE:?OUT_BASE}
export CELLS=rel_large COPIES=A FIRST_ROUND=1 LARGE_BIN=large_trace DDS_TRACE=1
(export ARMS=M1 ROUNDS=5 DUR_LARGE=135 RATE_STEP="45:4 90:30" FWS="tickle-ad"
 OUT=$B/RS/rl1m LARGE_SZ=1048576 "$S/wtc_run.sh"
 OUT=$B/RS/rl128k LARGE_SZ=131072 "$S/wtc_run.sh")
export ROUNDS=3 LARGE_BIN=large
OUT=$B/ST/rl128k_M ARMS="M1 M2" FWS="tickle-ad tickle-ad-c30" LARGE_SZ=131072 "$S/wtc_run.sh"
OUT=$B/ST/rl1m_M ARMS="M1 M2" FWS="tickle-ad tickle-ad-c30" LARGE_SZ=1048576 "$S/wtc_run.sh"
OUT=$B/ST/rl1m_S ARMS="S1 S2" FWS="tickle-ad tickle-ad-c4" LARGE_SZ=1048576 "$S/wtc_run.sh"
echo RV_DONE
