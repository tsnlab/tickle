#!/usr/bin/env bash
# rmw_tickle's own ctest suite, which CI's "Check all" runs and check_gates.sh did not.
#
# Why this gate exists: on 2026-10-02 a change to how rmw_create_publisher() reports a typesupport
# refusal broke test_type_checks. check-gates reported all 13 gates PASS, the commit went out, and
# main stayed red for three commits before anyone looked. Every gate was honest; none of them ran
# this suite. ~40 s incremental.
#
# The tests create rmw contexts and broadcast, so they run in a PRIVATE network namespace: a TickLE
# node in this PC's default netns reaches the rig over the 10.1.1.x management LAN and corrupts
# whatever the rig is measuring (see the project notes on default-netns cross-talk).
#
# Exit codes, which check_gates.sh maps to distinct verdicts - "I could not look" must never read
# as "it passed":
#   0   ran, every test passed
#   77  no ROS workspace with TickLE typesupport interfaces -> SKIP
#   78  no ROS installation -> SKIP
#   79  could not create or could not PROVE a private netns -> SKIP; the suite did not run
#   80  the build could not be PROVEN to come from this checkout -> NOT THIS CHECKOUT, which fails the
#       run: whatever the suite would say is about some other source tree
#   1+  the suite ran and something failed -> FAIL
#
# Which checkout is tested (2026-10-05). The workspace is shared by every checkout on this machine, and
# it used to be built in its one default build/ directory. CMake keeps the source directory it was first
# configured with in CMakeCache.txt, and colcon reuses that cache: run from a worktree, the build took
# 0.2 s, compiled nothing, and ctest ran /home/semih/tickle-dev's code - and this row said PASS about a
# checkout nobody had asked about. So each checkout now gets a build/install/log base of its own, keyed on
# its path, and after the build the script asks the build itself which sources it compiled.
#
# --behaviour (make test-rmw-behaviour, 2026-10-08): after the suite, the rmw behaviour checks CI's "Check all" runs
# and the suite does not, in the same netns against the same build - check_ros2_interfaces.sh as CI calls it: std_msgs
# String and Header between two processes, the same from a default rclcpp::Node (-r), and an rclcpp_action Fibonacci
# (-A), each against the workspace's ifaces/ and with RMW_TICKLE_PREFIX naming this checkout's install, so the chain
# of underlays that workspace was built on cannot put another rmw_tickle ahead of it. NOT run here, still CI-only: the
# upstream conformance suite (test_rmw_implementation, a patched jazzy clone this machine's ROS does not build), the
# controls that need two more interface workspaces built, and the direct-codec identity harness.
set -uo pipefail

BEHAVIOUR=0
[ "${1:-}" = "--behaviour" ] && BEHAVIOUR=1

ROS_SETUP=$(find /opt/ros -maxdepth 2 -name setup.bash -print -quit 2>/dev/null)
[ -n "$ROS_SETUP" ] || { echo "run_rmw_suite: no ROS installation under /opt/ros"; exit 78; }

WS=""
for cand in "${RMW_TEST_WS:-}" "$HOME/rmw_accept_dev" "$HOME/rmw_accept_ws"; do
    [ -n "$cand" ] && [ -d "$cand/ifaces/install" ] && { WS="$cand"; break; }
done
[ -n "$WS" ] || {
    echo "run_rmw_suite: no workspace with ifaces/install (tried \$RMW_TEST_WS, ~/rmw_accept_dev, ~/rmw_accept_ws)"
    echo "run_rmw_suite: rmw_tickle/scripts/build_ros2_interfaces.sh builds one"
    exit 77
}

