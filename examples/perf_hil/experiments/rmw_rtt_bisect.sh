#!/usr/bin/env bash
# rmw_rtt_bisect.sh - rmw_tickle's same-host RTT, CPU and page faults per round trip across several commits, on this
# PC, each run in its own private netns with its own /dev/shm (2026-10-09). The question it was written for: did
# R2/R9 (bench RELIABLE block: RTT 36.9 -> 40.0 us, CPU 63.3 -> 68.1 us per round trip on the rig) regress between
# a7e02807 and 06dd78d8, and if so which commit - in particular, does 718220d8 (zero_allocate instead of memset for
# the 2.2 MB context) move page faults into the measured window?
#
# Each run is rmw_samehost_cell.sh exactly as the rig's harness runs it (pong, 4 s, ping -d DUR -i 0.001 --wait
# block, --stamps), whose sampler records each process's CPU and (since this change) its minor faults and context
# switches every 0.1 s. Arms are rmw_bisect_build.sh builds named on the command line; the FIRST arm is run twice per
# repetition (as "<sha>" and "<sha>_ctl"), the control: the same build against itself, which no commit can touch.
# Arm order is rotated every repetition and reversed every other one, so a drift of the PC lands on every arm alike.
#
# The reading rules are in rmw_rtt_bisect.py (written before the run, enforced there, not here).
# Usage: rmw_rtt_bisect.sh <out dir> <reps> <sha>...   env: DUR (20), CELLS ("bench:reliable bench:best_effort"),
#        BISECT_BUILDS (~/rmw_bisect_builds). Detach it. The log ends "=== rmw_rtt_bisect ended rc=<n>".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CELL="$HERE/rmw_samehost_cell.sh"
OUT=${1:?usage: rmw_rtt_bisect.sh <out dir> <reps> <sha>...}
REPS=${2:?}
shift 2
[ $# -ge 2 ] || { echo "need at least two arms"; exit 2; }
DUR=${DUR:-20}
CELLS=${CELLS:-"bench:reliable bench:best_effort"}
BISECT_BUILDS=${BISECT_BUILDS:-$HOME/rmw_bisect_builds}
rc=1
NS=rttbisect_$$
trap 'sudo -n ip netns del "$NS" 2>/dev/null; echo "=== rmw_rtt_bisect ended rc=$rc $(date -Is)"' EXIT
mkdir -p "$OUT" || exit 1
arms=()
for s in "$@"; do
    d=$(ls -d "$BISECT_BUILDS/$s"* 2>/dev/null | head -1)
    [ -n "$d" ] && [ -f "$d/BUILD_OK" ] || { echo "REFUSED: no complete build of $s in $BISECT_BUILDS"; rc=2; exit; }
    arms+=("$s")
done
runarms=("${arms[0]}_ctl" "${arms[@]}")
echo "rmw_rtt_bisect $(date -Is) reps=$REPS dur=$DUR cells='$CELLS' arms=${runarms[*]} host=$(hostname)" | tee "$OUT/params.txt"
n=${#runarms[@]}
for rep in $(seq 1 "$REPS"); do
    order=()
    for i in $(seq 0 $((n - 1))); do order+=("${runarms[$(((i + rep) % n))]}"); done
    if [ $((rep % 2)) = 0 ]; then
        rev=()
        for ((i = n - 1; i >= 0; i--)); do rev+=("${order[$i]}"); done
        order=("${rev[@]}")
    fi
    echo "--- rep $rep $(date -Is) order ${order[*]}"
    for c in $CELLS; do
        IFS=: read -r msg qos <<<"$c"
        for a in "${order[@]}"; do
            sha=${a%_ctl}
            d=$(ls -d "$BISECT_BUILDS/$sha"* | head -1)
            dir="$OUT/rtt_${msg}_${qos}_block_${a}_r$rep"
            sudo -n ip netns add "$NS" || { echo "cannot create netns"; rc=3; exit; }
            sudo -n ip -n "$NS" link set lo up multicast on
            sudo -n ip -n "$NS" route add default dev lo
            sudo -n ip netns exec "$NS" env -i PATH="$PATH" HOME="$HOME" ARM=tickle KIND=rtt MSG="$msg" QOS="$qos" \
                WAIT=block DUR="$DUR" RTT_I=0.001 POLL_ARGS="" CELL_DIR="$dir" DOMAIN=83 \
                ROS_SETUP=/opt/ros/lyrical/setup.bash OVERLAYS="$d/install/local_setup.bash" \
                TICKLE_BCAST=127.255.255.255 PINGPONG="$d/install/rmw_perf_pingpong/lib/rmw_perf_pingpong" \
                OWNER="$(id -u):$(id -g)" \
                bash -c 'mount -t tmpfs -o size=512m tmpfs /dev/shm && bash "$0"; rc=$?; chown -R "$OWNER" "$CELL_DIR"; exit $rc' "$CELL"
            echo "$(date +%T) $msg $qos $a r$rep: cell exit $?"
            sudo -n ip netns del "$NS"
        done
    done
done
python3 "$HERE/rmw_rtt_bisect.py" "$OUT" | tee "$OUT/summary.txt"
rc=${PIPESTATUS[0]}
