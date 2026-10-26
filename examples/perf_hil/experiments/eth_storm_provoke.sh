#!/usr/bin/env bash
# eth_storm_provoke.sh - can a ~1.5 ms repair timeout alone start the ETHERNET amp writer storm, and does a per-unit
# back-off stop it? (RESULTS `EWS`, 2026-10-26; a diagnostic beside eth_storm_census.sh, registered before its run.)
#
# The census's storm rep released 4.55 M units by the repair timeout, all at 1-2 ms; calm traced reps' srtt is
# 0.35-7.5 ms (ews1 round 1: min 345 us, q01 768 us, median 5.6 ms), so a 1-2 ms timeout is within what calm reps
# reach. Two experiment-only trees, copies of this checkout (trace hooks applied, eth_storm_provoke.py make):
#   P1 = the timeout pinned at ~1.5 ms wherever srtt is measured, no back-off;
#   P2 = P1 + a per-unit back-off (base << min(resends - 1, 6)).
# Each REPS (10) reps of the census's cell, both at once (wifi_burst_loss_netns.sh, same arguments as the census).
# READING: eth_storm_provoke.py read (its docstring is the rule set, in its code).
# Usage: [REPS=10] [OUT=~/ews_results/provoke] [TREES=~/ews_trees] eth_storm_provoke.sh
#   This script's log ends with "=== eth_storm_provoke ended".
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
REPS=${REPS:-10}
OUT=${OUT:-$HOME/ews_results/provoke}
TREES=${TREES:-$HOME/ews_trees}
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== eth_storm_provoke ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ==="
}
trap on_exit EXIT
mkdir -p "$OUT" "$TREES" || exit 1
echo "=== eth_storm_provoke $(date -Is) from $(git -C "$REPO" rev-parse --short HEAD) (src dirty" \
    "$(git -C "$REPO" status --porcelain -- src | wc -l)) REPS=$REPS load $(cut -d' ' -f1-3 /proc/loadavg) ==="
for t in P1 P2; do
    if [ -e "$TREES/$t" ]; then
        echo "REFUSED: $TREES/$t exists"
        exit 2
    fi
    mkdir -p "$TREES/$t" && cp -r "$REPO/src" "$REPO/include" "$REPO/examples" "$TREES/$t/" || exit 1
done
python3 -I "$HERE/eth_storm_provoke.py" make "$TREES/P1" 0 || exit 1
python3 -I "$HERE/eth_storm_provoke.py" make "$TREES/P2" 1 || exit 1
diff "$REPO/src/tickle.c" "$TREES/P2/src/tickle.c"
pids=()
for t in P1 P2; do
    env OUT="$OUT/$t" REPS="$REPS" DUR=30 PROFILE=ethernet ARMS="ge0.1b1" CELLS="rel_large" LARGE_HZ=30 BANDS=1 \
        NETEM_BASE="limit 100000 delay 100us rate 1gbit" NODE_ENV="TT_UDP_OFFLOAD=0" LARGE_RSTATS=1 LARGE_TRACE=1 \
        "$TREES/$t/examples/perf_hil/experiments/wifi_burst_loss_netns.sh" >"$OUT/$t.log" 2>&1 &
    pids+=($!)
done
for p in "${pids[@]}"; do wait "$p"; done
python3 -I "$HERE/eth_storm_provoke.py" read "$OUT" | tee "$OUT/read.txt"
