#!/usr/bin/env bash
# A new endpoint announced at once, and a changed announce answered (2026-09-26, Plan's pre-registration):
# does an rmw_tickle ping stop broadcasting its first samples, and what does it cost in announces?
#
# ARMS: before (core without the change: src/tickle.c and include/tickle/tickle.h stashed) and after (the
# working tree), rmw_tickle rebuilt for each, a ping-pong on a veth pair between two private netns, bench,
# best effort, --wait block, 10 s at 10 ms, 3 reps each. tcpdump in each namespace counts that host's
# broadcast datagrams - announces, once the ping unicasts its data.
#
# HOW TO READ IT, written before running (Plan's criteria):
#   success: the ping's rx_self_sent_data (data it broadcast and heard itself) is 0 in every after run.
#   cost: each host's broadcast count in the after arm is within +10% of the before arm's, allowing for the
#   ping's early broadcast data in the before arm (subtracted: its rx_self_sent_data).
#   control: the before arm shows how often the late registration happens on this PC at all; if it is 0
#   here, the after arm's 0 proves nothing about the fix on its own, and the unit test
#   (test_peer_discovery: a new publisher learns a known peer within 5 ms) is the evidence.
#
# SEEN, 3 reps each (2026-09-26): ping_rx_self_sent_data before 3/0/0, after 0/0/0; broadcasts per host over
# the run, ping 14 -> 15 (+7%), pong 18 -> 18-19 (+0-6%); every run registered its peer. The first version of
# the change (early announce and reply only) gave 1/0/3 after: the early announce sat batched for up to a
# flush tick and pulled the first samples into its broadcast - hence publishing now flushes a pending
# broadcast first and unicasts from an empty buffer.
#
# RE-USED 2026-09-26 for rmw_publisher_count_matched_subscriptions() (STASH_PATHS=rmw_graph.c): counting the
# graph let the ping's wait for a match end before the core registered the peer, so its first sample went
# by broadcast (Plan's M6 pcaps: the ping topic's endpoint, 5 of 24 rig pings). HOW TO READ IT, written
# before running: after, ping_rx_self_sent_data is 0 in every rep; before shows it in some (the control -
# if before is 0 in all 10 on this PC, the race does not reproduce here and the rig's M6 capture decides).
# The pong's own first reply can still go by broadcast when it has not yet heard the ping's Subscription
# (its announce after the ping's Publisher's); that is pong-side and not what this counts.
#
# Usage: STASH_PATHS="<paths>" rmw_late_peer_check.sh <reps>   Results: $OUT (default /tmp/rmw_late_peer_check.txt)
# Rebuilds rmw_tickle in this checkout twice; nothing else may build in it meanwhile.
set -u
REPS=${1:?usage: rmw_late_peer_check.sh <reps>}
OUT=${OUT:-/tmp/rmw_late_peer_check.txt}
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
NS1=lpc-ns1
NS2=lpc-ns2
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
restore() {
    cleanup
    git -C "$REPO" stash list | grep -q "rmw_late_peer_check" && git -C "$REPO" stash pop -q
    return 0
}
trap restore EXIT

build_rmw() {
    (
        set +u
        cd "$REPO" || exit 1
        # shellcheck disable=SC1091
        . /opt/ros/lyrical/setup.bash
        colcon build --packages-select rmw_tickle --cmake-args -DBUILD_SHARED_LIBS=ON >>/tmp/lpc_build.log 2>&1
    ) || { echo "build failed" >>"$OUT"; exit 1; }
}

run_one() { # <arm>
    local arm=$1
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add lpc1 type veth peer name lpc2 || exit 1
    sudo -n ip link set lpc1 netns "$NS1"
    sudo -n ip link set lpc2 netns "$NS2"
    sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev lpc1
    sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev lpc2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set lpc1 up
    sudo -n ip -n "$NS2" link set lpc2 up
    local env=". /opt/ros/lyrical/setup.bash; . $REPO/install/setup.bash; export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_HOME=/tmp/lpc_roshome"
    # shellcheck disable=SC2024 # the capture files are this shell's
    sudo -n ip netns exec "$NS1" timeout 20 tcpdump -i lpc1 -nn -l "udp and dst host 192.168.10.255" >/tmp/lpc_bc1.txt 2>/dev/null &
    local td1=$!
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS2" timeout 20 tcpdump -i lpc2 -nn -l "udp and dst host 192.168.10.255" >/tmp/lpc_bc2.txt 2>/dev/null &
    local td2=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS2" bash -c "$env; timeout -s INT 15 ros2 run rmw_perf_pingpong pong_node" >/tmp/lpc_pong.log 2>&1 &
    local pong=$!
    sleep 2
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" bash -c "$env; timeout -s INT 14 ros2 run rmw_perf_pingpong ping_node -d 10 -i 0.01 --wait block" >/tmp/lpc_ping.log 2>&1
    wait "$pong"
    wait "$td1" "$td2" 2>/dev/null
    local self_sent bc_ping bc_pong
    self_sent=$(grep -o "rx_self_sent_data=[0-9]*" /tmp/lpc_ping.log | tail -1 | cut -d= -f2)
    bc_ping=$(grep -c " 192.168.10.1\." /tmp/lpc_bc1.txt)
    bc_pong=$(grep -c " 192.168.10.2\." /tmp/lpc_bc2.txt)
    echo "arm=$arm $(grep -h '^RESULT' /tmp/lpc_ping.log | tr ' ' '\n' | grep -E '^(sent|recv|rtt_avg_ms)=' | paste -sd' ') ping_rx_self_sent_data=${self_sent:-na} ping_broadcasts=$bc_ping pong_broadcasts=$bc_pong registered=$(grep -c 'Publisher peer registered' /tmp/lpc_ping.log)" >>"$OUT"
}

: >"$OUT"
# shellcheck disable=SC2086 # STASH_PATHS is a list of paths on purpose
git -C "$REPO" stash push -q -m rmw_late_peer_check -- ${STASH_PATHS:-src/tickle.c include/tickle/tickle.h} || exit 1
build_rmw
for _ in $(seq 1 "$REPS"); do run_one before; done
git -C "$REPO" stash pop -q
build_rmw
for _ in $(seq 1 "$REPS"); do run_one after; done
echo "DONE" >>"$OUT"
