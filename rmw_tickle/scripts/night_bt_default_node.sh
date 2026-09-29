#!/usr/bin/env bash
# A backtrace for the rclcpp default-node publisher crash (CI add73df8 "Check all", reproduced
# locally 2026-09-29 22:40). Runs the same binary check_ros2_interfaces.sh -r runs, under gdb, with
# the same environment.
#
# gdb on one side only moved the fault to the other side earlier tonight, in a different test - so
# this runs ONLY the publisher, which is the process that crashes here, and takes whatever stack it
# gets. If the publisher declines to crash under gdb, that is itself the finding and the script says
# so rather than reporting a pass.
#
# Loopback (127.255.255.255) as check_ros2_interfaces.sh uses, so nothing leaves the host and the
# netns rule for rig cross-talk does not apply.
set -u
REPO=/home/semih/tickle-dev
WS="$HOME/tickle_ros2_interfaces"
OUT=${OUT:-/tmp/night_bt_default_node.log}
BIN="$REPO/build/rmw_tickle_interfaces_check/default_node"

[ -x "$BIN" ] || { echo "VOID: $BIN not built"; exit 1; }
command -v gdb >/dev/null || { echo "VOID: no gdb"; exit 1; }

set +u
# shellcheck disable=SC1091
. /opt/ros/jazzy/setup.bash 2>/dev/null || . /opt/ros/lyrical/setup.bash
# shellcheck disable=SC1091
. "$REPO/install/setup.bash"
# shellcheck disable=SC1091
. "$WS/install/setup.bash"
set -u

export RMW_IMPLEMENTATION=rmw_tickle
export TICKLE_BROADCAST_ADDR=127.255.255.255
export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST

{
    echo "=== library identity, before anything else ==="
    echo "librmw_tickle: $(readlink -f "$REPO/install/rmw_tickle/lib/librmw_tickle.so")"
    ls -l --time-style=+%Y-%m-%dT%H:%M:%S "$REPO/install/rmw_tickle/lib/librmw_tickle.so" "$REPO/src/tickle.c"
    echo "=== run ==="
    # The two subscribers first, as check_ros2_interfaces.sh -r starts them, and only then the
    # publisher under gdb. Without them the publisher has no peer at all: it broadcasts everything,
    # tx_shm stays 0, and it runs to completion having never touched the segment - which is what the
    # first version of this script measured and read as "does not crash under gdb".
    CHECK="$REPO/build/rmw_tickle_interfaces_check/interfaces_check"
    TICKLE_NODE_ID=121 "$CHECK" sub 15 >/tmp/night_bt_sub.txt 2>&1 &
    sub_pid=$!
    TICKLE_NODE_ID=123 "$BIN" sub 15 >/tmp/night_bt_cpp_sub.txt 2>&1 &
    cpp_pid=$!
    sleep 2
    TICKLE_NODE_ID=122 gdb -batch -ex run -ex "echo \n=== STACK ===\n" -ex "bt full" \
        -ex "info registers rip" --args "$BIN" pub 10
    echo "=== gdb returned, subscribers' tails ==="
    kill "$sub_pid" "$cpp_pid" 2>/dev/null || true
    tail -5 /tmp/night_bt_sub.txt /tmp/night_bt_cpp_sub.txt 2>/dev/null
    echo "=== gdb exit $? ==="
} >"$OUT" 2>&1

if grep -q 'SIGSEGV' "$OUT"; then
    echo "=== SIGSEGV, stack follows ==="
    sed -n '/=== STACK ===/,$p' "$OUT" | head -40
else
    echo "=== no SIGSEGV under gdb: that is the finding, not a pass ==="
    tail -15 "$OUT"
fi
echo "full log: $OUT"
