#!/usr/bin/env bash
# pub_hugepage_ab.sh - A/B of huge pages for the publisher's large buffers (branch ab/pub-hugepage: rmw_large.c,
# RMW_TICKLE_PUBLISH_HUGEPAGE, on by default) against main, on the rig. Written 2026-10-18 before any of its runs.
#
# Why: i4be_close_rig_20261018-102207 (5 reps, unpinned) ran huge pages for BOTH ends' large buffers: the publisher's
# task-clock fell 0.122 ms/MB (floor 0.035; pub cycles:k -0.71M, cycles:u -0.19M a sample), 168% of its gap to Fast
# DDS, while the subscriber's rose 0.058 ms/MB (floor 0.037; cycles:k +1.33M). The change keeps the publisher's half:
# a publish buffer of 2 MiB or more is 2 MiB-aligned and advised MADV_HUGEPAGE; a reassembly buffer is not.
#
# Arms (two binaries, interleaved, the order rotated per rep - rmw_keepall_rig.sh):
#   tickle@pre   main (PRE_SHA): the baseline, built as a librmw_tickle.so-only variant
#   tickle@head  the change (HEAD_SHA, ab/pub-hugepage)
#   tickle@aa    the change again under a second name: the A/A floor
#   fastdds      the vendor, the control no rmw_tickle change can touch (drift between the arms' runs)
# Two phases under ONE rig lock (a queued campaign cannot cut in between them):
#   A  I4 best effort: Array4m, BEST_EFFORT KEEP_LAST 5, 15 Hz, perf stat --per-thread on each perf_test over a steady
#      window (PERF_ATTACH) - the primary: the PUBLISHER's task-clock per MB.
#   B  no-harm cells at 30 Hz, same QoS: I4s (Array4m, saturating) and I1 (Array1m). Huge pages reach every large
#      publish of 2 MiB or more; I1's 1 MB sample is below one huge page, so the change cannot touch it - I1 doubles as
#      a control for the change's own bookkeeping (the publish mark, the buffer kind).
#
# HOW TO READ IT: pub_hugepage_read.py, whose header holds the rules (treatment per run from rmw_tickle's own pool
# line, the A/A floor, the primary, the no-harm checks, the verdict). Written before the run and enforced in its code.
#
# Modes:
#   PC=1   PC check (rmw_keepall_rig.sh PC_PREFLIGHT=1): two private network namespaces and a veth, the publisher's
#          egress shaped to 1 Gbit/s (PF_SHAPE tbf); the PC's preflight builds of HEAD and of PRE_SHA (PF_BUILD,
#          PF_VARIANT_pre - pc_preflight_build.sh); the build's own socket buffer (i4be_close_gap.sh BUFFERS=pc: the
#          Pis' buffer drops datagrams at the x86 subscriber). Whether the PC can back the advice with a huge page
#          depends on its free memory (thp_fault_fallback); the PC reading is about treatment, harm and fallback.
#          Run it under `taskset -c 0-3 nice -n 19` (the PC is shared).
#   rig    (default) takes the rig lock inside this process; HEAD_SHA and PRE_SHA must be pushed (the Pis fetch them).
# ESTIMATE: rig: build 2-5 min + A 5 reps x 4 arms x ~85 s (~28 min) + B 2 cells x 5 reps x 4 arms x ~75 s (~50 min)
#   = ~85 min; PC: 3 reps, ~35 min. Analyse an overrun at 1.5 x. The last line is always "PUB_HUGEPAGE_AB rc=<n> ...".
# Usage:
#   PC=1 PRE_SHA=<main> setsid nohup taskset -c 0-3 nice -n 19 examples/perf_hil/experiments/pub_hugepage_ab.sh \
#     > ~/largemsg_pc/pub_hugepage/pc.log 2>&1 < /dev/null &
#   HEAD_SHA=<pushed sha of ab/pub-hugepage> PRE_SHA=<pushed sha of main> RIG_LOCK_WAIT=36000 setsid nohup \
#     examples/perf_hil/experiments/pub_hugepage_ab.sh > ~/rig_results_safe/pub_hugepage_queue.log 2>&1 < /dev/null &
set -u
X="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$X/../../.." && pwd)"
PC=${PC:-0}
ARMS=${ARMS:-"tickle@pre tickle@head tickle@aa fastdds"}
say() { echo "$(date -Is) $*"; }
: "${PRE_SHA:?set PRE_SHA to main (pushed for the rig)}"
if [ "$PC" != 1 ] && [ "${RIG_LOCK_HELD_HIL:-0}" != 1 ]; then
    : "${HEAD_SHA:?set HEAD_SHA to a pushed commit of ab/pub-hugepage}"
    say "=== publisher huge pages: head $HEAD_SHA, pre $PRE_SHA, harness $X; taking the rig lock (hil) ==="
    export RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000} TMPDIR=/home/semih/tmp_build
    exec "$X/../rig_lock.sh" "$0" "$@"
