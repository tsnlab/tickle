#!/usr/bin/env bash
# shellcheck disable=SC2086,SC2030,SC2031  # the rate-step parts run in subshells on purpose (their ROUNDS and arms are local)
# Re-validation of an ADAPTIVE build, second form: (1) rate step for the new tickle-ad only (30 -> 4 at 45 s -> 30 Mbit/s at 90 s, 135 s runs): 1 MiB x 12 runs, 128 KiB x 5;
# (2) static: arm A (tickle-ad, no prior) and arm B (right prior) on M1/M2 at 128 KiB and 1 MiB and S1/S2 at 1 MiB, 3 runs per cell. Usage: OUT_BASE=<dir> ./wtc_chainRV2.sh
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${OUT_BASE:?OUT_BASE}
export CELLS=rel_large COPIES=A FIRST_ROUND=1 LARGE_BIN=large_trace DDS_TRACE=1
(export ARMS=M1 DUR_LARGE=135 RATE_STEP="45:4 90:30" FWS="tickle-ad"
 ROUNDS=12 OUT=$B/RS/rl1m LARGE_SZ=1048576 "$S/wtc_run.sh"
 ROUNDS=5 OUT=$B/RS/rl128k LARGE_SZ=131072 "$S/wtc_run.sh")
export ROUNDS=3 LARGE_BIN=large
OUT=$B/ST/rl128k_M ARMS="M1 M2" FWS="tickle-ad tickle-ad-c30" LARGE_SZ=131072 "$S/wtc_run.sh"
OUT=$B/ST/rl1m_M ARMS="M1 M2" FWS="tickle-ad tickle-ad-c30" LARGE_SZ=1048576 "$S/wtc_run.sh"
OUT=$B/ST/rl1m_S ARMS="S1 S2" FWS="tickle-ad tickle-ad-c4" LARGE_SZ=1048576 "$S/wtc_run.sh"
echo RV2_DONE
