#!/usr/bin/env bash
# rclcpp_r_shape_once.sh CHECK_INSTALL: one run of check_ros2_interfaces.sh -r's process shape, without rebuilding the
# check package every run - a C subscriber (interfaces_check sub 15, node 121), a C++ default-node subscriber
# (default_node sub 15, node 123) and, a second later, a default-node publisher (default_node pub 10, node 122). Run
# from a directory of its own (flaky_repeat_netns.sh gives each run one): each process's output goes there.
#
# For chasing the C++ subscriber segfault seen once in ~370 -r runs (2026-09-25, ROADMAP "Other open items").
# Exit 0 only when all three exit 0. Otherwise prints one line per process with its status, "SIGNAL n" for one that
# died on a signal (139 = SIGSEGV), so a crash is never read as an ordinary FAIL. The environment (ROS, the interface
# workspace, the rmw_tickle under test) is the caller's; RMW_IMPLEMENTATION and the loopback broadcast are set here as
# check_ros2_interfaces.sh sets them.
set -u
CHECK="$1/rmw_tickle_interfaces_check/lib/rmw_tickle_interfaces_check"
[ -x "$CHECK/default_node" ] || { echo "no $CHECK/default_node"; exit 2; }
export RMW_IMPLEMENTATION=rmw_tickle
export TICKLE_BROADCAST_ADDR="${TICKLE_BROADCAST_ADDR:-127.255.255.255}"

TICKLE_NODE_ID=121 "$CHECK/interfaces_check" sub 15 >sub.txt 2>&1 &
sub_pid=$!
TICKLE_NODE_ID=123 "$CHECK/default_node" sub 15 >cpp_sub.txt 2>&1 &
cpp_sub_pid=$!
sleep 1
TICKLE_NODE_ID=122 "$CHECK/default_node" pub 10 >pub.txt 2>&1 &
pub_pid=$!

rc=0
report() {
    local name="$1" status="$2"
    if [ "$status" -gt 128 ]; then
        echo "$name: SIGNAL $((status - 128)) (exit $status)"
        rc=1
    elif [ "$status" != 0 ]; then
        echo "$name: exit $status"
        rc=1
    else
        echo "$name: ok"
    fi
}
s=0
wait "$sub_pid" || s=$?
report c_sub "$s"
s=0
wait "$pub_pid" || s=$?
report pub "$s"
s=0
wait "$cpp_sub_pid" || s=$?
report cpp_sub "$s"
grep -q "PASS - all four arrived intact" cpp_sub.txt || { echo "cpp_sub: no PASS line"; rc=1; }
exit "$rc"
