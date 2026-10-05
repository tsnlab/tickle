#!/usr/bin/env bash
# shellcheck disable=SC2030,SC2031 # each build subshell sets its own PYTHONPATH, on purpose
# Does an rmw_tickle subscriber's rmw_wait() honour its timeout once the writer has gone?
#
# Observation (2026-10-05, rmw_keepall_evict_repro.sh with SUB_CPUS set): perf_test -c ROS2 --reliable KEEP_ALL
# -r 0, publisher and subscriber in two netns joined by a veth, the subscriber pinned to one CPU. In 2 of 2 runs the
# subscriber never exited: its runner thread sat in Executor::spin_once(100 ms) -> rmw_wait -> tt_Context_poll ->
# tt_receive -> ppoll for 87 minutes. perf_test's ROS 2 callback communicator always passes 100 ms, so a ppoll that
# does not end is a wait that lost its timeout somewhere between rmw_wait() and ppoll().
#
# Shape: the same perf_test, two private netns (never this PC's default one), a veth, optional netem loss on the
# publisher's egress. rmw_tickle and its typesupport are built from `git archive` of THIS checkout's HEAD (or
# SRC_REF) into $W, RelWithDebInfo so gdb can name frames. PATCH=<file> applies a patch to that copy first (an
# instrumented or mutant build); BUILD_TAG names the build dir so several can coexist.
#
# Each run: the subscriber is given max_runtime DUR+8 s. When the subscriber has not exited GRACE s after that, the
# run is a HANG, and before killing it the harness records, from the stuck process itself: gdb `thread apply all bt`,
# and per thread its /proc wchan and 2 s of `strace -e ppoll,poll` (the ppoll timeout argument: NULL = no timeout).
#
# HOW TO READ IT, written before running and enforced by the analysis at the bottom:
#   VOID run: the subscriber's /proc/PID/maps does not show THIS build's librmw_tickle.so, or it received nothing.
#   Arms: pin_ka (subscriber pinned, RELIABLE KEEP_ALL, LOSS%) - the subject; free_ka (same, unpinned) and pin_be
#     (pinned, BEST_EFFORT) - controls the hypothesis says should exit.
#   REPRODUCED: >= 1 pin_ka run hangs, and no control run hangs.
#   NOT REPRODUCED: no pin_ka run hangs in REPS (with the 2/2 rate seen, 4 clean reps would happen < 7% of the time
#     if the true rate were >= 50%).
#   CONTROL FAILED: a control arm hangs too - the factor is not what the arm separates; read the captures anyway.
#   FIXED (a PATCH build): REPS pin_ka runs, none hangs, while the same harness reproduced the hang on the parent.
#   MECHANISM: a stuck thread whose strace shows ppoll(..., NULL, ...) inside rmw_wait -> tt_Context_poll waits with
#     no timeout at all - a timeout lost on the way down; ppoll(..., {tv_sec=..}) that keeps returning means the wait
#     is bounded and something above it loops instead.
#
# Second campaign, rules added after the first (parent, 2026-10-05 18:09) and before running it. The first showed
# pin_ka 3/3 and pin_be 4/4 hung, free_ka 0/4, and every hung capture's ppoll timed (ppoll_NULL=0 in 7 of 7): the
# factor is the pinning, not KEEP_ALL, and the timeout reaches ppoll. perf_test's SpinLock (utilities/spin_lock.hpp)
# is a bare test_and_set loop, and its runner holds it across spin_once(100 ms) while sync_reset() spins for it on the
# main thread - on one CPU the spinner can only run while the holder sleeps, i.e. while the lock is held.
#   pin_cyc (pinned, RELIABLE KEEP_ALL, rmw_cyclonedds_cpp on both sides) is the control that separates the two:
#     pin_cyc hangs -> the hang needs neither rmw_tickle nor anything in it: perf_test's lock starves under pinning.
#     pin_cyc exits in every run while pin_ka hangs -> something rmw_tickle does is required; keep looking in rmw.
#   Third campaign, rules written before it ran. The second gave pin_ka 3/3 hung with rmw_wait re-entered every
#   ~110 ms inside the hung process, and pin_cyc 3/3 hung with the same main-thread frame (VOID by the rule above:
#   Cyclone never matched here, so it shows only that an idle pinned Cyclone subscriber hangs the same way).
#   PERF_FAIR_LOCK=1 builds perf_test from a COPY of ~/rmw_perf_ws/src/performance_test whose SpinLock is a ticket
#   lock that yields while waiting (FIFO: the runner's next lock() queues behind sync_reset()). pin2_ka pins the
#   subscriber to two CPUs with the stock perf_test.
#     fair lock: pin_ka and pin_be exit in every run -> the starvation of sync_reset() on perf_test's unfair lock is
#       the whole cause; any hang left -> something else is also needed, read its capture.
#     pin2_ka exits in every run -> a single CPU is required (the spinner and the holder must share one).
#   rmw_wait_entries (a gdb breakpoint on rmw_wait in the hung process, RW_PROBES continues): >= 3 entries in the
#     probe means rmw_wait keeps returning and is re-entered - the runner is looping, not stuck in one call.
#
# Usage: rmw_wait_hang_repro.sh
#   env: W=$HOME/rmw_wait_hang  BUILD_TAG=parent  PATCH=  SKIP_BUILD=0  ARMS="pin_ka free_ka pin_be"  REPS=4  DUR=15
#        LOSS=5  SUB_CPUS=4  GRACE=20
set -o pipefail
REPO="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
W="${W:-$HOME/rmw_wait_hang}"
TAG="${BUILD_TAG:-parent}"
B="$W/build_$TAG"
OUT="${OUT:-$W/run_${TAG}_$(date +%Y%m%d-%H%M%S)}"
DUR="${DUR:-15}"; REPS="${REPS:-4}"; LOSS="${LOSS:-5}"; SUB_CPUS="${SUB_CPUS:-4}"; GRACE="${GRACE:-20}"
ARMS="${ARMS:-pin_ka free_ka pin_be}"; RW_PROBES="${RW_PROBES:-6}"
NS_P=rwh-pub; NS_S=rwh-sub; VP=rwhv0; VS=rwhv1; NET=192.168.78; BCAST=$NET.255
mkdir -p "$OUT"
exec > >(tee -a "$OUT/summary.txt") 2>&1
say() { echo "[$(date +%T)] $*"; }

