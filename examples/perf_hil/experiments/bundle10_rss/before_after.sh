#!/usr/bin/env bash
# before_after.sh - bundle 10 item 2 (2026-10-22): the bundle10_rss scenarios run against two source trees, the tables
# a context resident before and after the change. Each tree is built with its own copy of this directory's
# build_rmw.sh and build_core.sh (rmw_tickle and core_host from that tree, the layouts from its own debug info), then
# run.sh runs the same scenarios against each, one arm after the other, and read.py reads each arm.
#
# The harness is the exp/bundle10-rss branch's (2026-10-22, the measurement behind the decision); this script only
# drives it twice. Its reading rules are read.py's, and the comparison is per table: resident bytes before -> after.
#
# Usage: BEFORE=<tree> AFTER=<tree> OUT=<dir> [SCENARIOS=...] [REPS=2] [DUR=10] before_after.sh
#   Each tree must hold this directory (copy it into a tree that predates it). OUT/<before|after>/ holds each arm's
#   build (ws/, core_host, layouts), results/ and report.txt. Ends with "=== before_after ended".
set -uo pipefail
BEFORE=${BEFORE:?a source tree}
AFTER=${AFTER:?a source tree}
OUT=${OUT:?results directory}
REPS=${REPS:-2}
DUR=${DUR:-10}
T0=$(date +%s)
trap 'echo "=== before_after ended rc=$? after $(($(date +%s) - T0)) s"' EXIT
EXP=examples/perf_hil/experiments/bundle10_rss
for arm in before after; do
    tree=$BEFORE
    [ "$arm" = after ] && tree=$AFTER
    dir=$OUT/$arm
    mkdir -p "$dir"
    echo "ARM $arm $(date +%T): tree $tree"
    WS=$dir/ws "$tree/$EXP/build_rmw.sh" >"$dir/build_rmw.log" 2>&1 || {
        echo "  build_rmw failed: $dir/build_rmw.log"
        exit 1
    }
    OUT=$dir WS=$dir/ws "$tree/$EXP/build_core.sh" >"$dir/build_core.log" 2>&1 || {
        echo "  build_core failed: $dir/build_core.log"
        exit 1
    }
    B=$dir OUT=$dir/results REPS=$REPS DUR=$DUR ${SCENARIOS:+SCENARIOS="$SCENARIOS"} "$tree/$EXP/run.sh" \
        >"$dir/run.log" 2>&1
    python3 "$tree/$EXP/read.py" "$dir/results" >"$dir/report.txt" 2>&1
    echo "  done $(date +%T): $dir/report.txt"
done