fi
rc=1
trap 'echo "PUB_HUGEPAGE_AB rc=$rc $(date -Is)"' EXIT
TS=$(date +%Y%m%d-%H%M%S)
QOS="--keep_last --history_depth 5"
if [ "$PC" = 1 ]; then
    REPS=${REPS:-3} DUR=${DUR:-40} ATTACH=${ATTACH:-8_25}
    OUTDIR=${OUTDIR:-$HOME/largemsg_pc/pub_hugepage}
    mkdir -p "$OUTDIR" || exit
    OUT=$OUTDIR/pub_hugepage_pc_$TS
    d=${PF_BUILD:-}
    [ -n "$d" ] || d=$("$X/pc_preflight_build.sh" --find) || { say "no PC preflight build for HEAD"; exit; }
    # The baseline's build: pc_preflight_build.sh run at PRE_SHA (its --find from a checkout of PRE_SHA names it).
    pre=${PF_VARIANT_pre:?PC=1 needs PF_VARIANT_pre, the install of a PC preflight build of PRE_SHA}
    [ -f "$pre/rmw_tickle/lib/librmw_tickle.so" ] || { say "no librmw_tickle.so under PF_VARIANT_pre=$pre"; exit; }
    say "=== PC: head build $d, pre $pre, $REPS reps x ${DUR} s, attach $ATTACH, arms: $ARMS, load $(cut -d' ' -f1-3 /proc/loadavg), $(grep thp_fault_fallback' ' /proc/vmstat) ==="
    common=(INSN_COUNT=0 CLOCK_PROBE=1 PC_PREFLIGHT=1 PF_TICKLE_INSTALL="$d/install" PF_PERF_WS="$d/perf/install" PF_VARIANT_pre="$pre"
        PF_SHAPE="tbf rate 1gbit burst 64kb latency 100ms" HEAD_SHA="$(git -C "$REPO" rev-parse --short=8 HEAD)"
        PRE_SHA="$PRE_SHA" REPS="$REPS" DUR="$DUR" QOS_ARGS="$QOS" ARMS="$ARMS" SUMMARY="$X/largemsg_rig_summary.py")
    # x86 has no exc_svc/exc_irq (Arm PMU exception counters): the rest of the rig's events.
    env "${common[@]}" PERF_EVENTS=task-clock,context-switches,cpu-migrations,instructions:u,instructions:k,cycles:u,cycles:k \
        PERF_ATTACH="$ATTACH" PUB_RATE=15 CELLS="Array4m:0" OUT="${OUT}_A" "$X/rmw_keepall_rig.sh" > "${OUT}_A.launch.log" 2>&1
    say "  phase A rmw_keepall_rig.sh rc=$? -> ${OUT}_A.txt"
    env "${common[@]}" PUB_RATE=30 CELLS="Array4m:0 Array1m:0" OUT="${OUT}_B" "$X/rmw_keepall_rig.sh" > "${OUT}_B.launch.log" 2>&1
    say "  phase B rmw_keepall_rig.sh rc=$? -> ${OUT}_B.txt; $(grep thp_fault_fallback' ' /proc/vmstat)"
    REALIZED=0
else
    say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null) ---"
    OUT=$HOME/rig_results_safe/pub_hugepage_rig_$TS
    common=(INSN_COUNT=0 KCOUNTERS=1 CLOCK_PROBE=1 HEAD_SHA="$HEAD_SHA" PRE_SHA="$PRE_SHA" REPS="${REPS:-5}" DUR=60
        QOS_ARGS="$QOS" ARMS="$ARMS" SUMMARY="$X/largemsg_rig_summary.py")
    env "${common[@]}" PERF_ATTACH=10_40 SKIP_BUILD=0 PUB_RATE=15 CELLS="Array4m:0" OUT="${OUT}_A" \
        "$X/rmw_keepall_rig.sh" > "${OUT}_A.launch.log" 2>&1
    say "  phase A rmw_keepall_rig.sh rc=$? -> ${OUT}_A.txt"
    # Phase B runs the binaries phase A built, under the same lock; each run's treatment is checked from its own log.
    env "${common[@]}" SKIP_BUILD=1 PUB_RATE=30 CELLS="Array4m:0 Array1m:0" OUT="${OUT}_B" \
        "$X/rmw_keepall_rig.sh" > "${OUT}_B.launch.log" 2>&1
    say "  phase B rmw_keepall_rig.sh rc=$? -> ${OUT}_B.txt"
    REALIZED=1
fi
python3 "$X/pub_hugepage_read.py" "${OUT}_A.runs" "${OUT}_B.runs" --realized "$REALIZED" > "$OUT.read.txt" 2>&1
read_rc=$?
say "  reading rc=$read_rc -> $OUT.read.txt"
[ "$read_rc" = 0 ] && rc=0
say "=== done ==="
