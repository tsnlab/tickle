#!/usr/bin/env bash
# Creation-time latency re-measure on the rig (the user's decision of 2026-10-17: latency = the receiver's arrival -
# the data's creation time, the scheduled publish time start + k / rate in a fixed-rate cell; the send-stamp latency
# stays as the secondary, diagnostic figure). Written 2026-10-17 before any of its runs; NOT run by the session that
# wrote it. The readings are creation_latency_summary.py's (its docstring, R0-R5, is the rule; its selftest must pass).
#
# CELLS (each rmw cell as in docs/RESULTS.md "rmw large messages", L4; CLOCK_PROBE=1 KCOUNTERS=1 CREATION_TRACE=1):
#   ctl   the ping-pong control (R4), FIRST: Array1k RELIABLE KEEP_LAST 10, 100 Hz, perf_test --roundtrip_mode
#         Main/Relay, 30 s, 3 reps, tickle@head fastdds cyclonedds. Its verdict gates every rmw creation figure.
#   i4s   Array4m BEST_EFFORT KEEP_LAST 5, 30 Hz (link-saturating), 60 s, 5 reps, tickle@head tickle@block fastdds
#         fastdds@mms1472 cyclonedds (tickle@block: RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS=1000, off by default)
#   i4be  Array4m BEST_EFFORT KEEP_LAST 5, 15 Hz, 60 s, 5 reps, tickle@head fastdds fastdds@mms1472 cyclonedds
#   i4rel Array4m RELIABLE KEEP_LAST 10, 15 Hz, same
#   core  campaign_sweep.sh's latency cells, the ones RESULTS rows 1-6b (and 24-25) are read from: c10 L p1, c11 L p2,
#         c12 L p1 +10 ms netem, c16 L p3, c17 L p4; all three frameworks, 3 reps (reliable_latency clients print
#         crt_* - the round trip from each ping's due time - beside rtt_*; read by creation_latency_summary.py --rtt,
#         whose R4 is the core control: a ping-pong has nothing waiting at the sender).
#
# perf_test. The creation trace lives in perf_test (perf_test_creation_trace.py): this queue applies it on both Pis'
# ~/rmw_perf_ws/src/performance_test/performance_test before the first build (the first rmw_keepall_rig.sh invocation
# rebuilds performance_test; the later ones run SKIP_BUILD=1) and refuses to measure if it does not apply cleanly. The
# EXIT trap (the only one) reverts the source on both Pis. The installed perf_test then still carries the trace,
# inert without PERF_TEST_CREATION_TRACE (it writes nothing and the send loop is unchanged), until the next
# rmw_keepall_rig.sh build rebuilds it from the reverted source.
#
# ESTIMATE (2026-10-15 L4: a 60 s run ~85 s with probes; a 30 s run ~45 s; a full Pi build with performance_test
# recompiled ~15 min; a core latency run ~50 s): build 15 + ctl 9 x 45 s (7) + i4s 25 x 85 s (35) + i4be and i4rel
# 2 x 20 x 85 s (57) + core 45 runs x 50 s + its builds (46) = ~2 h 40 min; say 3 h, and analyse an overrun at 4 h 30.
#
# Usage, from a checkout of HEAD_SHA (it must contain this script, perf_test_creation_trace.py and CreationTime.h;
# pushed, since the Pis fetch it):
#   HEAD_SHA=<pushed sha> setsid nohup examples/perf_hil/experiments/rig_queue_creation_latency.sh \
#     > ~/rig_results_safe/creation_latency_queue.log 2>&1 < /dev/null &
#   RUNNER (default /home/semih/tickle_runner_ctl) is the detached worktree it runs from, at HEAD_SHA.
#   PREFLIGHT=0 skips the PC dry run (creation_latency_pc.sh, ~25 min, before the lock); PREFLIGHT_ONLY=1 stops after it.
# The last line is always "CREATION_LATENCY_QUEUE rc=<n> ...".
set -u
HEAD_SHA=${HEAD_SHA:?set HEAD_SHA to the pushed commit to measure (it must contain this harness)}
RUNNER=${RUNNER:-/home/semih/tickle_runner_ctl}
SAFE=$HOME/rig_results_safe
TAG=creation_latency
X=$RUNNER/examples/perf_hil/experiments
say() { echo "$(date -Is) $*"; }
mkdir -p "$SAFE"

