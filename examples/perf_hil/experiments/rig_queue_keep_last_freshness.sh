#!/usr/bin/env bash
# KEEP_LAST freshness on the rig (the user's decision D3, 2026-10-18; QoS redesign step 8): RESULTS rows 43/44 and the
# same-host KEEP_LAST 1 rows S1-S3 re-scored on freshness. Written 2026-10-18 before any of its runs; NOT launched by
# the session that wrote it. The reading is freshness_summary.py's (its docstring, R0-R4, is the rule; its selftest
# must pass before the lock is taken).
#
# STEPS, under one rig lock (hil), taken inside this detached process:
#   1. keep_last_freshness.sh, REPS=3, every cell: h43 h44 b8 b16 (cross-host history_depth_burst_loss, the clock
#      probe before and after every run, each age corrected at its own take time), s44 s16 (h44 and b16 on one host,
#      the fallback where the offset rule voids a cross-host cell), ctl (same host: the clock control) and fault (ctl
#      with a 5 ms sender skew: the control must fail it). All three frameworks.
#      RISK, before the run: U keeps half the probe's best round trip, ~103 us on the rig (5,453 earlier probes:
#      delay_min p50 205 us), and a cross-host run is VOID when U exceeds its age p50. TickLE's cross-host age at 20
#      samples/s is of the order of one transit, so h43/b8/b16 may VOID for TickLE; s44/s16 and ctl cannot.
#   2. fair_samehost_remeasure.sh, the headline (free) arm of S1-S3 only: best_effort_throughput p2/p3/p4, KEEP_LAST 1
#      on every framework (BE_HISTORY=1), 3 reps, with --fresh added to the window arguments every client and server
#      gets (WINDOW_ARGS). Goodput figures from this run are diagnostic: --fresh adds a CLOCK_REALTIME read per write
#      and per take on every framework, so they do not replace the published S1-S3.
#   3. freshness_summary.py over both: sample counts, clock, control and fault arm, then WIN / DRAW / LOSE per cell.
#
# The S1-S3 cells' senders have no schedule (max rate): they are scored on age alone (freshness_summary.py R3).
#
# ESTIMATE (PC dry run 2026-10-18: ~25 s per history run with probes; s6 2026-10-17: 5 reps of one cell, both arms,
# 9 min): build ~5 min + 66 history runs x 25 s (28 min) + 3 s6 cells x 3 reps (~18 min) + s6 builds (~5 min) =
# ~56 min; say 1 h 10, and analyse an overrun at 1 h 45.
#
# Usage, from a checkout of HEAD_SHA (it must contain this script, keep_last_freshness.sh, freshness_summary.py and
# BenchFreshness.h; pushed, since the Pis fetch it):
#   HEAD_SHA=<pushed sha> setsid nohup examples/perf_hil/experiments/rig_queue_keep_last_freshness.sh \
#     > ~/rig_results_safe/keep_last_freshness_queue.log 2>&1 < /dev/null &
#   RUNNER (default /home/semih/tickle_runner_klf) is the detached worktree it runs from, at HEAD_SHA.
# The last line is always "KLF_QUEUE rc=<n> ...".
set -u
HEAD_SHA=${HEAD_SHA:?set HEAD_SHA to the pushed commit to measure (it must contain this harness)}
RUNNER=${RUNNER:-/home/semih/tickle_runner_klf}
SAFE=$HOME/rig_results_safe
X=$RUNNER/examples/perf_hil/experiments
say() { echo "$(date -Is) $*"; }
mkdir -p "$SAFE"

if [ "${KLF_LOCKED:-0}" != 1 ]; then
    trap 'echo "KLF_QUEUE rc=$? (before the lock) $(date -Is)"' EXIT
    git -C /home/semih/tickle fetch -q origin || { say "REFUSED: git fetch failed"; exit 1; }
    git -C /home/semih/tickle merge-base --is-ancestor "$HEAD_SHA" origin/main ||
        { say "REFUSED: $HEAD_SHA is not on origin/main (the Pis fetch it)"; exit 1; }
    if [ ! -d "$RUNNER" ]; then
        git -C /home/semih/tickle worktree add -q --detach "$RUNNER" "$HEAD_SHA" || { say "REFUSED: worktree"; exit 1; }
    fi
    git -C "$RUNNER" checkout -q --detach "$HEAD_SHA" || { say "REFUSED: checkout"; exit 1; }
    for f in examples/perf_hil/experiments/keep_last_freshness.sh examples/perf_hil/experiments/freshness_summary.py \
        examples/perf_hil/experiments/fair_samehost_remeasure.sh examples/perf_hil/tickle/common/BenchFreshness.h; do
        [ -f "$RUNNER/$f" ] || { say "REFUSED: $HEAD_SHA has no $f"; exit 1; }
    done
    python3 "$X/freshness_summary.py" --selftest | tail -1 | grep -q '^SELFTEST PASS' ||
        { say "REFUSED: the reader's selftest does not pass"; exit 1; }
    say "=== KEEP_LAST freshness queue: head ${HEAD_SHA:0:8}, runner $RUNNER; ~1 h 10, overrun analysis at 1 h 45 ==="
    say "--- taking the rig lock (hil), once for every step ---"
    trap - EXIT
    export KLF_LOCKED=1 RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=36000 TMPDIR=/home/semih/tmp_build
    exec "$RUNNER/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi

# ---- under the lock ----------------------------------------------------------------------------------------------
cd "$RUNNER" || exit 1
STAMP=$(date +%Y%m%d-%H%M%S)
KOUT=$SAFE/keep_last_freshness_${HEAD_SHA:0:8}_$STAMP.txt
SOUTB=$SAFE/fair_samehost_fresh_${HEAD_SHA:0:8}_$STAMP
SUM=$SAFE/keep_last_freshness_${HEAD_SHA:0:8}_$STAMP.summary.txt
rc=0
# The only EXIT trap: the queue's last line, whatever ended it. Neither step shapes the link (keep_last_freshness.sh
# refuses a shaped one), so there is nothing to restore.
trap 'echo "KLF_QUEUE rc=$rc history=$KOUT samehost=$SOUTB.* summary=$SUM $(date -Is)"' EXIT
say "=== under the lock: head ${HEAD_SHA:0:8} ==="

say "--- 1. keep_last_freshness.sh -> $KOUT ---"
SHA=$HEAD_SHA REPS=3 OUT=$KOUT "$X/keep_last_freshness.sh" > "$KOUT.log" 2>&1
k=$?
[ "$k" = 0 ] || rc=1
say "    exit $k; $(grep -c '^FRESH run=[0-9]* cell=' "$KOUT" 2>/dev/null) runs"

say "--- 2. fair_samehost_remeasure.sh, S1-S3 free arm with --fresh -> $SOUTB.* ---"
SHA=$HEAD_SHA OUTB=$SOUTB ARMS=free REPS=3 BE_HISTORY=1 \
    CELLS="best_effort_throughput:p2 best_effort_throughput:p3 best_effort_throughput:p4" \
    WINDOW_ARGS="--warmup-s 2 --cooldown-s 2 --fresh" "$X/fair_samehost_remeasure.sh" > "$SOUTB.queue.log" 2>&1
k=$?
[ "$k" = 0 ] || rc=1
say "    exit $k"

say "--- 3. freshness_summary.py -> $SUM ---"
python3 "$X/freshness_summary.py" "$KOUT" --s6 "$SOUTB".best_effort_throughput_p*_free.txt > "$SUM" 2>&1
src=$?
[ "$src" = 0 ] || rc=1
say "    summary exit $src (0: session READ)"
tail -3 "$SUM"
