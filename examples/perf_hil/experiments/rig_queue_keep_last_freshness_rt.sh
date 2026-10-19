#!/usr/bin/env bash
# KEEP_LAST freshness by ROUND TRIP on the rig (the user's decision D3, 2026-10-18; rule of 2026-10-19: across hosts
# only round trips are timed): RESULTS rows 43/44 (and bursts of 8 and 16) re-scored on freshness, all three
# frameworks, cross-host. Written 2026-10-19 before any of its runs; NOT launched by the session that wrote it. The
# reading is freshness_rt_summary.py's (--prereg prints it; its selftest must pass before the lock is taken).
#
# STEPS, under one rig lock (hil), taken inside this detached process and held once for every cell:
#   1. keep_last_freshness_rt.sh, REPS=3, cells h43 dly h44 b8 b16, all three frameworks (45 runs).
#   2. freshness_rt_summary.py over it: sample counts, echo accounting, the side-channel control, the fault arm
#      (dly: netem delay 1 ms on rpi#1's eth0, removed by the campaign's EXIT trap whatever ends it), then
#      WIN / LOSE / TIE per cell.
#
# ESTIMATE (PC dry run 2026-10-19: ~17 s per history run; keep_last_freshness.sh's rig estimate ~25 s per run with
# probes, ~20 s without): build ~5 min + 45 runs x ~20 s (15 min) + the leftover checks = ~22 min; say 30 min, and
# analyse an overrun at 45 min.
#
# Usage, from a checkout of HEAD_SHA (it must contain this script, keep_last_freshness_rt.sh, freshness_rt_summary.py
# and BenchFreshRt.h; pushed, since the Pis fetch it):
#   HEAD_SHA=<pushed sha> setsid nohup examples/perf_hil/experiments/rig_queue_keep_last_freshness_rt.sh \
#     > ~/rig_results_safe/keep_last_freshness_rt_queue.log 2>&1 < /dev/null &
#   RUNNER (default /home/semih/tickle_runner_klfrt) is the detached worktree it runs from, at HEAD_SHA.
# The last line is always "KLFRT_QUEUE rc=<n> ...".
set -u
HEAD_SHA=${HEAD_SHA:?set HEAD_SHA to the pushed commit to measure (it must contain this harness)}
RUNNER=${RUNNER:-/home/semih/tickle_runner_klfrt}
SAFE=$HOME/rig_results_safe
X=$RUNNER/examples/perf_hil/experiments
say() { echo "$(date -Is) $*"; }
mkdir -p "$SAFE"

if [ "${KLFRT_LOCKED:-0}" != 1 ]; then
    trap 'echo "KLFRT_QUEUE rc=$? (before the lock) $(date -Is)"' EXIT
    git -C /home/semih/tickle fetch -q origin || { say "REFUSED: git fetch failed"; exit 1; }
    git -C /home/semih/tickle merge-base --is-ancestor "$HEAD_SHA" origin/main ||
        { say "REFUSED: $HEAD_SHA is not on origin/main (the Pis fetch it)"; exit 1; }
    if [ ! -d "$RUNNER" ]; then
        git -C /home/semih/tickle worktree add -q --detach "$RUNNER" "$HEAD_SHA" || { say "REFUSED: worktree"; exit 1; }
    fi
    git -C "$RUNNER" checkout -q --detach "$HEAD_SHA" || { say "REFUSED: checkout"; exit 1; }
    for f in examples/perf_hil/experiments/keep_last_freshness_rt.sh examples/perf_hil/experiments/freshness_rt_summary.py \
        examples/perf_hil/tickle/common/BenchFreshRt.h; do
        [ -f "$RUNNER/$f" ] || { say "REFUSED: $HEAD_SHA has no $f"; exit 1; }
    done
    python3 "$X/freshness_rt_summary.py" --selftest | tail -1 | grep -q '^SELFTEST PASS' ||
        { say "REFUSED: the reader's selftest does not pass"; exit 1; }
    say "=== KEEP_LAST freshness (round trip) queue: head ${HEAD_SHA:0:8}, runner $RUNNER; ~30 min, overrun analysis at 45 min ==="
    say "--- taking the rig lock (hil), once for every cell ---"
    trap - EXIT
    export KLFRT_LOCKED=1 RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=36000 TMPDIR=/home/semih/tmp_build
    exec "$RUNNER/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi

# ---- under the lock ----------------------------------------------------------------------------------------------
cd "$RUNNER" || exit 1
STAMP=$(date +%Y%m%d-%H%M%S)
KOUT=$SAFE/keep_last_freshness_rt_${HEAD_SHA:0:8}_$STAMP.txt
SUM=$SAFE/keep_last_freshness_rt_${HEAD_SHA:0:8}_$STAMP.summary.txt
rc=0
# The only EXIT trap here: the queue's last line, whatever ended it. The campaign removes its own netem in its own trap.
trap 'echo "KLFRT_QUEUE rc=$rc results=$KOUT summary=$SUM $(date -Is)"' EXIT
say "=== under the lock: head ${HEAD_SHA:0:8} ==="

say "--- 1. keep_last_freshness_rt.sh -> $KOUT ---"
SHA=$HEAD_SHA REPS=3 OUT=$KOUT "$X/keep_last_freshness_rt.sh" >"$KOUT.log" 2>&1
k=$?
[ "$k" = 0 ] || rc=1
say "    exit $k; $(grep -c '^FRESH run=[0-9]* cell=' "$KOUT" 2>/dev/null) runs"

say "--- 2. freshness_rt_summary.py -> $SUM ---"
python3 "$X/freshness_rt_summary.py" "$KOUT" >"$SUM" 2>&1
src=$?
[ "$src" = 0 ] || rc=1
say "    summary exit $src (0: session READ)"
tail -3 "$SUM"
