#!/usr/bin/env bash
# shellcheck disable=SC2086
# Stage A of PLAN.md: small cells, rel_large 128 KiB and 1 MiB, then the zenoh-pico reference round. Pacing rates from the calibration
# (0.75 x carried rate, rounded: M 29.73 Mbit/s -> 22, S 3.97 Mbit/s -> 3). Usage: OUT_BASE=<dir> ./wtc_chain.sh
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${OUT_BASE:?OUT_BASE}
export PACE_M1=22 PACE_M2=22 PACE_S1=3 PACE_S2=3 ROUNDS=${ROUNDS:-2} FIRST_ROUND=${FIRST_ROUND:-1}
OUT=$B/small CELLS="be_small rel_small" "$S/wtc_run.sh"
OUT=$B/rl128k CELLS=rel_large LARGE_SZ=131072 "$S/wtc_run.sh"
OUT=$B/rl1m CELLS=rel_large LARGE_SZ=1048576 "$S/wtc_run.sh"
if [ "${REF:-1}" = 1 ]; then
    OUT=$B/ref_small CELLS="be_small rel_small" FWS=zenohpico COPIES=A ROUNDS=1 "$S/wtc_run.sh"
    OUT=$B/ref_rl128k CELLS=rel_large LARGE_SZ=131072 FWS=zenohpico COPIES=A ROUNDS=1 "$S/wtc_run.sh"
    OUT=$B/ref_rl1m CELLS=rel_large LARGE_SZ=1048576 FWS=zenohpico COPIES=A ROUNDS=1 "$S/wtc_run.sh"
fi
echo WTC_CHAIN_DONE
