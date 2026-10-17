#!/usr/bin/env bash
# Why rmw_tickle spends more CPU per MB than Fast DDS as shipped in the rmw large-message cell I4 best effort (Array4m,
# BEST_EFFORT KEEP_LAST 5, 15 Hz, cross-host; docs/RESULTS.md "rmw large messages", L4: subscriber +4%, publisher +3%).
# Written 2026-10-17 before any of its runs. Diagnostic only: no figure from here is a headline figure.
#
# What the existing runs already settle (i4be_cpu_split.py on i4be_be_20261015-210806, l4main2_i4be_20261015-094605,
# creation_latency_i4be_20261017-094944): the whole gap is system time on both sides (subscriber user time is LOWER
# for rmw_tickle), with the same datagrams, IP fragments and wakes per sample as Fast DDS and no page faults. What
# they cannot say is what the extra kernel time is: more syscalls, more interrupt and softirq time charged to our
# threads (the Pis' kernels account by tick, without IRQ_TIME_ACCOUNTING, so an interrupt that lands on a running
# thread is that thread's stime), or the same work at a lower IPC (a copy into cold memory).
#
# Phases, all under one rig lock (taken inside this detached process):
#   smoke  fastdds alone, 1 rep, 20 s, PERF_ATTACH "5 8": the attach path works on both Pis (every run has a .pstat
#          with every event counted). Otherwise nothing else runs.
#   main   tickle@head tickle@aa fastdds, 5 reps, 60 s, PERF_ATTACH "10 40" (rmw_keepall_rig.sh header): perf stat
#          --per-thread on each perf_test for publisher seconds 10..50, task-clock, context switches, migrations,
#          instructions and cycles :u/:k, exc_svc (one per syscall) and exc_irq (IRQs taken while the thread ran).
#          tickle@aa is tickle@head's binary under a second name: the A/A control the analysis cannot touch.
#   pin    tickle@head fastdds, 5 reps, 60 s, the same attach, PIN_CPUS=1-3 on both Pis (eth0's IRQ is on CPU 0 on
#          both): the causal test of interrupt charging. Diagnostic only (no testbed tuning for headline figures).
# Reading: i4be_perf_attach_read.py (its header holds the rules: VOID runs, the A/A floor, the instrument check
# against the uninstrumented L4 gap, and what would falsify each hypothesis).
#
# ESTIMATE: smoke 1 run ~45 s; main build check 2-5 min + 15 x ~85 s = ~26 min; pin 10 x ~85 s = ~15 min: ~45 min,
# analyse an overrun at 70 min. The last line is always "I4BE_CPU_GAP rc=<n> ...".
# Usage (HEAD_SHA pushed; the harness runs from this checkout, only HEAD_SHA is built on the Pis):
#   HEAD_SHA=4fca013b setsid nohup examples/perf_hil/experiments/i4be_cpu_gap.sh \
#     > ~/rig_results_safe/i4be_cpu_gap_queue.log 2>&1 < /dev/null &
set -u
HEAD_SHA=${HEAD_SHA:?set HEAD_SHA to a pushed commit}
X="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SAFE=$HOME/rig_results_safe
say() { echo "$(date -Is) $*"; }
if [ "${RIG_LOCK_HELD_HIL:-0}" != 1 ]; then
    say "=== I4 best-effort CPU gap: head $HEAD_SHA, harness $X; taking the rig lock (hil) ==="
    export RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000} TMPDIR=/home/semih/tmp_build
    exec "$X/../rig_lock.sh" "$0" "$@"
fi
trap 'echo "I4BE_CPU_GAP rc=$? $(date -Is)"' EXIT
say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null) ---"
TS=$(date +%Y%m%d-%H%M%S)
QOS="--keep_last --history_depth 5"
cell() { # tag reps dur arms skip_build [env words]
    local tag=$1 reps=$2 dur=$3 arms=$4 skip=$5 extra=${6:-}
    local out=$SAFE/i4be_cpu_${tag}_$TS
    # shellcheck disable=SC2086 # extra holds env words
    env KCOUNTERS=1 CLOCK_PROBE=1 HEAD_SHA="$HEAD_SHA" $extra SKIP_BUILD="$skip" REPS="$reps" DUR="$dur" PUB_RATE=15 \
        QOS_ARGS="$QOS" CELLS="Array4m:0" ARMS="$arms" SUMMARY="$X/largemsg_rig_summary.py" OUT="$out" \
        "$X/rmw_keepall_rig.sh" > "$out.launch.log" 2>&1
    say "  $tag rc=$? -> $out.txt" >&2
    echo "$out"
}
SMOKE=$(cell smoke 1 20 "fastdds" 1 "PERF_ATTACH=5_8" | tail -1)
n_runs=$(grep -c '^RUN ' "$SMOKE.runs/index.txt" 2>/dev/null)
n_ok=0
for f in "$SMOKE.runs"/*.pstat; do
    [ -s "$f" ] || continue
    grep -q 'not supported' "$f" && continue # "<not counted>" is a thread that did not run in the window: zero
    grep -qE ',[0-9]+,,exc_irq,' "$f" && grep -qE ',[0-9]+,,cycles:k,' "$f" && n_ok=$((n_ok + 1))
done
if [ -z "$n_runs" ] || [ "$n_runs" = 0 ] || [ "$n_ok" != $((2 * n_runs)) ]; then
    say "STOPPED: smoke has $n_ok counted .pstat files for ${n_runs:-0} runs (want 2 a run): the attach path does not work"
    exit 1
fi
say "  smoke: $n_ok of $((2 * n_runs)) .pstat files counted every event"
MAIN=$(cell main 5 60 "tickle@head tickle@aa fastdds" 0 "PERF_ATTACH=10_40" | tail -1)
PIN=$(cell pin 5 60 "tickle@head fastdds" 1 "PERF_ATTACH=10_40 PIN_CPUS=1-3" | tail -1)
python3 "$X/i4be_perf_attach_read.py" "$MAIN.runs" --pin "$PIN.runs" \
    --baseline "$SAFE/i4be_be_20261015-210806.runs" > "$SAFE/i4be_cpu_$TS.read.txt" 2>&1
say "  reading rc=$? -> $SAFE/i4be_cpu_$TS.read.txt"
say "=== done ==="
