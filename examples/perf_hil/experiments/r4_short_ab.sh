#!/usr/bin/env bash
# r4_short_ab.sh - does the stall back-off (S) cut short R4's ge10b4 rel_large reps? (RESULTS `EWB`.) In ess1, one
# S rep of 8 delivered 129 of 150 after "Reliable gap too large to track"; no rep of G ever had. Written and committed
# before its first run; the reading is r4_short_read.py (its docstring is the rule set, applied in its code).
#
# TREES (made by eth_storm_stall_ab.sh prepare, reused untouched): M = G (45129b71), A = G again, S = the stall
#   back-off (646d27d4's pre-rebase build). The three run at once rep by rep (wifi_repair_ab.sh), so they share load.
# CELLS: PROFILE=wifi ARMS="ctl ge10b4" CELLS=rel_large; ctl is the control no repair can touch.
# SAMPLE SIZE (before the run): at a true short rate of 1/8, (1 - 1/8)^n is the chance of seeing none: n = 24 -> 4%,
#   n = 32 -> 1.4%. With 32 reps a tree, S at 4 of 32 against G + A at 0 of 64 is one-sided Fisher p 0.011 (WORSE);
#   2 of 32 against 0 of 64 is p 0.11 (not decided). REPS = 32: ~50 min (two arms x 32 reps x ~45 s).
# Usage: [TAG=r4s1] [OUT=~/ews_stall_results] [REPS=32] [TREES_DIR=~/ews_stall_trees] r4_short_ab.sh
#   The log ends with "=== r4_short_ab ended"; the reading is $OUT/$TAG.read.txt; STATUS.txt gets a line a phase.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TAG=${TAG:-r4s1}
OUT=${OUT:-$HOME/ews_stall_results}
REPS=${REPS:-32}
TREES_DIR=${TREES_DIR:-$HOME/ews_stall_trees}
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== r4_short_ab ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ==="
}
trap on_exit EXIT
status() { echo "$(date '+%H:%M') r4s: $*" >>"$OUT/STATUS.txt"; echo "$(date -Is) $*"; }
for t in M A S; do
    [ -f "$TREES_DIR/$t/PROVENANCE.txt" ] || { echo "REFUSED: $TREES_DIR/$t not prepared"; exit 2; }
    cat "$TREES_DIR/$t/PROVENANCE.txt"
done
status "run $TAG start (load $(cut -d' ' -f1-3 /proc/loadavg)), REPS=$REPS, wifi ctl + ge10b4 rel_large, M A S at once"
TREES="M=$TREES_DIR/M A=$TREES_DIR/A S=$TREES_DIR/S" TAG="$TAG" BASE_OUT="$OUT" PROFILE=wifi REPS="$REPS" \
    ARMS="ctl ge10b4" CELLS=rel_large LARGE_RSTATS=1 NODE_ENV=TT_UDP_OFFLOAD=0 "$HERE/wifi_repair_ab.sh" >/dev/null 2>&1
status "reps done (load $(cut -d' ' -f1-3 /proc/loadavg))"
python3 -I "$HERE/r4_short_read.py" "$OUT" "$TAG" >"$OUT/$TAG.read.txt" 2>&1
status "read rc=$? -> $OUT/$TAG.read.txt: $(grep -E '^SHORT VERDICT' "$OUT/$TAG.read.txt")"
grep -E '^(CONTROL|SHORT|VOID)' "$OUT/$TAG.read.txt"
