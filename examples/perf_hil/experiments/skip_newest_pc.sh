#!/usr/bin/env bash
# skip_newest_pc.sh - the segment drain's skip-to-newest against main and against drain-one-sample, on THIS PC, in a
# private network namespace. Not a published figure (the PC is not the rig); what it decides is which way the arms
# move against each other on the same host in the same session. 2026-10-07.
#
# WHY. rmw_tickle same-host BEST_EFFORT KEEP_LAST 1 at -r 0: main decodes and delivers every record into the rmw
# queue, which overwrites all but about one per take (61k/s taken of 1.48M/s written, the CPU spent on samples nobody
# sees). origin/ab/drain-one-sample (132cd136) stops the drain at each delivered sample: 367k/s on the PC, but it
# reads the ring in order, so the application sees samples ~1.4 ms old instead of the newest. Skip-to-newest passes
# over a sample when the KEEP_LAST reader already has its depth of newer complete samples queued behind it.
#
# ARMS (each built from `git archive <sha>` into $BASE/src_<arm>, with its own HOME for the native bench's install
# prefix so nothing is shared with another session's build): main=912eae0e, one=132cd136, skip=$SKIP (required).
# The reference arm cyclonedds (rmw_cyclonedds_cpp from /opt/ros) runs in every repetition: the change cannot touch it,
# so it is the CONTROL for drift over the session.
#
# CELLS. rmw (apex perf_test via rmw_samehost_cell.sh, DUR s at -r 0): Array1k and Array4k BEST_EFFORT KEEP_LAST 1,
# Array1k RELIABLE (KEEP_ALL depth 1000). Native bench (DUR_NATIVE s): best_effort_throughput p3 and p4,
# reliable_latency p2 and p4. Every repetition runs every arm on every cell; the arm order rotates per repetition.
#
# READING RULES (implemented in skip_newest_summary.py, written before the run):
#   - n per arm and cell is printed beside every mean; a cell with n < 3 for an arm gives NO VERDICT for that arm.
#   - TREATMENT, per cell and repetition: the skip arm's subscriber traffic line must carry rx_shm_skipped_superseded;
#     on a BEST_EFFORT rmw cell it must be > 0 (or the run is VOID: skipping did not happen), on the RELIABLE (KEEP_ALL)
#     cell and on every native cell (depth 0) it must be 0 (or VOID: something was skipped that nothing asked to skip).
#   - CONTROL: cyclonedds delivered/s per cell; its first-half and second-half means are printed with their SE. If they
#     differ by more than 2 x the combined SE the session drifted and every verdict is marked DRIFT.
#   - TARGET (BEST_EFFORT rmw cells): skip's sample age (perf_test latency_mean) is NOT WORSE than main's (skip <= main
#     + 2 SE), AND skip's delivered/s is NOT WORSE than one's (skip >= one - 2 SE), AND skip's subscriber CPU per
#     delivered sample is NOT WORSE than one's (skip <= one + 2 SE). Each of the three is printed as MET / NOT MET.
#   - NO REGRESSION (RELIABLE rmw cell; native cells): skip vs main within 2 SE on delivered/s (throughput) or mean
#     RTT (latency); outside it in the worse direction reads WORSE.
#
# Usage: SKIP=<sha> [REPS=5] [DUR=10] [DUR_NATIVE=5] [BASE=~/skipnewest] setsid nohup skip_newest_pc.sh > log 2>&1 &
# Ends with "=== skip_newest_pc ended" always, "=== ALL_DONE" only on success. Results: $BASE/runs/, summary in
# $BASE/summary.txt.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X=$REPO/examples/perf_hil/experiments
SKIP=${SKIP:?SKIP=<sha of the skip-to-newest commit>}
MAIN=${MAIN:-912eae0e}
ONE=${ONE:-132cd136}
REPS=${REPS:-5}
DUR=${DUR:-10}
DUR_NATIVE=${DUR_NATIVE:-5}
BASE=${BASE:-$HOME/skipnewest}
PERF=${PERF:-/tmp/keepall_evict/perf/install}
ROS_SETUP=${ROS_SETUP:-/opt/ros/lyrical/setup.bash}
NS=skipnewest_$$
RUNS=$BASE/runs
mkdir -p "$RUNS"
echo "=== skip_newest_pc start $(date -Is) main=$MAIN one=$ONE skip=$SKIP reps=$REPS dur=$DUR dur_native=$DUR_NATIVE"
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    echo "=== skip_newest_pc ended $(date -Is)"
}
trap cleanup EXIT

