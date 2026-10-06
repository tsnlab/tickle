#!/usr/bin/env bash
# rmw_samehost_many.sh - g8's two same-host checks beyond rmw_gap_acceptance.sh's samehost (RMW_GAPS_PLAN.md, 2026-09-28).
#   many:      8 rclpy nodes as 8 processes in one netns, no TICKLE_NODE_ID. Each reports how many of the other 7 it
#              sees, and a ninth process's `ros2 node list` must name all 8. PASS: every node sees 7, the list names 8.
#   collision: talker and listener in one netns with the host registry off (TICKLE_ID_REGISTRY=off), so both take the
#              same preferred id and only the link-level safety net can separate them. PASS: the listener gets at least
#              50 of about 140 and one of the two logs "moves to".
# Each case runs on rmw_cyclonedds_cpp first as the control (it must PASS, or the case is VOID), then on rmw_tickle.
#
# Usage: rmw_samehost_many.sh -w WS [CASE...]   (WS as rmw_gap_acceptance.sh's: rmw/install and ifaces/install)
# Output: $OUT (default /tmp/rmw_samehost_many), summary.txt at the end.
set -u
WS=
[ "${1:-}" = -w ] && WS=${2:-} && shift 2
[ -n "$WS" ] && [ -d "$WS/rmw/install" ] && [ -d "$WS/ifaces/install" ] || { echo "usage: $0 -w WS [many|collision...]" >&2; exit 2; }
CASES=${*:-many collision}
DISTRO=/opt/ros/lyrical
OUT=${OUT:-/tmp/rmw_samehost_many}
NS1=rsm1-$$
NS2=rsm2-$$
NODES=8
mkdir -p "$OUT/home"
: >"$OUT/summary.txt"
cleanup() {
    for ns in "$NS1" "$NS2"; do
        for p in $(sudo -n ip netns pids "$ns" 2>/dev/null); do sudo -n ip netns exec "$ns" kill -TERM "$p" 2>/dev/null; done
        sudo -n ip netns del "$ns" 2>/dev/null
    done
    return 0
}
trap cleanup EXIT
cat >"$OUT/node.py" <<'PY'
import sys, time, rclpy
from std_msgs.msg import String
role, seconds = sys.argv[1], float(sys.argv[2])
rclpy.init()
node = rclpy.create_node(role)
received = 0
sent = 0
def on_message(msg):
    global received
    received += 1
if role == 'listener':
    node.create_subscription(String, '/rsm_chatter', on_message, 10)
elif role == 'talker':
    pub = node.create_publisher(String, '/rsm_chatter', 10)
end = time.time() + seconds
next_send = time.time() + 2.0
seen = 0
while rclpy.ok() and time.time() < end:
    rclpy.spin_once(node, timeout_sec=0.05)
    if role == 'talker' and time.time() >= next_send:
        pub.publish(String(data=f'msg-{sent}'))
        sent += 1
        next_send += 0.05
    if role.startswith('many_') and time.time() > end - 3.0:
        seen = max(seen, len([n for n in node.get_node_names() if n.startswith('many_') and n != role]))
print(f'RESULT: role={role} received={received} sent={sent} seen={seen}', flush=True)
PY
env_for() { # RMW EXTRA
    local e="source $DISTRO/setup.bash; export ROS_DOMAIN_ID=93 RMW_IMPLEMENTATION=$1 ROS_LOG_DIR=$OUT/roslog HOME=$OUT/home"
    case "$1" in
        rmw_tickle) e="$e; source $WS/rmw/install/setup.bash; source $WS/ifaces/install/local_setup.bash; export TICKLE_BROADCAST_ADDR=10.79.0.255" ;;
        rmw_cyclonedds_cpp) e="$e; export CYCLONEDDS_URI='<CycloneDDS><Domain><General><Interfaces><NetworkInterface address=\"10.79.0.1\"/></Interfaces></General></Domain></CycloneDDS>'" ;;
    esac
    [ -n "${2:-}" ] && e="$e; export $2"
    echo "$e"
}
run_in() { # RMW EXTRA SECONDS CMD...
    local rmw=$1 extra=$2 secs=$3
    shift 3
    sudo -n ip netns exec "$NS1" sudo -n -u "$USER" bash -c "set +u; $(env_for "$rmw" "$extra"); timeout $((secs + 20)) $*"
}
setup_ns() {
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add rsmv1 netns "$NS1" type veth peer name rsmv2 netns "$NS2" || exit 1
    sudo -n ip -n "$NS1" addr add 10.79.0.1/24 dev rsmv1; sudo -n ip -n "$NS2" addr add 10.79.0.2/24 dev rsmv2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set rsmv1 up; sudo -n ip -n "$NS2" link set rsmv2 up
    sudo -n ip -n "$NS1" route add 224.0.0.0/4 dev rsmv1
}
field() { grep -h '^RESULT:' "$1" 2>/dev/null | tail -1 | tr ' ' '\n' | sed -n "s/^$2=//p"; }
case_many() {
    local rmw=$1 d=$OUT/many_$1 pids=() ok=1
    mkdir -p "$d"
    for i in $(seq 1 "$NODES"); do
        # shellcheck disable=SC2024 # the logs are this shell's
        run_in "$rmw" "" 15 python3 "$OUT/node.py" "many_$i" 15 >"$d/node_$i.log" 2>&1 &
        pids+=($!)
    done
    sleep 8
    run_in "$rmw" "" 10 ros2 node list --no-daemon >"$d/list.log" 2>&1
    for p in "${pids[@]}"; do wait "$p"; done
    local listed seen_all=0
    listed=$(grep -c '^/many_' "$d/list.log")
    for i in $(seq 1 "$NODES"); do
        [ "$(field "$d/node_$i.log" seen)" = $((NODES - 1)) ] && seen_all=$((seen_all + 1))
    done
    [ "$listed" = "$NODES" ] && [ "$seen_all" = "$NODES" ] || ok=0
    if [ "$ok" = 1 ]; then echo PASS; else echo "FAIL(listed=$listed nodes_seeing_all=$seen_all)"; fi
}
case_collision() {
    local rmw=$1 d=$OUT/collision_$1
    mkdir -p "$d"
    # shellcheck disable=SC2024
    run_in "$rmw" TICKLE_ID_REGISTRY=off 12 python3 "$OUT/node.py" listener 12 >"$d/listener.log" 2>&1 &
    local lp=$!
    sleep 1
    # shellcheck disable=SC2024
    run_in "$rmw" TICKLE_ID_REGISTRY=off 11 python3 "$OUT/node.py" talker 11 >"$d/talker.log" 2>&1
    wait "$lp"
    local got moved=0
    got=$(field "$d/listener.log" received)
    [ "$rmw" != rmw_tickle ] || grep -q 'moves to' "$d/listener.log" "$d/talker.log" && moved=1
    if [ "${got:-0}" -ge 50 ] && [ "$moved" = 1 ]; then echo PASS; else echo "FAIL(listener=${got:-0} moved=$moved)"; fi
}
for c in $CASES; do
    setup_ns
    control=$("case_$c" rmw_cyclonedds_cpp)
    setup_ns
    if [ "${control%%(*}" != PASS ]; then
        verdict="VOID (control $control)"
    else
        verdict=$("case_$c" rmw_tickle)
    fi
    echo "$c | rmw_cyclonedds_cpp $control | rmw_tickle $verdict" | tee -a "$OUT/summary.txt"
done