TOP="$(git rev-parse --show-toplevel)" || exit 1
TOP="$(realpath "$TOP")"
SRC="$TOP/rmw_tickle"
# One base per checkout, never the workspace's default build/ (which belongs to whichever checkout
# configured it first). The hash keeps two checkouts with the same basename apart; the basename is for
# whoever lists the directory.
KEY="$(printf '%s' "$TOP" | sha1sum | cut -c1-12)-$(basename "$TOP")"
BASE="$WS/rmw/per_checkout/$KEY"
BUILD_BASE="$BASE/build"
INSTALL_BASE="$BASE/install"
mkdir -p "$BASE/log" || exit 90
export COLCON_LOG_PATH="$BASE/log"
NS="rmwgate$$"
# shellcheck disable=SC2329  # invoked by the EXIT trap below, which shellcheck cannot see
cleanup() { sudo -n ip netns del "$NS" 2>/dev/null; }
trap cleanup EXIT # the only EXIT trap here - a second would silently replace it

set +u
# shellcheck disable=SC1090
source "$ROS_SETUP"
# shellcheck disable=SC1091
source "$WS/ifaces/install/setup.bash"
set -u

cd "$WS/rmw" || exit 90
echo "run_rmw_suite: checkout $TOP -> $BASE"
if ! colcon build --base-paths "$SRC" --packages-select rmw_tickle \
    --build-base "$BUILD_BASE" --install-base "$INSTALL_BASE"; then
    echo "run_rmw_suite: build failed - the previous binary is still on disk, so NOTHING was tested"
    exit 1
fi

# Ask the build which sources it compiled, rather than trusting the directory it was given. Three
# independent witnesses, each of which named /home/semih/tickle-dev in the 2026-10-05 defect: the CMake
# cache's source dir, the object CMake made from the core's tickle.c (its path is the source's path), and
# that object's own dependency file. A witness that is missing is a failure to look, not a pass.
not_this_checkout() {
    echo "run_rmw_suite: NOT THIS CHECKOUT - $1"
    echo "run_rmw_suite: the build in $BUILD_BASE/rmw_tickle is not provably from $TOP; the suite did NOT test it"
    exit 80
}
cache="$BUILD_BASE/rmw_tickle/CMakeCache.txt"
[ -f "$cache" ] || not_this_checkout "no $cache"
cache_src=$(sed -n 's/^rmw_tickle_SOURCE_DIR:STATIC=//p' "$cache")
[ "$(realpath -m "${cache_src:-/nonexistent}")" = "$SRC/rmw_tickle" ] \
    || not_this_checkout "CMakeCache rmw_tickle_SOURCE_DIR='$cache_src', expected '$SRC/rmw_tickle'"
core_obj_d="$BUILD_BASE/rmw_tickle/CMakeFiles/rmw_tickle.dir$TOP/src/tickle.c.o.d"
[ -f "$core_obj_d" ] || not_this_checkout "no object for $TOP/src/tickle.c (looked for $core_obj_d)"
grep -qF "$TOP/src/tickle.c" "$core_obj_d" \
    || not_this_checkout "$core_obj_d does not name $TOP/src/tickle.c"
echo "run_rmw_suite: source proof cache=$cache_src core_obj=${core_obj_d#"$BUILD_BASE"/}"
lib="$INSTALL_BASE/rmw_tickle/lib/librmw_tickle.so"
# The install step rewrites the RPATH, so the bytes differ; the GNU build ID is what the linker stamped.
build_id() { readelf -n "$1" 2>/dev/null | sed -n 's/.*Build ID: //p'; }
built_id=$(build_id "$BUILD_BASE/rmw_tickle/librmw_tickle.so")
[ -n "$built_id" ] && [ "$built_id" = "$(build_id "$lib")" ] \
    || not_this_checkout "installed $lib is not the library this build made (build id '${built_id:-none}')"
echo "run_rmw_suite: testing $(md5sum "$lib" | cut -c1-12)"

if ! sudo -n ip netns add "$NS" 2>/dev/null \
    || ! sudo -n ip netns exec "$NS" ip link set lo up \
    || ! sudo -n ip netns exec "$NS" ip route add default dev lo; then
    echo "run_rmw_suite: could not create netns $NS (needs passwordless 'ip'); the suite did NOT run"
    exit 79
fi

