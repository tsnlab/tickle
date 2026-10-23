#!/usr/bin/env bash
# kh2_ab_pc.sh - PC A/B of the wait ring's first move (ab/kh2-receive-return, RESULTS KH2), parent against branch, with
# a control the change cannot touch. Written 2026-10-24 before any of its runs; the reading rules are code in
# kh2_ab_read.py (its docstring says them), run at the end.
#
# Each run is one rmw_wake_split.sh (two private netns over veth, nothing in the default namespace): pong_node and
# ping_node --wait block, Bench BEST_EFFORT, both under sysstamp. Arms: P / B the parent's and the branch's libc build
# (kh2_pc_build.sh ... -DTT_HAL_LIBC_CALLS, so sysstamp sees the HAL's calls), PC / BC the same builds with
# TT_RX_URING_WAIT=0 (no ring: the move rule never runs - the control). Every rep runs the four arms once, in an order
# rotated by one per rep. The 1-minute load average is read before each run; the reader voids a run above 4.
#
# Usage (detached; results outside the session):
#   setsid nohup env PARENT=<install> BRANCH=<install> SYSSTAMP=<libsysstamp.so> OUT=<dir> \
#     examples/perf_hil/experiments/kh2_ab_pc.sh > <dir>.log 2>&1 < /dev/null &
#   env: REPS (6) GAP (0.005 s) DUR (2 s) PIN_PING (4-5) PIN_PONG (2-3)
# ESTIMATE: a run ~8 s (3 s for the pong to start, DUR, teardown); 4 arms x 6 reps = 24 runs, ~4 min.
# The last line is always "KH2_AB_END rc=<n> <date>".
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT=${PARENT:?PARENT=<parent install dir>}
BRANCH=${BRANCH:?BRANCH=<branch install dir>}
SYSSTAMP=${SYSSTAMP:?SYSSTAMP=<libsysstamp.so>}
OUT=${OUT:?OUT=<results dir>}
REPS=${REPS:-6}
GAP=${GAP:-0.005}
DUR=${DUR:-2}
PIN_PING=${PIN_PING:-4-5}
PIN_PONG=${PIN_PONG:-2-3}
rc=1
trap 'echo "KH2_AB_END rc=$rc $(date -Is)"' EXIT
for d in "$PARENT" "$BRANCH"; do
    [ -f "$d/rmw_tickle/lib/librmw_tickle.so" ] || { echo "REFUSED: no librmw_tickle.so under $d"; exit 1; }
done
[ -f "$SYSSTAMP" ] || { echo "REFUSED: no $SYSSTAMP"; exit 1; }
mkdir -p "$OUT" || exit 1
python3 "$HERE/kh2_ab_read.py" --selftest | tail -1 | grep -qx "SELFTEST OK" || { echo "REFUSED: reader selftest"; exit 1; }
M=$OUT/manifest.txt
: >"$M"
echo "kh2_ab_pc: parent $PARENT ($(cat "$PARENT/../BUILD_SHA" 2>/dev/null)), branch $BRANCH ($(cat "$BRANCH/../BUILD_SHA" 2>/dev/null))" | tee -a "$M"
echo "reps $REPS gap $GAP dur $DUR pin ping $PIN_PING pong $PIN_PONG; uptime: $(uptime)" | tee -a "$M"
arms=(P B PC BC)
for rep in $(seq 1 "$REPS"); do
    for i in 0 1 2 3; do
        arm=${arms[$(((i + rep) % 4))]}
        case $arm in
        P) inst=$PARENT env= ;;
        B) inst=$BRANCH env= ;;
        PC) inst=$PARENT env=TT_RX_URING_WAIT=0 ;;
        BC) inst=$BRANCH env=TT_RX_URING_WAIT=0 ;;
        esac
        load=$(cut -d' ' -f1 /proc/loadavg)
        tag=${arm}_r$rep
        line=$(
            ulimit -v 2097152
            INSTALL=$inst RMW=rmw_tickle RMW_ENV="$env" GAP=$GAP DUR=$DUR MSG=bench QOS=best_effort OUTDIR=$OUT \
                TAG=$tag SYSSTAMP=$SYSSTAMP PIN_PING=$PIN_PING PIN_PONG=$PIN_PONG timeout 120 "$HERE/rmw_wake_split.sh" \
                2>&1 | grep '^RUN ' | tail -1
        )
        status=VOID
        case "$line" in *" ok loaded="*) status=ok ;; esac
        echo "RUNMETA $arm $rep $load $status $OUT/$tag" >>"$M"
        echo "$(date +%T) $arm rep$rep load $load $status | ${line:0:200}"
    done
done
python3 "$HERE/kh2_ab_read.py" "$OUT" | tee "$OUT/reading.txt"
rc=${PIPESTATUS[0]}
