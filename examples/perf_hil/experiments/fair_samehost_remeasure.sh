#!/usr/bin/env bash
# The same-host table (RESULTS S1-S10) again, without pinning (the user's rule, 2026-10-05).
#
# Every S-cell came from s6_transport_cells.sh with the server on core 1 and the client on core 2 - one core each,
# which squeezes the multi-threaded DDS processes and suits single-threaded TickLE. This runs each cell twice through
# the same harness, unchanged otherwise:
#   free   - no pinning: the OS schedules every framework, as anyone would run it. The headline.
#   pinned - PIN_SERVER="taskset -c 1" PIN_CLIENT="taskset -c 2", the old method. Published only as a labelled note
#            ("with each process pinned off the interrupt core").
# The two arms of a cell run back to back, their order alternating cell by cell.
#
# HOW TO READ IT, written before running: s6_transport_cells.sh's own witness and drop-free rules decide which reps
# count, unchanged. Per cell and framework the free arm's median is the published figure; the pinned arm's median and
# the free/pinned ratio go in the note. A row's leader is decided on the free arm alone. Where the free arm reverses a
# lead the pinned arm showed, the row says so - that is the point of measuring it.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# The rig lock is taken once, here, for the whole run (2026-10-05): each s6_transport_cells.sh call used to take and
# release it, and a KEEP_ALL campaign queued meanwhile took it between two cells; the next cell then waited out
# rig_lock's 1800 s and was lost (exit 75). With the lock held, the inner calls see RIG_LOCK_HELD_HIL=1 and skip theirs.
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
SHA=${SHA:?set SHA to a pushed commit}
FRAMEWORKS=${FRAMEWORKS:-"tickle fastdds cyclonedds"}
REPS=${REPS:-3}
CELLS=${CELLS:-"best_effort_throughput:p2 best_effort_throughput:p3 best_effort_throughput:p4 reliable_throughput:p2 reliable_throughput:p3 reliable_throughput:p4 reliable_latency:p2 reliable_latency:p3 reliable_latency:p4"}
OUTB=${OUTB:-$HOME/rig_results_safe/fair_samehost_${SHA}_$(date +%Y%m%d-%H%M%S)}
LOG="$OUTB.driver.log"
: >"$LOG"
say() { echo "$*" | tee -a "$LOG"; }
say "=== fair same-host re-measure, $(date -Is), sha $SHA, frameworks $FRAMEWORKS, $REPS reps, cells: $CELLS ==="
n=0
for cell in $CELLS; do
    scen=${cell%%:*}; size=${cell#*:}
    dur=5; cli=""
    if [ "$scen" = reliable_latency ]; then dur=10; cli="-i 0.005"; fi # 200 pings/s, as S7/S9/S10
    order="free pinned"; [ $((n % 2)) = 1 ] && order="pinned free"
    for arm in $order; do
        ps_=""; pc_=""
        if [ "$arm" = pinned ]; then ps_="taskset -c 1"; pc_="taskset -c 2"; fi
        out="$OUTB.${scen}_${size}_${arm}.txt"
        say "--- $scen $size $arm -> $out ($(date +%T))"
        FRAMEWORKS="$FRAMEWORKS" SCEN=$scen SIZE=$size DUR=$dur REPS=$REPS SHA=$SHA CLI_ARGS="$cli" OUT="$out" \
            PIN_SERVER="$ps_" PIN_CLIENT="$pc_" "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" >"$out.log" 2>&1
        say "    exit $? ($(date +%T))"
    done
    n=$((n + 1))
done
say "=== ALL_DONE $(date -Is) ==="
