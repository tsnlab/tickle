#!/usr/bin/env bash
# shellcheck disable=SC2086
# Adaptive rate control on the tc-emulated link (ADAPTIVE_PLAN.md): the stages that put the ADAPTIVE arm ("tickle-ad") next to the DDS stacks and the
# earlier TickLE arms, through wtc_run.sh. Usage: OUT_BASE=<results dir> ./wtc_chainD.sh s1|all|step|readers
#   s1       S1 (4 Mbit/s, 5 ms, 1 % loss): 1 MiB and 128 KiB rel_large, plus the small cells; tickle-ad, tickle (default), tickle-p95, cyclonedds, fastdds
#   all      the same on M1 M2 S1 S2
#   step     M1 stepped 30 -> 4 (at 8 s) -> 30 (at 24 s) Mbit/s while 1 MiB samples go at 0.25 Hz, 48 s runs
#   readers  2 and 3 reader processes on the link (TickLE arms only: the runner starts the extra readers with TT_CONTEXT_ID), on S1 and M1, 1 MiB
# Each stage takes the Pis' rig lock through wtc_run.sh; stages run one after another, never in parallel.
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${OUT_BASE:?OUT_BASE}
stage=${1:?usage: wtc_chainD.sh s1|all|step|readers}
export FIRST_ROUND=1
FWS_ALL=${FWS_ALL:-"tickle-ad tickle tickle-p95 cyclonedds fastdds"}
FWS_SMALL=${FWS_SMALL:-"tickle-ad tickle-lg tickle cyclonedds fastdds"}
case $stage in
s1 | all)
    [ $stage = s1 ] && arms="S1" || arms="M1 M2 S1 S2"
    export ROUNDS=${ROUNDS:-5} ARMS="$arms" FWS="$FWS_ALL"
    OUT=$B/D_$stage/rl1m CELLS=rel_large LARGE_SZ=1048576 "$S/wtc_run.sh"
    OUT=$B/D_$stage/rl128k CELLS=rel_large LARGE_SZ=131072 "$S/wtc_run.sh"
    OUT=$B/D_$stage/small CELLS="be_small rel_small" FWS="$FWS_SMALL" "$S/wtc_run.sh"
    ;;
step)
    export ROUNDS=${ROUNDS:-5} ARMS="M1" FWS="tickle-ad tickle cyclonedds fastdds" DUR_LARGE=48
    OUT=$B/D_step/rl1m CELLS=rel_large LARGE_SZ=1048576 RATE_STEP="8:4 24:30" "$S/wtc_run.sh"
    ;;
readers)
    export ROUNDS=${ROUNDS:-5} FWS="tickle-ad tickle" COPIES="A"
    for n in 2 3; do
        OUT=$B/D_readers/n$n/S1 ARMS="S1" CELLS=rel_large LARGE_SZ=1048576 READERS=$n "$S/wtc_run.sh"
        OUT=$B/D_readers/n$n/M1 ARMS="M1" CELLS=rel_large LARGE_SZ=1048576 READERS=$n "$S/wtc_run.sh"
    done
    ;;
*) echo "usage: wtc_chainD.sh s1|all|step|readers" >&2; exit 2 ;;
esac
echo "WTC_CHAIND_${stage}_DONE"
