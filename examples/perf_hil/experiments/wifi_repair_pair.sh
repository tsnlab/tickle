#!/usr/bin/env bash
# wifi_repair_pair.sh - network profiles phase 3, Wi-Fi repair values (2026-10-23): the WB harness
# (wifi_burst_loss_netns.sh, rules as amended there) run for PROFILE=ethernet and PROFILE=wifi at once, from ONE build of
# this tree, so the two profiles share the PC's load and differ only in the profile each node is created with.
#
# Usage: TAG=name [REPS=3] [ARMS=...] [CELLS=...] [DUR=30] wifi_repair_pair.sh
#   Output: ~/rig_results_safe/wifi_repair/<TAG>_{ethernet,wifi}/ (the harness's own layout, summary.txt each) and
#   ~/rig_results_safe/wifi_repair/<TAG>.pair.txt, which ends with "=== wifi_repair_pair ended" whatever happened.
#   NODE_ENV defaults to TT_UDP_OFFLOAD=0 (every frame its own skb, as WW ran the large cell) and LARGE_RSTATS to 1.
#   Run it from a frozen copy of the tree (a detached git worktree): the harness builds from the tree it sits in.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG=${TAG:?TAG names the output}
BASE_OUT=${BASE_OUT:-$HOME/rig_results_safe/wifi_repair}
mkdir -p "$BASE_OUT" || exit 1
PAIR="$BASE_OUT/$TAG.pair.txt"
T0=$(date +%s)
pids=()
on_exit() {
    local rc=$?
    echo "=== wifi_repair_pair ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ===" >>"$PAIR"
}
trap on_exit EXIT
echo "=== wifi_repair_pair $(date -Is) tag=$TAG tree=$(git -C "$HERE" rev-parse --short HEAD) ===" >"$PAIR"
for profile in ethernet wifi; do
    PROFILE=$profile OUT="$BASE_OUT/${TAG}_$profile" NODE_ENV=${NODE_ENV:-TT_UDP_OFFLOAD=0} \
        LARGE_RSTATS=${LARGE_RSTATS:-1} "$HERE/wifi_burst_loss_netns.sh" >"$BASE_OUT/${TAG}_$profile.log" 2>&1 &
    pids+=($!)
    echo "started $profile pid=$! log=$BASE_OUT/${TAG}_$profile.log" >>"$PAIR"
done
for pid in "${pids[@]}"; do
    wait "$pid"
    echo "pid $pid exited $?" >>"$PAIR"
done