# ---- build every arm from its own commit ------------------------------------------------------------------------
build_arm() { # build_arm <name> <sha>
    local name=$1 sha=$2 src=$BASE/src_$1
    if [ -f "$src/.built" ] && [ "$(cat "$src/.built")" = "$(git -C "$REPO" rev-parse "$sha")" ]; then
        echo "arm $name: already built from $(cat "$src/.built")"
        return 0
    fi
    rm -rf "$src" && mkdir -p "$src/home" || return 1
    git -C "$REPO" archive "$sha" | tar -x -C "$src" || return 1
    (
        cd "$src" || exit 1
        export PYTHONPATH=$src/tools/typesupport${PYTHONPATH:+:$PYTHONPATH}
        set +u
        # shellcheck disable=SC1090
        source "$ROS_SETUP"
        set -u
        colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
            --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
    ) >"$src/build_rmw.log" 2>&1 || { echo "arm $name: rmw build FAILED ($src/build_rmw.log)"; return 1; }
    local cell
    for cell in best_effort_throughput:p3 best_effort_throughput:p4 reliable_latency:p2 reliable_latency:p4; do
        (cd "$src/examples/perf_hil/tickle" && HOME=$src/home ./build.sh "${cell%%:*}" "${cell#*:}") \
            >>"$src/build_native.log" 2>&1 || { echo "arm $name: native build $cell FAILED"; return 1; }
    done
    git -C "$REPO" rev-parse "$sha" >"$src/.built"
    echo "arm $name: built $(cat "$src/.built")"
}
build_arm main "$MAIN" && build_arm one "$ONE" && build_arm skip "$SKIP" || exit 1

# ---- one namespace for the session: lo, and a dummy link carrying the configured broadcast address --------------
sudo -n ip netns add "$NS" || { echo "FATAL: cannot create netns"; exit 1; }
sudo -n ip -n "$NS" link set lo up
sudo -n ip -n "$NS" link add eth0 type dummy
sudo -n ip -n "$NS" addr add 192.168.10.2/24 brd 192.168.10.255 dev eth0
sudo -n ip -n "$NS" link set eth0 up
sudo -n ip -n "$NS" route add default dev eth0

rmw_cell() { # rmw_cell <out> <arm> <msg> <qos>
    local out=$1 arm=$2 msg=$3 qos=$4 rarm=tickle overlays=""
    if [ "$arm" = cyclonedds ]; then
        rarm=cyclonedds
    else
        overlays="$BASE/src_$arm/install/local_setup.bash"
    fi
    # The cell runs as root inside the namespace (ip netns exec), with a private /dev/shm, and hands its files back.
    sudo -n ip netns exec "$NS" env -i PATH="$PATH" HOME="$HOME" ARM="$rarm" KIND=tput MSG="$msg" QOS="$qos" \
        DUR="$DUR" CELL_DIR="$out" DOMAIN=83 ROS_SETUP="$ROS_SETUP" OVERLAYS="$overlays $PERF/local_setup.bash" \
        TICKLE_BCAST=192.168.10.255 PERF_TEST="$PERF/performance_test/lib/performance_test/perf_test" ZENOHD=/bin/false \
        OWNER="$(id -u):$(id -g)" CELL="$X/rmw_samehost_cell.sh" \
        bash -c 'mount -t tmpfs -o size=512m tmpfs /dev/shm && bash "$CELL"; rc=$?; chown -R "$OWNER" "$CELL_DIR"; exit $rc'
}

native_cell() { # native_cell <out> <arm> <scenario> <size>
    local out=$1 dir=$BASE/src_$2/examples/perf_hil/tickle/$3_$4
    mkdir -p "$out"
    # shellcheck disable=SC2024 # the logs are ours, not root's
    sudo -n ip netns exec "$NS" env -i PATH="$PATH" BENCH_IFACE=eth0 bash -c \
        'mount -t tmpfs -o size=512m tmpfs /dev/shm && { "$0/server" -Q -d $(($1 + 4)) >"$2/srv.log" 2>&1 & s=$!; sleep 1; "$0/client" -Q -d "$1" >"$2/cli.log" 2>&1; wait $s; }; chown -R "$3" "$2"' \
        "$dir" "$DUR_NATIVE" "$out" "$(id -u):$(id -g)"
}

ARMS=(main one skip)
for rep in $(seq 1 "$REPS"); do
    # Rotate the order per repetition, so no arm always runs first or last.
    k=$(((rep - 1) % 3))
    order=("${ARMS[@]:$k}" "${ARMS[@]:0:$k}")
    echo "--- rep $rep order ${order[*]} cyclonedds $(date -Is)"
    for cell in Array1k:best_effort Array4k:best_effort Array1k:reliable; do
        for arm in "${order[@]}" cyclonedds; do
            rmw_cell "$RUNS/rmw_${cell%%:*}_${cell#*:}_${arm}_r$rep" "$arm" "${cell%%:*}" "${cell#*:}" >/dev/null 2>&1
            echo "rmw ${cell} ${arm} r$rep exit $?"
        done
    done
    for cell in best_effort_throughput:p3 best_effort_throughput:p4 reliable_latency:p2 reliable_latency:p4; do
        for arm in "${order[@]}"; do
            native_cell "$RUNS/nat_${cell%%:*}_${cell#*:}_${arm}_r$rep" "$arm" "${cell%%:*}" "${cell#*:}"
            echo "native ${cell} ${arm} r$rep exit $?"
        done
    done
done
python3 "$X/skip_newest_summary.py" "$RUNS" >"$BASE/summary.txt" 2>&1
cat "$BASE/summary.txt"
echo "=== ALL_DONE"
