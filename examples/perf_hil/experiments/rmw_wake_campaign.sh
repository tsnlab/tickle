#!/usr/bin/env bash
# rmw_wake_campaign.sh - rmw_wake_split.sh over a list of arms, interleaved rep by rep, then rmw_wake_split.py on every
# run (ROADMAP "rmw kernel wake / handoff", 2026-10-20). PC-only (private netns), no rig lock. Run it detached; it
# ends with "=== ALL_DONE rc=N ===".
#
# ARMS: space-separated "<tag>:<rmw>:<install dir>[:sys[:KEY=VALUE,...]]" - "sys" runs the arm under sysstamp.c
# ($SYSSTAMP); the last field is exported for both nodes (rmw_wake_split.sh's RMW_ENV), commas for spaces.
# GAPS: "<gap s>[:<duration s>] ...", the duration defaulting to DUR.
# Usage: OUTDIR=<dir> ARMS="..." [REPS=3 GAPS="0.005 0.1" QOSES="best_effort" DUR=5 MSG=bench] rmw_wake_campaign.sh
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTDIR=${OUTDIR:?OUTDIR=<dir>}
ARMS=${ARMS:?ARMS=<tag:rmw:install[:sys]> ...}
REPS=${REPS:-3}
GAPS=${GAPS:-0.005 0.1}
QOSES=${QOSES:-best_effort}
DUR=${DUR:-5}
MSG=${MSG:-bench}
SYSSTAMP=${SYSSTAMP:-}
mkdir -p "$OUTDIR"
rc=0
on_exit() { echo "=== ALL_DONE $(date -Is) rc=$rc ==="; }
trap on_exit EXIT
echo "=== rmw_wake_campaign $(date -Is) arms=[$ARMS] reps=$REPS gaps=[$GAPS] qos=[$QOSES] dur=$DUR msg=$MSG load=[$(cut -d' ' -f1-3 /proc/loadavg)] ==="
prefixes=()
for rep in $(seq 1 "$REPS"); do
    for qos in $QOSES; do
        for gap in $GAPS; do
            IFS=: read -r gap gdur <<<"$gap"
            gdur=${gdur:-$DUR}
            for arm in $ARMS; do
                IFS=: read -r tag rmw install sys envs <<<"$arm"
                pre=""
                [ "$sys" = sys ] && pre=$SYSSTAMP
                name="${tag}_${qos}_g${gap}_r${rep}"
                SYSSTAMP=$pre RMW_ENV=${envs//,/ } RMW=$rmw INSTALL=$install OUTDIR=$OUTDIR TAG=$name GAP=$gap DUR=$gdur MSG=$MSG QOS=$qos \
                    timeout $((gdur + 60)) "$HERE/rmw_wake_split.sh" || rc=1
                echo "    load=[$(cut -d' ' -f1-3 /proc/loadavg)]"
                prefixes+=("$OUTDIR/$name")
            done
        done
    done
done
python3 "$HERE/rmw_wake_split.py" "${prefixes[@]}" >"$OUTDIR/split.txt" 2>&1 || rc=1
grep '^TSV' "$OUTDIR/split.txt"
