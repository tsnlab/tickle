#!/usr/bin/env bash
# Reproduces off CI the segfault in the rclcpp default-node publisher that check_ros2_interfaces.sh
# -r hits (CI add73df8, "Check all"). Builds what the reproduction needs, then runs it.
#
# Why the whole chain is here rather than run by hand: the workspace has to be built against THIS
# tree's rmw_tickle, and a reproduction assembled from three commands typed at different times is
# how you end up testing yesterday's library. The identity checks inside check_ros2_interfaces.sh
# are what prove that did not happen.
#
# Detached-safe: the script lives in the repo and the log in /tmp, so a session that dies takes
# neither with it.
#
# Pre-registered reading:
#   SEGV reproduced  -> a local core/stack to work from; the rest of the night is a fix.
#   -r passes here   -> the crash needs something CI has and this does not (load, timing, or the
#                       runner's own ROS build). Say that, do not read it as fixed.
#   build fails      -> VOID: nothing is learned about the crash.
set -u
REPO=/home/semih/tickle-dev
WS="$HOME/tickle_ros2_interfaces"
cd "$REPO" || exit 1
set +u
# shellcheck disable=SC1091
. /opt/ros/jazzy/setup.bash 2>/dev/null || . /opt/ros/lyrical/setup.bash
set -u

echo "=== 1/3 colcon build rmw_tickle + typesupport $(date -Is) ==="
colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
    --cmake-args -DBUILD_SHARED_LIBS=ON || { echo "VOID: rmw build failed"; exit 1; }

set +u
# shellcheck disable=SC1091
. "$REPO/install/setup.bash"
set -u

echo "=== 2/3 build_ros2_interfaces -a $(date -Is) ==="
"$REPO/rmw_tickle/scripts/build_ros2_interfaces.sh" -a -w "$WS" || { echo "VOID: interfaces build failed"; exit 1; }

echo "=== 3/3 check_ros2_interfaces -r $(date -Is) ==="
# ulimit so the crash leaves something to look at, and the netns rule does not apply here: this runs
# on loopback (TICKLE_BROADCAST_ADDR=127.255.255.255), which never leaves the host.
ulimit -c unlimited
CHECK_ROS2_KEEP_LOGS=/tmp/rclcpp_ifaces_logs "$REPO/rmw_tickle/scripts/check_ros2_interfaces.sh" -w "$WS" -r
echo "=== check exit $? at $(date -Is) ==="

# The oversize mixed-stream window used to be read here too, from these same kept logs. It now has
# its own instrument, check_mixed_stream_window.sh, which gates the reading on the libraries having
# been the ones under test and distinguishes "no traffic line was printed" from "the counter was
# zero" - a distinction this copy did not make, and the reason a reading of 0 was quoted from a run
# whose counters had not been read at all. One question, one instrument.
