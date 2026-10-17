#!/usr/bin/env bash
# The mixed shared-memory + network delivery test (mixed_delivery.sh, RESULTS S15/S16) for all three frameworks at one
# commit, under one hold of the rig lock (2026-10-17, ROADMAP item 8: the CycloneDDS arm).
#
# Cells, in this order: CycloneDDS BEST_EFFORT and RELIABLE (the new arm), then TickLE and FastDDS at the same commit as
# the same-day control - nothing in the CycloneDDS change can touch their benches, so if their figures move against the
# published 7f127d56 ones (S15/S16) the rig or the build moved, and the CycloneDDS figures are read with that in mind.
# CLI_ARGS as the published runs: none for BEST_EFFORT, -Q (KEEP_ALL) for TickLE's RELIABLE and the same string for
# FastDDS's; CycloneDDS's RELIABLE bench is KEEP_ALL by default (4,000 samples, as FastDDS's) and gets none.
# REPS: 4 for CycloneDDS (four arms, each in each position once), 3 for the others (three arms, as published).
#
# Reading rules: mixed_delivery.sh's own, in its header and its analysis; each cell's verdict lines are at the end of
# its file. This driver adds one: a cell whose script did not print "=== done" is reported as not run, never as empty.
#
# Usage: SHA=<sha> mixed_delivery_campaign.sh     (detached: setsid nohup ... > <log> 2>&1 < /dev/null &)
# CELLS="cyclonedds:best_effort_throughput: ..." limits the run; RIG_LOCK_WAIT is rig_lock.sh's.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
SHA=${SHA:?set SHA to a commit the Pis can fetch}
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
CELLS=${CELLS:-"cyclonedds:best_effort_throughput: cyclonedds:reliable_throughput: tickle:best_effort_throughput: tickle:reliable_throughput:-Q fastdds:best_effort_throughput: fastdds:reliable_throughput:-Q"}
OUTB=${OUTB:-$HOME/rig_results_safe/mixed_${SHA}_$(date +%Y%m%d-%H%M%S)}
LOG="$OUTB.driver.log"
: >"$LOG"
say() { echo "$*" | tee -a "$LOG"; }
say "=== mixed delivery campaign $(date -Is) sha=$SHA cells: $CELLS ==="
for cell in $CELLS; do
    IFS=: read -r fw scen cli <<<"$cell"
    reps=3
    [ "$fw" = cyclonedds ] && reps=4
    out="$OUTB.${fw}_${scen}.txt"
    say "--- $fw $scen cli_args='$cli' reps=$reps -> $out ($(date +%T))"
    SHA=$SHA FRAMEWORK=$fw SCEN=$scen SIZE=p2 CLI_ARGS="$cli" REPS=$reps DUR=5 OUT="$out" \
        "$REPO/examples/perf_hil/experiments/mixed_delivery.sh" >"$out.log" 2>&1
    rc=$?
    if grep -q '^=== done' "$out" 2>/dev/null; then
        say "    exit $rc ($(date +%T))"
        grep -E '^(CORRECTNESS|DELIVERY|local:|remote:|  CONTROL)' "$out" | sed 's/^/    /' | tee -a "$LOG"
    else
        say "    NOT RUN to the end (exit $rc): $(tail -2 "$out.log" 2>/dev/null | tr '\n' ' ')"
    fi
done
say "=== ALL_DONE $(date -Is) ==="
