#!/usr/bin/env bash
# g6_localhost_pcap.sh - under ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST, does anything leave the host? (RMW_GAPS_PLAN.md
# g6, pre-registered: "a pcap control in the LOCALHOST arm: no datagram on the veth".)
#
# A talker and a listener, two rmw_tickle processes in one netns, captured on the far end of its veth for their whole
# run. Two arms: LOCALHOST (must put 0 UDP datagrams on the veth, while the listener still receives - so the capture
# is not empty for want of traffic) and SUBNET (the control: the same pair must put datagrams on the veth, or the
# capture could not see them at all).
#
# Usage: g6_localhost_pcap.sh      Results: $OUT (default /tmp/g6_localhost_pcap), summary.txt
# IFACES: an interface overlay's local_setup.bash with std_msgs for rmw_tickle (default /tmp/dev_accept's).
set -u
OUT=${OUT:-/tmp/g6_localhost_pcap}
IFACES=${IFACES:-/tmp/dev_accept/ifaces/install/local_setup.bash}
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
NS1=g6pc1
NS2=g6pc2
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
role = sys.argv[1]
rclpy.init()
node = rclpy.create_node('g6_' + role)
got = [0]
if role == 'listener':
    node.create_subscription(String, '/g6_chatter', lambda m: got.__setitem__(0, got[0] + 1), 10)
else:
    pub = node.create_publisher(String, '/g6_chatter', 10)
end = time.time() + 8
nxt = time.time() + 2
while time.time() < end:
    rclpy.spin_once(node, timeout_sec=0.05)
    if role == 'talker' and time.time() >= nxt:
        pub.publish(String(data='x'))
        nxt += 0.05
print(f'RESULT: role={role} received={got[0]}', flush=True)
PY
for range in LOCALHOST SUBNET; do
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add g6pv1 type veth peer name g6pv2 || exit 1
    sudo -n ip link set g6pv1 netns "$NS1"; sudo -n ip link set g6pv2 netns "$NS2"
    sudo -n ip -n "$NS1" addr add 10.83.0.1/24 dev g6pv1; sudo -n ip -n "$NS2" addr add 10.83.0.2/24 dev g6pv2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set g6pv1 up; sudo -n ip -n "$NS2" link set g6pv2 up
    sudo -n ip netns exec "$NS2" timeout 14 tcpdump -i g6pv2 -w "$OUT/$range.pcap" -U udp >/dev/null 2>&1 &
    cap=$!
    sleep 1
    env="set +u; source /opt/ros/lyrical/setup.bash; source $REPO/install/setup.bash; source $IFACES; export ROS_DOMAIN_ID=98 RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=10.83.0.255 ROS_AUTOMATIC_DISCOVERY_RANGE=$range ROS_LOG_DIR=$OUT/roslog HOME=$OUT/home"
    # shellcheck disable=SC2024 # the logs are this shell's
    sudo -n ip netns exec "$NS1" sudo -n -u "$USER" bash -c "$env; timeout 20 python3 $OUT/node.py listener" >"$OUT/${range}_listener.log" 2>&1 &
    lp=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" sudo -n -u "$USER" bash -c "$env; timeout 20 python3 $OUT/node.py talker" >"$OUT/${range}_talker.log" 2>&1
    wait "$lp"
    wait "$cap"
    datagrams=$(python3 -c "
import struct,sys
d=open(sys.argv[1],'rb').read(); pos=24; n=0
while pos+16<=len(d):
    incl=struct.unpack_from('<I',d,pos+8)[0]; pos+=16+incl; n+=1
print(n)" "$OUT/$range.pcap")
    received=$(sed -n 's/^RESULT: role=listener received=//p' "$OUT/${range}_listener.log")
    echo "$range: udp_datagrams_on_veth=$datagrams listener_received=${received:-0}" | tee -a "$OUT/summary.txt"
done
