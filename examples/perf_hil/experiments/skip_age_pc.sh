#!/usr/bin/env bash
# skip_age_pc.sh - where skip-to-newest's extra sample age comes from, on THIS PC, in a private network namespace.
# Not a published figure (the PC is not the rig); it decides which way the arms move against each other on the same
# host in the same session. 2026-10-07.
#
# WHY. On the rig (rmw same-host, perf_test BEST_EFFORT KEEP_LAST 1 at -r 0) skip-to-newest (4c46f7cb) doubled
# Array1k delivered/s but the delivered sample's age (perf_test latency_mean) rose 3.92 -> 5.74 us. The PC run of
# skip_newest_pc.sh saw the same direction (3.1 -> 3.4 us). Hypothesis: the newest sample of a backlog is decoded only
# after the drain has walked the backlog's headers (plan_segment_skips) and passed over every older record one by one
# (segment_skip_head -> skip_superseded_record), so its age grows by that walk and that accounting.
#
# ARMS: ARMS="name:sha ..." (default main:912eae0e skip:4c46f7cb), each built from `git archive <sha>`. PROF=1 builds
# every arm with skip_age_profile.py's clock reads (a separate build, arm name suffixed _prof) - the profiled build
# is for the breakdown only; its age and rate are not compared against an unprofiled arm.
# CONTROL: cyclonedds on Array1k in every repetition - nothing here can touch it.
# CELLS: CELLS="Array1k Array4k" (BEST_EFFORT KEEP_LAST 1, -r 0, DUR s). Arm order rotates per repetition.
#
# READING RULES (implemented in skip_age_summary.py, written before the run):
#   - n per arm and cell printed beside every mean; n < 3 for an arm gives NO VERDICT for it.
#   - TREATMENT: every non-main tickle arm must report rx_shm_skipped_superseded > 0 per run, main must report none
#     (its build has no such counter); otherwise the run is VOID.
#   - CONTROL: cyclonedds delivered/s first-half vs second-half means; differing by > 2 x combined SE marks DRIFT.
#   - TARGET for each arm X other than main and skip: age(X) <= age(main) + 2 SE (combined), delivered(X) >=
#     delivered(skip) - 2 SE, CPU/sample(X) <= CPU/sample(skip) + 2 SE. Each printed MET / NOT MET.
#   - PROFILE (PROF=1): per arm, ns per drain in plan, per skipped record, per read, and kl_pre (the time the
#     delivered sample waits inside the drain before its decode starts). The hypothesis is FALSIFIED if skip's kl_pre
#     is less than a third of skip's PC age excess over main.
#
# Usage: [ARMS=...] [PROF=0] [REPS=5] [DUR=10] [BASE=~/skipage] setsid nohup skip_age_pc.sh > log 2>&1 < /dev/null &
# Ends with "=== skip_age_pc ended" always, "=== ALL_DONE" only on success. Results in $BASE/runs_<tag>/.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X=$REPO/examples/perf_hil/experiments
ARMS_SPEC=${ARMS:-main:912eae0e skip:4c46f7cb}
PROF=${PROF:-0}
REPS=${REPS:-5}
DUR=${DUR:-10}
CELLS=${CELLS:-Array1k Array4k}
BASE=${BASE:-$HOME/skipage}
TAG=${TAG:-$(date +%Y%m%d-%H%M%S)}
PERF=${PERF:-/tmp/keepall_evict/perf/install}
ROS_SETUP=${ROS_SETUP:-/opt/ros/lyrical/setup.bash}
NS=skipage_$$
RUNS=$BASE/runs_$TAG
mkdir -p "$RUNS"
echo "=== skip_age_pc start $(date -Is) arms='$ARMS_SPEC' prof=$PROF reps=$REPS dur=$DUR cells='$CELLS' runs=$RUNS"
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    echo "=== skip_age_pc ended $(date -Is)"
}
trap cleanup EXIT

build_arm() { # build_arm <name> <sha>: into $BASE/src_<name>[_prof]
    local name=$1 sha=$2 full src
    full=$(git -C "$REPO" rev-parse "$sha") || return 1
    src=$BASE/src_$name
    [ "$PROF" = 1 ] && src=${src}_prof
    if [ -f "$src/.built" ] && [ "$(cat "$src/.built")" = "$full prof=$PROF" ]; then
        echo "arm $name: already built $(cat "$src/.built")"
        return 0
    fi
    rm -rf "$src" && mkdir -p "$src" || return 1
    git -C "$REPO" archive "$full" | tar -x -C "$src" || return 1
    if [ "$PROF" = 1 ]; then
        python3 "$X/skip_age_profile.py" "$src/src/tickle.c" || { echo "arm $name: profile patch FAILED"; return 1; }
    fi
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
    echo "$full prof=$PROF" >"$src/.built"
    echo "arm $name: built $(cat "$src/.built")"
}
NAMES=()
for spec in $ARMS_SPEC; do
    build_arm "${spec%%:*}" "${spec#*:}" || exit 1
    NAMES+=("${spec%%:*}")
done

sudo -n ip netns add "$NS" || { echo "FATAL: cannot create netns"; exit 1; }
sudo -n ip -n "$NS" link set lo up
sudo -n ip -n "$NS" link add eth0 type dummy
sudo -n ip -n "$NS" addr add 192.168.10.2/24 brd 192.168.10.255 dev eth0
sudo -n ip -n "$NS" link set eth0 up
sudo -n ip -n "$NS" route add default dev eth0

rmw_cell() { # rmw_cell <out> <arm> <msg>
    local out=$1 arm=$2 msg=$3 rarm=tickle overlays="" src
    if [ "$arm" = cyclonedds ]; then
        rarm=cyclonedds
    else
        src=$BASE/src_$arm
        [ "$PROF" = 1 ] && src=${src}_prof
        overlays="$src/install/local_setup.bash"
    fi
    sudo -n ip netns exec "$NS" env -i PATH="$PATH" HOME="$HOME" ARM="$rarm" KIND=tput MSG="$msg" QOS=best_effort \
        DUR="$DUR" CELL_DIR="$out" DOMAIN=84 ROS_SETUP="$ROS_SETUP" OVERLAYS="$overlays $PERF/local_setup.bash" \
        TICKLE_BCAST=192.168.10.255 PERF_TEST="$PERF/performance_test/lib/performance_test/perf_test" ZENOHD=/bin/false \
        OWNER="$(id -u):$(id -g)" CELL="$X/rmw_samehost_cell.sh" \
        bash -c 'mount -t tmpfs -o size=512m tmpfs /dev/shm && bash "$CELL"; rc=$?; chown -R "$OWNER" "$CELL_DIR"; exit $rc'
}

n=${#NAMES[@]}
for rep in $(seq 1 "$REPS"); do
    k=$(((rep - 1) % n))
    order=("${NAMES[@]:$k}" "${NAMES[@]:0:$k}")
    echo "--- rep $rep order ${order[*]} $(date -Is)"
    for msg in $CELLS; do
        arms=("${order[@]}")
        [ "$msg" = Array1k ] && arms+=(cyclonedds)
        for arm in "${arms[@]}"; do
            rmw_cell "$RUNS/${msg}_${arm}_r$rep" "$arm" "$msg" >/dev/null 2>&1
            echo "rmw $msg $arm r$rep exit $?"
        done
    done
done
python3 "$X/skip_age_summary.py" "$RUNS" "${NAMES[@]}" >"$RUNS/summary.txt" 2>&1
cat "$RUNS/summary.txt"
echo "=== ALL_DONE"