# The one EXIT trap: kill what this script started (by the PIDs it recorded), then drop the namespaces.
cleanup() {
    for f in "$OUT"/*.pid; do
        [ -f "$f" ] || continue
        local p ns
        p=$(cat "$f")
        ns=$NS_S; case "$f" in *_pub.pid) ns=$NS_P ;; esac
        if [ -d "/proc/$p" ] && [ "$(cat "/proc/$p/comm" 2>/dev/null)" = perf_test ]; then
            sudo -n ip netns exec "$ns" kill -9 "$p" 2>/dev/null
        fi
    done
    sudo -n ip netns del "$NS_P" 2>/dev/null
    sudo -n ip netns del "$NS_S" 2>/dev/null
}
trap cleanup EXIT

# ---- build ---------------------------------------------------------------------------------------------------------
REF="${SRC_REF:-HEAD}"
SHA=$(git -C "$REPO" rev-parse --short "$REF")
build() {
    local src="$B/src"
    rm -rf "${B:?}"
    mkdir -p "$src"
    git -C "$REPO" archive "$REF" | tar -x -C "$src" || return 1
    if [ -n "${PATCH:-}" ]; then
        patch -d "$src" -p1 < "$PATCH" > "$B/patch.log" 2>&1 || { say "PATCH FAILED, see $B/patch.log"; return 1; }
    fi
    (
        set +u
        # shellcheck disable=SC1091
        . /opt/ros/lyrical/setup.bash
        export PYTHONPATH="$src/tools/typesupport${PYTHONPATH:+:$PYTHONPATH}"
        cd "$B" && colcon build --base-paths "$src" --build-base "$B/build" --install-base "$B/install" \
            --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
            --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
    ) > "$B/build.log" 2>&1 || { say "BUILD FAILED, see $B/build.log"; return 1; }
    [ -f "$B/install/rmw_tickle/lib/librmw_tickle.so" ] || { say "no librmw_tickle.so"; return 1; }
    # perf_test's message typesupport must come from this generator (see rmw_keepall_evict_repro.sh).
    (
        set +u
        # shellcheck disable=SC1091
        . /opt/ros/lyrical/setup.bash
        # shellcheck disable=SC1091
        . "$B/install/setup.bash"
        export PYTHONPATH="$src/tools/typesupport${PYTHONPATH:+:$PYTHONPATH}"
        cd "$B" && colcon build --base-paths "$HOME/rmw_perf_ws/src/performance_test" --build-base "$B/perf/build" \
            --install-base "$B/perf/install" --packages-select performance_test \
            --cmake-args -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DPERFORMANCE_TEST_RT_ENABLED=ON \
            -DPERFORMANCE_TEST_CALLBACK_EXECUTOR_ENABLED=ON
    ) > "$B/build_perf.log" 2>&1 || { say "BUILD FAILED (perf_test), see $B/build_perf.log"; return 1; }
    [ -x "$B/perf/install/performance_test/lib/performance_test/perf_test" ] || { say "no perf_test"; return 1; }
    say "built $TAG from $SHA${PATCH:+ + $PATCH}"
}
# perf_test with a fair SpinLock, for the third campaign: the same sources and flags, one header replaced.
build_fair_perf() {
    local psrc="$B/perf_fair_src"
    rm -rf "${psrc:?}" "${B:?}/perf_fair"
    cp -a "$HOME/rmw_perf_ws/src/performance_test" "$psrc"
    cat > "$psrc/performance_test/src/utilities/spin_lock.hpp" <<'HPP'
// rmw_wait_hang_repro.sh experiment arm: a FIFO ticket lock in place of perf_test's test_and_set SpinLock.
#ifndef UTILITIES__SPIN_LOCK_HPP_
#define UTILITIES__SPIN_LOCK_HPP_
#include <atomic>
#include <cstdint>
#include <thread>
namespace performance_test
{
class SpinLock
{
public:
  inline void lock()
  {
    const uint32_t ticket = m_next.fetch_add(1, std::memory_order_relaxed);
    while (m_serving.load(std::memory_order_acquire) != ticket) {
      std::this_thread::yield();
    }
  }
  inline void unlock() {m_serving.fetch_add(1, std::memory_order_release);}

private:
  std::atomic<uint32_t> m_next{0};
  std::atomic<uint32_t> m_serving{0};
};
}  // namespace performance_test
#endif  // UTILITIES__SPIN_LOCK_HPP_
HPP
    (
        set +u
        # shellcheck disable=SC1091
        . /opt/ros/lyrical/setup.bash
        # shellcheck disable=SC1091
        . "$B/install/setup.bash"
        export PYTHONPATH="$B/src/tools/typesupport${PYTHONPATH:+:$PYTHONPATH}"
        cd "$B" && colcon build --base-paths "$psrc" --build-base "$B/perf_fair/build" \
            --install-base "$B/perf_fair/install" --packages-select performance_test \
            --cmake-args -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DPERFORMANCE_TEST_RT_ENABLED=ON \
            -DPERFORMANCE_TEST_CALLBACK_EXECUTOR_ENABLED=ON
    ) > "$B/build_perf_fair.log" 2>&1 || { say "BUILD FAILED (fair perf_test), see $B/build_perf_fair.log"; return 1; }
    say "built fair-lock perf_test"
}
PERF=perf
if [ "${PERF_FAIR_LOCK:-0}" = 1 ]; then
    PERF=perf_fair
    [ -x "$B/perf_fair/install/performance_test/lib/performance_test/perf_test" ] || build_fair_perf || exit 1
fi
if [ "${SKIP_BUILD:-0}" != 1 ]; then build || exit 1; fi

# ---- netns ---------------------------------------------------------------------------------------------------------
sudo -n ip netns del "$NS_P" 2>/dev/null; sudo -n ip netns del "$NS_S" 2>/dev/null
if ! sudo -n ip netns add "$NS_P" || ! sudo -n ip netns add "$NS_S"; then say "FATAL cannot create netns"; exit 1; fi
sudo -n ip link add "$VP" netns "$NS_P" type veth peer name "$VS" netns "$NS_S" || { say "FATAL veth"; exit 1; }
for pair in "$NS_P $VP 1" "$NS_S $VS 2"; do
    read -r ns dev host <<<"$pair"
    sudo -n ip netns exec "$ns" ip link set lo up
    sudo -n ip netns exec "$ns" ip addr add "$NET.$host/24" broadcast "$BCAST" dev "$dev"
    sudo -n ip netns exec "$ns" ip link set "$dev" up
    sudo -n ip netns exec "$ns" ip route add default dev "$dev"
done
set_loss() {
    sudo -n ip netns exec "$NS_P" tc qdisc del dev "$VP" root 2>/dev/null
    if [ "$1" != 0 ]; then sudo -n ip netns exec "$NS_P" tc qdisc add dev "$VP" root netem loss "$1%" || return 1; fi
}

# Runs inside a namespace as root: starts perf_test, records ITS pid ($!) and the librmw_tickle.so it mapped.
LAUNCH="$OUT/launch.sh"
cat > "$LAUNCH" <<'EOF'
#!/bin/bash
stem=$1; shift
set +u
. /opt/ros/lyrical/setup.bash
. "$B/$PERF/install/setup.bash"
. "$B/install/local_setup.bash"
export AMENT_PREFIX_PATH="$B/install/rmw_tickle:$AMENT_PREFIX_PATH"
export LD_LIBRARY_PATH="$B/install/rmw_tickle/lib:$B/install/rosidl_typesupport_tickle_c/lib:$B/install/rosidl_typesupport_tickle_cpp/lib:${LD_LIBRARY_PATH:-}"
export RMW_IMPLEMENTATION=${RMW:-rmw_tickle} TICKLE_BROADCAST_ADDR=$BCAST
cd "$OUT"
${CPUS:+taskset -c $CPUS} "$B/$PERF/install/performance_test/lib/performance_test/perf_test" "$@" > "$OUT/$stem.log" 2>&1 < /dev/null &
pid=$!
echo "$pid" > "$OUT/$stem.pid"
sleep 3
grep -o '/[^ ]*librmw_[a-z_]*\.so' "/proc/$pid/maps" 2>/dev/null | sort -u > "$OUT/$stem.maps"
wait "$pid"
echo "EXIT $?" >> "$OUT/$stem.log"
EOF
chmod +x "$LAUNCH"

# Everything the stuck subscriber can say about itself, taken from the process, before it is killed.
capture_hang() { # stem pid
    local stem=$1 pid=$2 cap="$OUT/$1_hang.txt"
    {
        echo "=== HANG $stem pid=$pid comm=$(cat "/proc/$pid/comm") $(date +%T)"
        for t in /proc/"$pid"/task/*; do
            echo "tid=${t##*/} comm=$(cat "$t/comm") state=$(awk '{print $3}' "$t/stat") wchan=$(cat "$t/wchan")"
        done
        echo "=== gdb"
        sudo -n ip netns exec "$NS_S" gdb -batch -p "$pid" -ex 'thread apply all bt' 2>&1 | grep -v '^\[New LWP'
        echo "=== strace (2 s, every thread)"
        sudo -n ip netns exec "$NS_S" timeout 2 strace -f -tt -p "$pid" -e trace=ppoll,poll,futex 2>&1 | head -200
        echo "=== rmw_wait entries ($RW_PROBES continues from a breakpoint on rmw_wait)"
        local probe=(-ex 'set pagination off' -ex 'break rmw_wait')
        # shellcheck disable=SC2016 # gdb's own `shell` expands $(date), at each breakpoint hit
        for _ in $(seq 1 "$RW_PROBES"); do probe+=(-ex continue -ex 'shell echo RW_ENTRY $(date +%T.%N)'); done
        sudo -n ip netns exec "$NS_S" timeout 20 gdb -batch -p "$pid" "${probe[@]}" 2>&1 | grep -E 'RW_ENTRY|Breakpoint 1,'
        echo "=== gdb again (still the same frames?)"
        sudo -n ip netns exec "$NS_S" gdb -batch -p "$pid" -ex 'thread apply all bt 8' 2>&1 | grep -v '^\[New LWP'
    } > "$cap" 2>&1
}

