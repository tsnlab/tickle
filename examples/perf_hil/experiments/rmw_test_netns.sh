#!/usr/bin/env bash
# Builds rmw_tickle in a checkout and runs its colcon tests inside a private netns (2026-09-27).
#
# Why a netns: in the PC's default netns the tests' limited broadcasts (255.255.255.255) leave by the default
# route onto the 10.1.1.x management LAN the rig's Pis are also on, and the rig's TickLE nodes answer them -
# they voided TickLE liveliness rows (a PC test node "departed") and a PC process showed up among a rig ping's
# peers. The netns has only lo and a dummy default route, so nothing leaves the machine.
#
# Why the LOADED line: a checkout built on an underlay (UNDERLAY=, for the typesupport packages) runs its tests
# with the underlay's library path first, so they load the underlay's librmw_tickle unless the checkout's own
# install is sourced after it - which this does, and then records which library a test binary resolves.
#
# Usage: [REPO=<checkout>] [UNDERLAY=<setup.bash>] rmw_test_netns.sh
# Results: /tmp/rmw_build.log, /tmp/rmw_test.log, /tmp/rmw_test_result.log; a summary on stdout.
set -u
REPO=${REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}
cd "$REPO" || exit 1
set +u
# shellcheck disable=SC1091
. /opt/ros/lyrical/setup.bash
# shellcheck disable=SC1090
[ -n "${UNDERLAY:-}" ] && . "$UNDERLAY"
set -u
colcon build --packages-select rmw_tickle --cmake-args -DBUILD_SHARED_LIBS=ON >/tmp/rmw_build.log 2>&1
rc=$?
echo "BUILD_EXIT $rc"
[ "$rc" = 0 ] || { echo ALL_DONE; exit 1; }
NS=tt-rmwtest
sudo -n ip netns del "$NS" 2>/dev/null
sudo -n ip netns add "$NS" || { echo "netns failed"; echo ALL_DONE; exit 1; }
sudo -n ip -n "$NS" link set lo up
sudo -n ip -n "$NS" link add dummy0 type dummy
sudo -n ip -n "$NS" addr add 10.99.0.1/24 dev dummy0
sudo -n ip -n "$NS" link set dummy0 up
sudo -n ip -n "$NS" route add default dev dummy0
# shellcheck disable=SC2016,SC2024 # expanded by the inner shell (REPO, UNDERLAY via env); the log is ours
sudo -n ip netns exec "$NS" setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups \
    env HOME="$HOME" REPO="$REPO" UNDERLAY="${UNDERLAY:-}" bash -c '
        set +u
        . /opt/ros/lyrical/setup.bash
        [ -n "$UNDERLAY" ] && . "$UNDERLAY"
        [ -f "$REPO/install/setup.bash" ] && . "$REPO/install/setup.bash"
        echo "LOADED $(ldd "$REPO"/build/rmw_tickle/test_domain_id 2>/dev/null | grep librmw_tickle)" >/tmp/rmw_test_loaded.txt
        cd "$REPO" && colcon test --packages-select rmw_tickle' >/tmp/rmw_test.log 2>&1
echo "TEST_RUN_EXIT $?"
sudo -n ip netns del "$NS"
colcon test-result --verbose >/tmp/rmw_test_result.log 2>&1
echo "TEST_RESULT_EXIT $?"
tail -3 /tmp/rmw_test_result.log
cat /tmp/rmw_test_loaded.txt
echo "ALL_DONE"
