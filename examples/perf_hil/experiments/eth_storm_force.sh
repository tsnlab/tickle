#!/usr/bin/env bash
# eth_storm_force.sh - the ETHERNET amp writer storm with its trigger forced: a whole sample lost every 3 s, on main,
# main + a per-unit back-off, a ~1.5 ms pinned timeout, and that + the back-off (RESULTS `EWS`, 2026-10-26; registered
# before its run - why, the arms and the rules: eth_storm_force.py's docstring, applied in its code).
# Four experiment-only trees, copies of this checkout (trace hooks applied), made by eth_storm_force.py make:
#   FM = PIN 0 BACKOFF 0, FB = PIN 0 BACKOFF 1, FP = PIN 1 BACKOFF 0, FPB = PIN 1 BACKOFF 1, all DROP=FORCE_EVERY.
# Each REPS (20) reps of the census's cell, all four at once (wifi_burst_loss_netns.sh, the census's arguments).
# SAMPLE SIZE (before the run): in provoke/P1 the one storm came from the one whole-sample burst of ~3 such reps; with
#   ~8 forced bursts a rep, FP should storm in most reps if a burst at ~1.5 ms starts the loop a third of the time.
#   FPB 0 of 20 against FP >= 5 of 20 gives one-sided Fisher p <= 0.024.
# Usage: [REPS=20] [FORCE_EVERY=90] [OUT=~/ews_results/force1] [TREES=~/ews_trees_force] eth_storm_force.sh
#   This script's log ends with "=== eth_storm_force ended".
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
REPS=${REPS:-20}
FORCE_EVERY=${FORCE_EVERY:-90}
OUT=${OUT:-$HOME/ews_results/force1}
TREES=${TREES:-$HOME/ews_trees_force}
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== eth_storm_force ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ==="
}
trap on_exit EXIT
mkdir -p "$OUT" "$TREES" || exit 1
echo "=== eth_storm_force $(date -Is) from $(git -C "$REPO" rev-parse --short HEAD) (src dirty" \
    "$(git -C "$REPO" status --porcelain -- src | wc -l)) REPS=$REPS FORCE_EVERY=$FORCE_EVERY" \
    "load $(cut -d' ' -f1-3 /proc/loadavg) ==="
declare -A PIN=([FM]=0 [FB]=0 [FP]=1 [FPB]=1) BACKOFF=([FM]=0 [FB]=1 [FP]=0 [FPB]=1)
for t in FM FB FP FPB; do
    if [ -e "$TREES/$t" ]; then
        echo "REFUSED: $TREES/$t exists"
        exit 2
    fi
    mkdir -p "$TREES/$t" && cp -r "$REPO/src" "$REPO/include" "$REPO/examples" "$TREES/$t/" || exit 1
    python3 -I "$HERE/eth_storm_force.py" make "$TREES/$t" "${PIN[$t]}" "${BACKOFF[$t]}" "$FORCE_EVERY" || exit 1
    echo "--- $t: diff against this checkout"
    diff "$REPO/src/tickle.c" "$TREES/$t/src/tickle.c" | grep -c '^>'
done
pids=()
for t in FM FB FP FPB; do
    env OUT="$OUT/$t" REPS="$REPS" DUR=30 PROFILE=ethernet ARMS="ge0.1b1" CELLS="rel_large" LARGE_HZ=30 BANDS=1 \
        NETEM_BASE="limit 100000 delay 100us rate 1gbit" NODE_ENV="TT_UDP_OFFLOAD=0" LARGE_RSTATS=1 LARGE_TRACE=1 \
        "$TREES/$t/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" >"$OUT/$t.log" 2>&1 &
    pids+=($!)
done
for p in "${pids[@]}"; do wait "$p"; done
python3 -I "$HERE/eth_storm_force.py" read "$OUT" | tee "$OUT/read.txt"