run_one() { # arm rep
    local arm=$1 rep=$2 stem="$1_r$2"
    local qos=(--reliable) cpus="" loss=$LOSS rmw=rmw_tickle
    case "$arm" in
        pin_cyc) cpus=$SUB_CPUS; rmw=rmw_cyclonedds_cpp ;;
        pin2_ka) cpus="$SUB_CPUS,$((SUB_CPUS + 1))" ;;
        pin_ka) cpus=$SUB_CPUS ;;
        free_ka) cpus="" ;;
        pin_be) cpus=$SUB_CPUS; qos=() ;;
    esac
    set_loss "$loss" || { say "tc failed"; exit 1; }
    local rosargs=(--ros-args --param start_type_description_service:=false)
    sudo -n ip netns exec "$NS_S" env B="$B" PERF="$PERF" OUT="$OUT" BCAST="$BCAST" HOME="$HOME" CPUS="$cpus" RMW="$rmw" TICKLE_NODE_ID=102 \
        "$LAUNCH" "${stem}_sub" -c ROS2 -t Array1k "${qos[@]}" -p 0 -s 1 --expected_num_pubs 1 \
        --max_runtime $((DUR + 8)) "${rosargs[@]}" &
    sleep 2
    sudo -n ip netns exec "$NS_P" env B="$B" PERF="$PERF" OUT="$OUT" BCAST="$BCAST" HOME="$HOME" RMW="$rmw" TICKLE_NODE_ID=101 \
        "$LAUNCH" "${stem}_pub" -c ROS2 -t Array1k "${qos[@]}" -r 0 -p 1 -s 0 --expected_num_subs 1 \
        --max_runtime "$DUR" "${rosargs[@]}"
    # The subscriber started 2 s before the publisher and runs DUR+8 s: it should be gone ~6 s from now.
    local waited=0 spid
    spid=$(cat "$OUT/${stem}_sub.pid")
    while [ -d "/proc/$spid" ] && [ "$(cat "/proc/$spid/comm" 2>/dev/null)" = perf_test ] && [ $waited -lt $((8 + GRACE)) ]; do
        sleep 1; waited=$((waited + 1))
    done
    local verdict=exited
    if [ -d "/proc/$spid" ] && [ "$(cat "/proc/$spid/comm" 2>/dev/null)" = perf_test ]; then
        verdict=HANG
        capture_hang "$stem" "$spid"
        sudo -n ip netns exec "$NS_S" kill -9 "$spid"
        sleep 1
    fi
    wait
    local want=$B/install/rmw_tickle/lib/librmw_tickle.so
    [ "$rmw" = rmw_tickle ] || want=/opt/ros/lyrical/lib/lib$rmw.so
    echo "RUN $stem verdict=$verdict want=$want" >> "$OUT/index.txt"
    say "  $stem $verdict"
}