if [ "${CTL_LOCKED:-0}" != 1 ]; then
    trap 'echo "CREATION_LATENCY_QUEUE rc=$? (before the lock) $(date -Is)"' EXIT
    git -C /home/semih/tickle fetch -q origin || { say "REFUSED: git fetch failed"; exit 1; }
    git -C /home/semih/tickle merge-base --is-ancestor "$HEAD_SHA" origin/main ||
        { say "REFUSED: $HEAD_SHA is not on origin/main (the Pis fetch it)"; exit 1; }
    if [ ! -d "$RUNNER" ]; then
        git -C /home/semih/tickle worktree add -q --detach "$RUNNER" "$HEAD_SHA" || { say "REFUSED: worktree"; exit 1; }
    fi
    git -C "$RUNNER" checkout -q --detach "$HEAD_SHA" || { say "REFUSED: checkout"; exit 1; }
    for f in examples/perf_hil/experiments/perf_test_creation_trace.py examples/perf_hil/experiments/creation_latency_summary.py \
        examples/perf_hil/tickle/common/CreationTime.h; do
        [ -f "$RUNNER/$f" ] || { say "REFUSED: $HEAD_SHA has no $f"; exit 1; }
    done
    python3 "$X/creation_latency_summary.py" --selftest | tail -1 | grep -qx 'selftest: PASS' ||
        { say "REFUSED: the reader's selftest does not pass"; exit 1; }
    say "=== creation-time latency queue: head ${HEAD_SHA:0:8}, runner $RUNNER ==="
    if [ "${PREFLIGHT:-1}" = 1 ]; then
        PFD=$SAFE/preflight/${TAG}_$(date +%Y%m%d-%H%M%S)
        say "--- PC dry run (creation_latency_pc.sh) -> $PFD ---"
        OUT=$PFD "$X/creation_latency_pc.sh" 2 30 > "$PFD.log" 2>&1
        grep -q '^PC_CHECK PASS' "$PFD.log" || { say "PREFLIGHT FAILED: $PFD.log"; exit 1; }
        say "--- PC dry run PASS ---"
    fi
    if [ "${PREFLIGHT_ONLY:-0}" = 1 ]; then say "=== PREFLIGHT_ONLY: stopping before the rig ==="; exit 0; fi
    say "--- taking the rig lock (hil), once for every cell ---"
    trap - EXIT
    export CTL_LOCKED=1 RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=36000 TMPDIR=/home/semih/tmp_build
    exec "$RUNNER/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi

# ---- under the lock ----------------------------------------------------------------------------------------------
cd "$RUNNER" || exit 1
K=$HOME/.ssh/tickle_ci_ed25519
PIS="10.1.1.214 10.1.1.213"
PT_PKG=/home/ci/rmw_perf_ws/src/performance_test/performance_test
pi() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "$2" < /dev/null; }
APPLIED=""
finish() { # the one EXIT trap: revert the perf_test source on every Pi it was applied to, then the last line
    local rc=$? h
    for h in $APPLIED; do
        say "revert on $h: $(pi "$h" "python3 /tmp/ctl_ptc.py revert $PT_PKG" 2>&1 | tr '\n' ' ')"
    done
    echo "CREATION_LATENCY_QUEUE rc=$rc $(date -Is)"
}
trap finish EXIT
say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null) ---"
for h in $PIS; do
    scp -q -i "$K" -o BatchMode=yes "$X/perf_test_creation_trace.py" "ci@$h:/tmp/ctl_ptc.py" ||
        { say "REFUSED: cannot copy the trace applier to $h"; exit 1; }
    out=$(pi "$h" "python3 /tmp/ctl_ptc.py apply $PT_PKG && python3 /tmp/ctl_ptc.py check $PT_PKG" 2>&1)
    rc=$?
    say "trace on $h: $(echo "$out" | tr '\n' ' ')"
    [ "$rc" = 0 ] || { say "REFUSED: the creation trace does not apply on $h (nothing measured)"; exit 1; }
    APPLIED="$APPLIED $h"
