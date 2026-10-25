#!/usr/bin/env bash
# storm_tv_run.sh - runs storm_tv_ab.sh's two phases one after the other, detached-safe (2026-10-25, RESULTS `TVS`):
# PHASE=tv on every tree in TV_TREES, then PHASE=arms on ARMS_TREES. Each phase's own reading is in
# $BASE_OUT/<TAG>_tv.storm.txt and <TAG>_arms.storm.txt; this script's log ends with "=== storm_tv_run ended".
# Usage: TV_TREES="M=dir A=dir F=dir K=dir FK=dir" ARMS_TREES="M=dir A=dir F=dir" TAG=name [TV_REPS=30] [ARMS_REPS=10]
#        storm_tv_run.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TAG=${TAG:?TAG}
BASE_OUT=${BASE_OUT:-$HOME/storm_results}
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== storm_tv_run ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ==="
}
trap on_exit EXIT
echo "=== storm_tv_run $(date -Is) tag=$TAG load $(cut -d' ' -f1-3 /proc/loadavg) ==="
BASE_OUT=$BASE_OUT TAG="${TAG}_tv" TREES=${TV_TREES:?TV_TREES} PHASE=tv REPS=${TV_REPS:-30} "$HERE/storm_tv_ab.sh"
echo "tv phase done $(date -Is) load $(cut -d' ' -f1-3 /proc/loadavg)"
BASE_OUT=$BASE_OUT TAG="${TAG}_arms" TREES=${ARMS_TREES:?ARMS_TREES} PHASE=arms REPS=${ARMS_REPS:-10} "$HERE/storm_tv_ab.sh"
echo "arms phase done $(date -Is)"
