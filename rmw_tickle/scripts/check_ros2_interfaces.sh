#!/usr/bin/env bash
# Proves that interface packages built by build_ros2_interfaces.sh work through rmw_tickle: sends
# std_msgs/String and std_msgs/Header between two processes and checks both arrive intact, and that
# the processes really loaded rmw_tickle and the workspace's own TickLE typesupport - a pass on the
# wrong library would prove nothing (rmw_tickle's own history has several such, see
# reference_ament_prefix_shadowing in its notes).
#
# Usage, in a shell with ROS 2 and rmw_tickle's install sourced:
#   check_ros2_interfaces.sh [-w WORKSPACE] [-r]  # the workspace build_ros2_interfaces.sh built into
#
# -r: publish from a default rclcpp::Node (default_node.cpp) instead of the rmw-level publisher -
# the first thing a user runs. rclcpp then starts the type description service, parameter services,
# /parameter_events and /rosout on its own, so this needs the workspace built with -a (rcl_interfaces,
# type_description_interfaces and their dependencies), and it passes only when that node exits 0
# having published, AND its own maps show rmw_tickle and the workspace's type_description_interfaces
# TickLE typesupport. Against a workspace without those packages it fails - rclcpp refuses to start.
#
# Loopback by default (TICKLE_BROADCAST_ADDR=127.255.255.255); set TICKLE_BROADCAST_ADDR yourself to
# check over a real link. Exit 0 only when both types round-tripped AND both libraries were proved.
set -euo pipefail

WORKSPACE="${HOME}/tickle_ros2_interfaces"
RCLCPP=0
while getopts "w:r" opt; do
    case "$opt" in
    w) WORKSPACE="$OPTARG" ;;
    r) RCLCPP=1 ;;
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
publisher=("$check" pub 10)
[ "$RCLCPP" = 0 ] || publisher=("$(dirname "$check")/default_node" 10)
# The datagram size everything here was generated for (rmw_tickle refuses a type generated for
# another): what decides whether GetTypeDescription's response fits at all.
extras=""
for prefix in "${prefixes[@]}"; do
    candidate="$prefix/share/rosidl_typesupport_tickle_c/cmake/rosidl_typesupport_tickle_c-extras.cmake"
    if [ -f "$candidate" ]; then
        extras="$candidate"
        break
    fi
done
[ -z "$extras" ] || echo "tt_MAX_BUFFER_LENGTH: $(sed -n 's/.*MAX_BUFFER_LENGTH "\([0-9]*\)".*/\1/p' "$extras")"

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
TICKLE_NODE_ID=122 "${publisher[@]}" >"$log/pub.txt" 2>&1 &
pub_pid=$!

# -r: the rclcpp node's own maps. Read while it runs, like the subscriber's below: it must have
# loaded rmw_tickle and this workspace's type_description_interfaces typesupport - the package
# whose absence is what used to stop a default node from starting.
pub_identity="not checked"
if [ "$RCLCPP" = 1 ]; then
    pub_identity="not seen"
    td_lib="$WORKSPACE/install/type_description_interfaces/lib/libtype_description_interfaces__rosidl_typesupport_tickle_c.so"
    for _ in $(seq 1 50); do
        if [ -r "/proc/$pub_pid/maps" ] && grep -q "libtype_description_interfaces__rosidl_typesupport_tickle_c" "/proc/$pub_pid/maps"; then
            mapped_rmw=$(grep -o '/[^ ]*librmw_tickle\.so' "/proc/$pub_pid/maps" | sort -u | head -1)
            mapped_td=$(grep -o '/[^ ]*libtype_description_interfaces__rosidl_typesupport_tickle_c\.so' "/proc/$pub_pid/maps" | sort -u | head -1)
            if [ "$(readlink -f "$mapped_rmw")" = "$(readlink -f "$rmw_lib")" ] &&
                [ "$(readlink -f "$mapped_td")" = "$(readlink -f "$td_lib")" ]; then
                pub_identity="OK: $mapped_rmw and $mapped_td"
            else
                pub_identity="WRONG LIBRARY: mapped $mapped_rmw and $mapped_td, expected $rmw_lib and $td_lib"
            fi
            break
        fi
        sleep 0.1
    done
fi

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
pub_status=0
wait "$pub_pid" 2>/dev/null || pub_status=$?
cat "$log/sub.txt"
echo "identity: $identity"
case "$identity" in OK:*) ;; *) fail "the libraries under test were not the ones loaded" ;; esac
if [ "$RCLCPP" = 1 ]; then
    grep -v '\[INFO\]' "$log/pub.txt" | tail -3
    echo "publisher identity: $pub_identity"
    [ "$pub_status" = 0 ] || {
        cat "$log/pub.txt" >&2
        fail "the default rclcpp::Node did not start and publish (exit $pub_status)"
    }
    case "$pub_identity" in OK:*) ;; *) fail "the rclcpp node did not load the libraries under test" ;; esac
fi
[ "$sub_status" = 0 ] || {
    echo "--- publisher ---" >&2
    cat "$log/pub.txt" >&2
    fail "std_msgs did not round-trip through rmw_tickle"
}
echo "check_ros2_interfaces: PASS"
