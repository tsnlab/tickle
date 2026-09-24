#!/usr/bin/env bash
# Proves that interface packages built by build_ros2_interfaces.sh work through rmw_tickle: sends
# std_msgs/String and std_msgs/Header between two processes and checks both arrive intact, and that
# the processes really loaded rmw_tickle and the workspace's own TickLE typesupport - a pass on the
# wrong library would prove nothing (rmw_tickle's own history has several such, see
# reference_ament_prefix_shadowing in its notes).
#
# Usage, in a shell with ROS 2 and rmw_tickle's install sourced:
#   check_ros2_interfaces.sh [-w WORKSPACE]      # the workspace build_ros2_interfaces.sh built into
#
# Loopback by default (TICKLE_BROADCAST_ADDR=127.255.255.255); set TICKLE_BROADCAST_ADDR yourself to
# check over a real link. Exit 0 only when both types round-tripped AND both libraries were proved.
set -euo pipefail

WORKSPACE="${HOME}/tickle_ros2_interfaces"
while getopts "w:" opt; do
    case "$opt" in
    w) WORKSPACE="$OPTARG" ;;
    *) exit 2 ;;
    esac
done

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fail() {
    echo "check_ros2_interfaces: $*" >&2
    exit 1
}
[ -n "${ROS_DISTRO:-}" ] || fail "no ROS 2 environment sourced"
[ -f "$WORKSPACE/install/setup.bash" ] || fail "no $WORKSPACE/install - run build_ros2_interfaces.sh first"

# ROS setup scripts read unset variables.
set +u
# shellcheck disable=SC1091
. "$WORKSPACE/install/setup.bash"
set -u

rmw_lib=""
IFS=: read -r -a prefixes <<<"${AMENT_PREFIX_PATH:-}"
for prefix in "${prefixes[@]}"; do
    if [ -f "$prefix/lib/librmw_tickle.so" ]; then
        rmw_lib="$prefix/lib/librmw_tickle.so"
        break
    fi
done
[ -n "$rmw_lib" ] || fail "librmw_tickle.so not found on AMENT_PREFIX_PATH - source rmw_tickle's install"
ts_lib="$WORKSPACE/install/std_msgs/lib/libstd_msgs__rosidl_typesupport_tickle_c.so"
[ -f "$ts_lib" ] || fail "no $ts_lib - std_msgs was not built with TickLE typesupport"

colcon build --base-paths "$HERE/../rmw_tickle_interfaces_check" --build-base "$WORKSPACE/build" \
    --install-base "$WORKSPACE/install" --packages-select rmw_tickle_interfaces_check >/dev/null ||
    fail "building rmw_tickle_interfaces_check failed"
set +u
# shellcheck disable=SC1091
. "$WORKSPACE/install/setup.bash"
set -u
check="$WORKSPACE/install/rmw_tickle_interfaces_check/lib/rmw_tickle_interfaces_check/interfaces_check"

# This workspace's libraries first, so the shadowing a stale install on LD_LIBRARY_PATH causes is not
# what gets measured (and /proc/PID/maps below would say so if it were).
rmw_lib_dir="$(dirname "$rmw_lib")"
export LD_LIBRARY_PATH="$rmw_lib_dir:$WORKSPACE/install/std_msgs/lib:$WORKSPACE/install/builtin_interfaces/lib:${LD_LIBRARY_PATH:-}"
export RMW_IMPLEMENTATION=rmw_tickle
export TICKLE_BROADCAST_ADDR="${TICKLE_BROADCAST_ADDR:-127.255.255.255}"

log="$(mktemp -d)"
trap 'kill "${sub_pid:-}" 2>/dev/null || true; rm -rf "$log"' EXIT
TICKLE_NODE_ID=121 "$check" sub 15 >"$log/sub.txt" 2>&1 &
sub_pid=$!
sleep 1
TICKLE_NODE_ID=122 "$check" pub 10 >"$log/pub.txt" 2>&1 &
pub_pid=$!

# Identity, read from the subscriber while it runs: the exact files, not name patterns.
identity="not seen"
for _ in $(seq 1 50); do
    if [ -r "/proc/$sub_pid/maps" ] && grep -q "libstd_msgs__rosidl_typesupport_tickle_c" "/proc/$sub_pid/maps"; then
        mapped_rmw=$(grep -o '/[^ ]*librmw_tickle\.so' "/proc/$sub_pid/maps" | sort -u | head -1)
        mapped_ts=$(grep -o '/[^ ]*libstd_msgs__rosidl_typesupport_tickle_c\.so' "/proc/$sub_pid/maps" | sort -u | head -1)
        if [ "$(readlink -f "$mapped_rmw")" = "$(readlink -f "$rmw_lib")" ] &&
            [ "$(readlink -f "$mapped_ts")" = "$(readlink -f "$ts_lib")" ]; then
            identity="OK: $mapped_rmw and $mapped_ts"
        else
            identity="WRONG LIBRARY: mapped $mapped_rmw and $mapped_ts, expected $rmw_lib and $ts_lib"
        fi
        break
    fi
    sleep 0.1
done

sub_status=0
wait "$sub_pid" || sub_status=$?
wait "$pub_pid" 2>/dev/null || true
cat "$log/sub.txt"
echo "identity: $identity"
case "$identity" in OK:*) ;; *) fail "the libraries under test were not the ones loaded" ;; esac
[ "$sub_status" = 0 ] || {
    echo "--- publisher ---" >&2
    cat "$log/pub.txt" >&2
    fail "std_msgs did not round-trip through rmw_tickle"
}
echo "check_ros2_interfaces: PASS"