say "=== rmw_wait_hang_repro $TAG perf=$PERF ($SHA${PATCH:+ + $PATCH}) ARMS=$ARMS REPS=$REPS DUR=$DUR LOSS=$LOSS SUB_CPUS=$SUB_CPUS ==="
for rep in $(seq 1 "$REPS"); do
    for arm in $ARMS; do run_one "$arm" "$rep"; done
done
set_loss 0
say "=== runs done ==="

python3 - "$OUT" <<'PY'
import re, sys
from pathlib import Path
out = Path(sys.argv[1])
rows = []
for line in (out / "index.txt").read_text().splitlines():
    m = re.match(r"RUN (\S+) verdict=(\S+) want=(\S+)", line)
    stem, verdict, want = m.groups()
    arm = stem.rsplit("_r", 1)[0]
    maps = (out / f"{stem}_sub.maps").read_text().split() if (out / f"{stem}_sub.maps").exists() else []
    log = (out / f"{stem}_sub.log").read_text(errors="replace") if (out / f"{stem}_sub.log").exists() else ""
    recv = 0
    lines = log.splitlines()
    if "---EXPERIMENT-START---" in lines:
        i = lines.index("---EXPERIMENT-START---")
        for l in lines[i + 2:]:
            c = [x.strip() for x in l.split(",")]
            if len(c) > 3 and re.match(r"^[0-9.]+$", c[0]):
                recv += int(float(c[2]))
    void = []
    if want not in maps:
        void.append(f"mapped {maps or 'nothing'}")
    if recv == 0:
        void.append("received 0")
    mech = ""
    cap = out / f"{stem}_hang.txt"
    if cap.exists():
        t = cap.read_text(errors="replace")
        nul = len(re.findall(r"ppoll\(\[.*\], \d+, NULL", t))
        timed = len(re.findall(r"ppoll\(\[.*\], \d+, \{tv_sec", t))
        mech = (f" ppoll_NULL={nul} ppoll_timed={timed} rmw_wait_frames={t.count('rmw_wait')}"
                f" rmw_wait_entries={t.count('RW_ENTRY')}")
    rows.append((arm, verdict, void))
    print(f"{stem:14s} {verdict:7s} recv={recv} {'VOID(' + '; '.join(void) + ')' if void else 'valid'}{mech}")
