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
#   1+  the suite ran and something failed -> FAIL
set -uo pipefail

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

SRC="$(git rev-parse --show-toplevel)/rmw_tickle"
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
if ! colcon build --base-paths "$SRC" --packages-select rmw_tickle; then
    echo "run_rmw_suite: build failed - the previous binary is still on disk, so NOTHING was tested"
    exit 1
fi
echo "run_rmw_suite: testing $(md5sum "$WS"/rmw/install/rmw_tickle/lib/librmw_tickle.so | cut -c1-12)"

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
    source '$WS/rmw/install/setup.bash'
    set -u
    cd '$WS/rmw' && colcon test --base-paths '$SRC' --packages-select rmw_tickle --event-handlers console_cohesion+
"
test_rc=$?
# `colcon test` exits 0 even when ctest cases FAIL - it reports "N packages had test failures" and
# returns success. `colcon test-result` is the one that carries the verdict. Taking it from the wrong
# command made this gate report PASS on a deliberately broken build the first time it was asked to
# fail, which is the whole defect this gate exists to prevent.
colcon test-result --all | tail -5
result_rc=${PIPESTATUS[0]}
if [ "$test_rc" -ne 0 ] || [ "$result_rc" -ne 0 ]; then
    echo "run_rmw_suite: FAILED (colcon test=$test_rc, test-result=$result_rc)"
    exit 1
fi
echo "run_rmw_suite: all tests passed"
exit 0