# Prove the isolation rather than assume it. Dropping the default route does NOT make these tests
# fail, so that is no control; the namespace inode and the absence of any 10.1.1.x address are what
# decide, reported by the process that is about to run the tests.
default_ns=$(readlink /proc/self/ns/net)
proof=$(sudo -n ip netns exec "$NS" sudo -n -u "$USER" bash -c \
    'echo "$(readlink /proc/self/ns/net) addrs=$(ip -o addr show | grep -c 10\\.1\\.1\\.)"')
echo "run_rmw_suite: netns proof default=$default_ns inside=$proof"
case "$proof" in
    "$default_ns"*) echo "run_rmw_suite: would run in the DEFAULT netns; refusing"; exit 79 ;;
    *" addrs=0") : ;;
    *) echo "run_rmw_suite: a 10.1.1.x address is visible inside $NS; refusing"; exit 79 ;;
esac

sudo -n ip netns exec "$NS" sudo -n -u "$USER" bash -c "
    set +u
    source '$ROS_SETUP'
    source '$WS/ifaces/install/setup.bash'
    source '$INSTALL_BASE/setup.bash'
    set -u
    export COLCON_LOG_PATH='$BASE/log'
    cd '$WS/rmw' && colcon test --base-paths '$SRC' --packages-select rmw_tickle \
        --build-base '$BUILD_BASE' --install-base '$INSTALL_BASE' --event-handlers console_cohesion+
"
test_rc=$?
# `colcon test` exits 0 even when ctest cases FAIL - it reports "N packages had test failures" and
# returns success. `colcon test-result` is the one that carries the verdict. Taking it from the wrong
# command made this gate report PASS on a deliberately broken build the first time it was asked to
# fail, which is the whole defect this gate exists to prevent.
colcon test-result --all --test-result-base "$BUILD_BASE/rmw_tickle" | tail -5
result_rc=${PIPESTATUS[0]}
if [ "$test_rc" -ne 0 ] || [ "$result_rc" -ne 0 ]; then
    echo "run_rmw_suite: FAILED (colcon test=$test_rc, test-result=$result_rc)"
    exit 1
fi
echo "run_rmw_suite: all tests passed"
[ "$BEHAVIOUR" = 1 ] || exit 0

# One check_ros2_interfaces.sh run in the netns; its whole output is kept, and its last line is the verdict it prints
# itself - a check that printed nothing has not passed.
behaviour_failed=0
behaviour() {
    local label="$1"
    shift
    local out="$BASE/log/behaviour_${label}.log"
    # shellcheck disable=SC2024  # the log is the invoking user's file, so the redirect deliberately stays outside sudo
    sudo -n ip netns exec "$NS" sudo -n -u "$USER" bash -c "
        set +u
        source '$ROS_SETUP'
        source '$WS/ifaces/install/setup.bash'
        source '$INSTALL_BASE/setup.bash'
        set -u
        export RMW_TICKLE_PREFIX='$INSTALL_BASE/rmw_tickle'
        '$SRC/scripts/check_ros2_interfaces.sh' -w '$WS/ifaces' $*
    " >"$out" 2>&1
    local rc=$?
    local under_test
    under_test=$(sed -n 's/^rmw_tickle under test: //p' "$out")
    if [ "$rc" = 0 ] && grep -q '^check_ros2_interfaces: PASS' "$out" && [ "$under_test" = "$lib" ]; then
        echo "run_rmw_suite: behaviour $label PASS (tested $under_test)"
    else
        tail -15 "$out"
        echo "run_rmw_suite: behaviour $label FAILED (exit $rc, tested '${under_test:-not reported}', expected $lib; log $out)"
        behaviour_failed=1
    fi
}
behaviour pubsub
behaviour rclcpp -r
behaviour action -A
if [ "$behaviour_failed" != 0 ]; then
    echo "run_rmw_suite: behaviour checks FAILED"
    exit 1
fi
echo "run_rmw_suite: all tests and behaviour checks passed"
exit 0
