#!/usr/bin/env bash
# rmw_service_info_check.sh - `ros2 service info --verbose` on rmw_cyclonedds_cpp and on rmw_tickle, for comparing
# lyrical's rmw_get_clients_info_by_service / rmw_get_servers_info_by_service (RMW_GAPS_PLAN.md g3, 2026-09-28).
# Two private netns joined by a veth: a node in the second (its parameter services are the servers), and a client of
# /svc_server_node/get_parameters in the first, where the query runs too - under TICKLE_NODE_ID=11, since two
# rmw_tickle processes on one address otherwise share a context id and drop each other's datagrams (rmw_init.c).
#
# Usage: rmw_service_info_check.sh -w WS     (WS as rmw_gap_acceptance.sh's: rmw/install and ifaces/install)
# Output: $OUT/info_<rmw>.txt (default OUT /tmp/rmw_service_info_check). Expected: "Clients count: 1" with
# svc_client_node and "Services count: 1" with svc_server_node on both rmws.
set -u
WS=
[ "${1:-}" = -w ] && WS=${2:-}
[ -n "$WS" ] && [ -d "$WS/rmw/install" ] && [ -d "$WS/ifaces/install" ] || { echo "usage: $0 -w WS" >&2; exit 2; }
DISTRO=/opt/ros/lyrical
OUT=${OUT:-/tmp/rmw_service_info_check}
NS1=rsic1-$$
NS2=rsic2-$$
mkdir -p "$OUT/home"
cleanup() {
    for ns in "$NS1" "$NS2"; do
        for p in $(sudo -n ip netns pids "$ns" 2>/dev/null); do sudo -n ip netns exec "$ns" kill -TERM "$p" 2>/dev/null; done
        sudo -n ip netns del "$ns" 2>/dev/null
    done
    return 0
}
trap cleanup EXIT
cat >"$OUT/server.py" <<'PY'
import sys, time, rclpy
rclpy.init()
node = rclpy.create_node('svc_server_node')  # serves /svc_server_node/get_parameters, as every node does
end = time.time() + float(sys.argv[1])
while rclpy.ok() and time.time() < end:
    rclpy.spin_once(node, timeout_sec=0.1)
PY
cat >"$OUT/client.py" <<'PY'
import sys, time, rclpy
from rcl_interfaces.srv import GetParameters
rclpy.init()
node = rclpy.create_node('svc_client_node')
client = node.create_client(GetParameters, '/svc_server_node/get_parameters')
end = time.time() + float(sys.argv[1])
while rclpy.ok() and time.time() < end:
    rclpy.spin_once(node, timeout_sec=0.1)
PY
env_for() { # RMW IDX
    local e="source $DISTRO/setup.bash; export ROS_DOMAIN_ID=92 RMW_IMPLEMENTATION=$1 ROS_LOG_DIR=$OUT/roslog HOME=$OUT/home"
    case "$1" in
        rmw_tickle) e="$e; source $WS/rmw/install/setup.bash; source $WS/ifaces/install/local_setup.bash; export TICKLE_BROADCAST_ADDR=10.78.0.255" ;;
        rmw_cyclonedds_cpp) e="$e; export CYCLONEDDS_URI='<CycloneDDS><Domain><General><Interfaces><NetworkInterface address=\"10.78.0.$2\"/></Interfaces></General></Domain></CycloneDDS>'" ;;
    esac
    echo "$e"
}
for rmw in rmw_cyclonedds_cpp rmw_tickle; do
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add rsiv1 netns "$NS1" type veth peer name rsiv2 netns "$NS2" || exit 1
    sudo -n ip -n "$NS1" addr add 10.78.0.1/24 dev rsiv1; sudo -n ip -n "$NS2" addr add 10.78.0.2/24 dev rsiv2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set rsiv1 up; sudo -n ip -n "$NS2" link set rsiv2 up
    sudo -n ip -n "$NS1" route add 224.0.0.0/4 dev rsiv1; sudo -n ip -n "$NS2" route add 224.0.0.0/4 dev rsiv2
    # shellcheck disable=SC2024 # the logs are this shell's
    sudo -n ip netns exec "$NS2" sudo -n -u "$USER" bash -c "set +u; $(env_for "$rmw" 2); timeout 40 python3 $OUT/server.py 30" >"$OUT/server_$rmw.log" 2>&1 &
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" sudo -n -u "$USER" bash -c "set +u; $(env_for "$rmw" 1); timeout 40 python3 $OUT/client.py 30" >"$OUT/client_$rmw.log" 2>&1 &
    sleep 8
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" sudo -n -u "$USER" bash -c "set +u; $(env_for "$rmw" 1); export TICKLE_NODE_ID=11; timeout 30 ros2 service info /svc_server_node/get_parameters --verbose --no-daemon" >"$OUT/info_$rmw.txt" 2>&1
    echo "== $rmw: $(grep -E '^(Clients|Services) count' "$OUT/info_$rmw.txt" | tr '\n' ' ')"
    wait
done
