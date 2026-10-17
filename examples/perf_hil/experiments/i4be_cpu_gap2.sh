#!/usr/bin/env bash
# I4 best-effort CPU gap, second run (i4be_cpu_gap.sh's follow-up; written 2026-10-17 after its main phase, before this
# runs). The main phase found the subscriber's whole gap in kernel cycles at a LOWER kernel instruction count: 76 more
# syscalls a sample (an epoll_pwait2() before each recvmsg(), where Fast DDS blocks in recvfrom()), fewer IRQs than
# Fast DDS, and a kernel IPC of 1.0 against 1.5. The candidate is the in-place read: direct_recvmsg() has the kernel copy
# each ~64 KB run straight into the 4 MB sample - memory no core has touched lately - where Fast DDS's recvfrom() fills a
# buffer it reuses and copies out in user space.
#
# Arms (rmw_keepall_rig.sh, PERF_ATTACH "10 40" as before, unpinned): tickle@head, tickle@nodirect (tickle@head's binary
# with TT_RX_DIRECT=0 on both ends: every datagram read into the receive buffer, the run copied out by core), fastdds
# (the control nothing here touches; its shift from the main phase is the drift floor). 5 reps, 60 s, interleaved.
# Reading: i4be_perf_attach_read.py --nodirect, H4 in its header (VOID on an unrealized treatment).
# ESTIMATE: 15 runs x ~85 s = ~22 min (no build: SKIP_BUILD=1, the main phase built HEAD_SHA); analyse an overrun at 33.
# Usage: MAIN=<main phase OUT> HEAD_SHA=<sha> setsid nohup examples/perf_hil/experiments/i4be_cpu_gap2.sh \
#     > ~/rig_results_safe/i4be_cpu_gap2_queue.log 2>&1 < /dev/null &
set -u
HEAD_SHA=${HEAD_SHA:?set HEAD_SHA to a pushed commit}
MAIN=${MAIN:?set MAIN to the main phase OUT (its .runs is read for H4)}
X="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SAFE=$HOME/rig_results_safe
say() { echo "$(date -Is) $*"; }
if [ "${RIG_LOCK_HELD_HIL:-0}" != 1 ]; then
    say "=== I4 best-effort CPU gap, nodirect: head $HEAD_SHA; taking the rig lock (hil) ==="
    export RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000} TMPDIR=/home/semih/tmp_build
    exec "$X/../rig_lock.sh" "$0" "$@"
fi
trap 'echo "I4BE_CPU_GAP2 rc=$? $(date -Is)"' EXIT
say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null) ---"
TS=$(date +%Y%m%d-%H%M%S)
OUT=$SAFE/i4be_cpu_nodirect_$TS
env KCOUNTERS=1 CLOCK_PROBE=1 HEAD_SHA="$HEAD_SHA" PERF_ATTACH=10_40 SKIP_BUILD=1 REPS=5 DUR=60 PUB_RATE=15 \
    QOS_ARGS="--keep_last --history_depth 5" CELLS="Array4m:0" ARMS="tickle@head tickle@nodirect fastdds" \
    SUMMARY="$X/largemsg_rig_summary.py" OUT="$OUT" "$X/rmw_keepall_rig.sh" > "$OUT.launch.log" 2>&1
say "  nodirect rc=$? -> $OUT.txt"
python3 "$X/i4be_perf_attach_read.py" "$MAIN.runs" --nodirect "$OUT.runs" \
    --baseline "$SAFE/i4be_be_20261015-210806.runs" > "$OUT.read.txt" 2>&1
say "  reading rc=$? -> $OUT.read.txt"
say "=== done ==="