done

TS=$(date +%Y%m%d-%H%M%S)
COMMON="CLOCK_PROBE=1 KCOUNTERS=1 CREATION_TRACE=1 HEAD_SHA=$HEAD_SHA"
rmw_cell() { # tag skip_build reps dur rate qos cells arms [extra env words]
    local tag=$1 skip=$2 reps=$3 dur=$4 rate=$5 qos=$6 cells=$7 arms=$8 extra=${9:-}
    local out=$SAFE/${TAG}_${tag}_$TS
    # shellcheck disable=SC2086 # COMMON and extra are env words
    env $COMMON $extra SKIP_BUILD="$skip" REPS="$reps" DUR="$dur" PUB_RATE="$rate" QOS_ARGS="$qos" CELLS="$cells" \
        ARMS="$arms" SUMMARY="$X/largemsg_rig_summary.py" OUT="$out" "$X/rmw_keepall_rig.sh" > "$out.launch.log" 2>&1
    say "  $tag rc=$? -> $out.txt" >&2
    echo "$out"
}
CTL=$(rmw_cell ctl 0 3 30 100 "--reliable --keep_last --history_depth 10" "Array1k:0" "tickle@head fastdds cyclonedds" \
    ROUNDTRIP=1 | tail -1)
# The treatment must be on the rig's perf_test before two hours are spent: every control run has a publisher trace.
n_runs=$(grep -c '^RUN ' "$CTL.runs/index.txt" 2>/dev/null)
n_trace=$(grep -l '^# ctl creation trace' "$CTL.runs"/*_pub.ctrace 2>/dev/null | wc -l)
if [ -z "$n_runs" ] || [ "$n_runs" = 0 ] || [ "$n_trace" != "$n_runs" ]; then
    say "STOPPED: $n_trace of ${n_runs:-0} control runs have a creation trace - the rig's perf_test does not carry it"
    exit 1
fi
python3 "$X/creation_latency_summary.py" "$CTL.runs" 30 > "$CTL.creation.txt" 2>&1
say "  control: $(grep -E '^CONTROL ' "$CTL.creation.txt" | tail -1) -> $CTL.creation.txt"
I4S=$(rmw_cell i4s 1 5 60 30 "--keep_last --history_depth 5" "Array4m:0" \
    "tickle@head tickle@block fastdds fastdds@mms1472 cyclonedds" | tail -1)
I4BE=$(rmw_cell i4be 1 5 60 15 "--keep_last --history_depth 5" "Array4m:0" \
    "tickle@head fastdds fastdds@mms1472 cyclonedds" | tail -1)
I4REL=$(rmw_cell i4rel 1 5 60 15 "--reliable --keep_last --history_depth 10" "Array4m:0" \
    "tickle@head fastdds fastdds@mms1472 cyclonedds" | tail -1)
for d in "$I4S" "$I4BE" "$I4REL"; do
    python3 "$X/creation_latency_summary.py" "$d.runs" 60 --control "$CTL.runs" > "$d.creation.txt" 2>&1
    say "  creation reading -> $d.creation.txt"
done
CORE=$SAFE/${TAG}_core_$TS.txt
SHA=$(git -C "$RUNNER" rev-parse HEAD) CELLS="10 11 12 16 17" REPS=3 PREFLIGHT=0 OUT="$CORE" \
    "$X/campaign_sweep.sh" > "$CORE.launch.log" 2>&1
say "  core rc=$? -> $CORE"
python3 "$X/creation_latency_summary.py" --rtt "$CORE" > "$CORE.creation.txt" 2>&1
say "  core: $(tail -1 "$CORE.creation.txt") -> $CORE.creation.txt"
say "=== creation-time latency queue done ==="
