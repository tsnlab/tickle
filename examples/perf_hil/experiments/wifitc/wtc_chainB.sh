#!/usr/bin/env bash
# shellcheck disable=SC2086
# Stage B of PLAN.md (deciding cells, chosen from Stage A before this started): rel_large 1 MiB rounds 3..5 and 128 KiB rounds 3..4 in the Stage A
# directories (runs already done are skipped), all four profiles. Usage: OUT_BASE=<Stage A dir> ./wtc_chainB.sh
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${OUT_BASE:?OUT_BASE}
export PACE_M1=22 PACE_M2=22 PACE_S1=3 PACE_S2=3
OUT=$B/rl1m CELLS=rel_large LARGE_SZ=1048576 FIRST_ROUND=3 ROUNDS=5 "$S/wtc_run.sh"
OUT=$B/rl128k CELLS=rel_large LARGE_SZ=131072 FIRST_ROUND=3 ROUNDS=4 "$S/wtc_run.sh"
echo WTC_CHAINB_DONE
