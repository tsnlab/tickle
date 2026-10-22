#!/usr/bin/env bash
# before_after.sh - bundle 10 item 2 (2026-10-22): the bundle10_rss scenarios run against two source trees, the tables
# a context holds resident before and after the change.
#
# The harness is the one behind the decision, on the local branch exp/bundle10-rss (4f94a864: build_rmw.sh,
# build_core.sh, run.sh, read.py and the rest of its examples/perf_hil/experiments/bundle10_rss). It is not copied into
# this branch; this script takes it from HARNESS_REF into each tree that lacks it, so each tree is built with that
# harness's own scripts (rmw_tickle and core_host from the tree, the layouts from the tree's debug info), run.sh runs
# the same scenarios against each, one arm after the other, read.py reads each arm, and compare.py puts the two side by
# side. The reading rules are read.py's; the comparison is per table, resident bytes before -> after.
#
# Usage: BEFORE=<tree> AFTER=<tree> OUT=<dir> [HARNESS_REF=4f94a864] [SCENARIOS=...] [REPS=2] [DUR=10] before_after.sh
#   A tree is a plain source tree (git archive of a commit). OUT/<before|after>/ holds each arm's build (ws/,
#   core_host, layouts), results/ and report.txt; OUT/compare.md the comparison. Ends with "=== before_after ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
BEFORE=${BEFORE:?a source tree}
AFTER=${AFTER:?a source tree}
OUT=${OUT:?results directory}
HARNESS_REF=${HARNESS_REF:-4f94a864}
REPS=${REPS:-2}
DUR=${DUR:-10}
# shellcheck disable=SC2034 # read by the EXIT trap below
T0=$(date +%s)
trap 'echo "=== before_after ended rc=$? after $(($(date +%s) - T0)) s"' EXIT
EXP=examples/perf_hil/experiments/bundle10_rss
for arm in before after; do
    tree=$BEFORE
    [ "$arm" = after ] && tree=$AFTER
    dir=$OUT/$arm
    mkdir -p "$dir"
    echo "ARM $arm $(date +%T): tree $tree"
    if [ ! -f "$tree/$EXP/run.sh" ]; then
        git -C "$REPO" archive "$HARNESS_REF" "$EXP" | tar -x -C "$tree" || {
            echo "  cannot take the harness from $HARNESS_REF"
            exit 1
        }
    fi
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
python3 "$HERE/compare.py" "$OUT/before/report.txt" "$OUT/after/report.txt" >"$OUT/compare.md"
echo "compare: $OUT/compare.md"
