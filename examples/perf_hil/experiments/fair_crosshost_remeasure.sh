#!/usr/bin/env bash
# The cross-host tables (RESULTS section 2, rows from the `A` and `T` campaigns) again, without pinning (the user's
# rule, 2026-10-05; TESTING.md section 5).
#
# Every `A`/`T` row came from campaign_sweep.sh with each framework pinned to cores 1-3 on both Pis
# (PIN="taskset -c 1-3" in the three run_scenario.sh, then the default). Pinning is now opt-in, so this runs the same
# campaigns twice through the unchanged harness:
#   free   - no pinning: the OS schedules every framework, as anyone would run it. The headline.
#   pinned - PIN="taskset -c 1-3", the old method. Published only as a labelled note.
# Order: A free, A pinned, T pinned, T free - each kind of arm once early and once late, so a drift across the night
# lands on both.
#
# HOW TO READ IT, written before running: campaign_sweep.sh's own rules (instrument=ok, the boundary gate, VOID,
# win only outside the spread of the reps) decide every cell, unchanged. The free campaign's verdicts are the
# headline; for each cell the pinned median and the free/pinned ratio go in the note. A cell whose verdict differs
# between free and pinned is listed - that is the point of measuring it.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# The rig lock is taken once for the whole run, so a job queued meanwhile cannot cut in between campaigns.
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
SHA=${SHA:?set SHA to a pushed commit}
OUTB=${OUTB:-$HOME/rig_results_safe/fair_crosshost_${SHA}_$(date +%Y%m%d-%H%M%S)}
LOG="$OUTB.driver.log"
: >"$LOG"
say() { echo "$*" | tee -a "$LOG"; }
say "=== fair cross-host re-measure, $(date -Is), sha $SHA ==="
run() { # <name> <pin> <profile> [cells]
    local name=$1 pin=$2 profile=$3 cells=${4:-}
    say "--- $name (PIN='$pin', FASTDDS_PROFILE=$profile, CELLS='${cells:-all}') $(date +%T)"
    PIN="$pin" FASTDDS_PROFILE="$profile" CELLS="$cells" SHA="$SHA" OUT="$OUTB.$name.txt" \
        "$REPO/examples/perf_hil/experiments/campaign_sweep.sh" >"$OUTB.$name.log" 2>&1
    say "    exit $? ($(date +%T))"
}
run A_free "" fastdds_eth0_only.xml
run A_pinned "taskset -c 1-3" fastdds_eth0_only.xml
run T_pinned "taskset -c 1-3" fastdds_eth0_only_mms1472.xml "3 4"
run T_free "" fastdds_eth0_only_mms1472.xml "3 4"
say "=== ALL_DONE $(date -Is) ==="