valid = [r for r in rows if not r[2]]
hangs = {a: sum(1 for r in valid if r[0] == a and r[1] == "HANG") for a in {r[0] for r in rows}}
n = {a: sum(1 for r in valid if r[0] == a) for a in {r[0] for r in rows}}
print("--- hangs per arm (valid runs):", {a: f"{hangs[a]}/{n[a]}" for a in sorted(n)})
subj = "pin_ka"
ctrl = [a for a in n if a != subj]
if n.get(subj, 0) == 0:
    print("VERDICT: VOID - no valid pin_ka run")
elif hangs[subj] > 0 and all(hangs[a] == 0 for a in ctrl):
    print("VERDICT: REPRODUCED - pin_ka hangs, controls exit")
elif hangs[subj] > 0:
    print("VERDICT: CONTROL FAILED - a control arm hangs too:", {a: hangs[a] for a in ctrl})
else:
    print(f"VERDICT: NOT REPRODUCED / no hang in {n[subj]} pin_ka runs")
if n.get("pin_cyc", 0) > 0:
    if hangs["pin_cyc"] > 0:
        print(f"VENDOR CONTROL: pin_cyc hangs {hangs['pin_cyc']}/{n['pin_cyc']} - the hang needs no rmw_tickle")
    else:
        print(f"VENDOR CONTROL: pin_cyc exits {n['pin_cyc']}/{n['pin_cyc']} - rmw_tickle is required for the hang")
PY
