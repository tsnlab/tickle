#!/usr/bin/env bash
# Does the rmw ping-pong harness measure its own polling loop? (2026-09-26, rmw_tickle/RMW_PERF_PLAN.md §6)
#
# RMW_PERF_PLAN §6 found ~240 us of every rmw round trip outside all of rmw_tickle's stamps. ping_node.cpp's
# default wait (--wait poll) is spin_some() plus a 100 us sleep, and it reads the round trip after the
# loop: every row includes the sleep after the spin that took the reply, and a reply is only noticed on
# the loop's ~150 us cadence (sleep plus timer slack). --wait block waits in spin_once() and reads the
# round trip in the callback, as the native client waits in tt_Node_poll().
#
# ARMS: rmw_tickle, rmw_cyclonedds_cpp, rmw_fastrtps_cpp x --wait poll | block, best effort, the same pong,
# a veth pair between two private netns on this PC, interleaved, 3 reps.
#
# HOW TO READ IT, written before running:
#   CONTROL: the pong is identical in both modes; only the ping's waiting differs. Each vendor's own
#   poll - block difference is therefore the harness, not the vendor.
#   If the harness is the missing time: poll - block is ~100-250 us for EVERY vendor (the sleep plus up
#   to one cadence), and block RTTs fall close to the native TickLE round trip on this link plus the
#   rmw layers' stamped cost (~tens of us).
#   If poll - block is small (< ~30 us) for all, the loop is not where the time goes and §6 stands.
#   Either way, whether the vendor ORDER changes between poll and block says how much the poll loop's
#   quantisation distorted the comparison.
#
# SEEN, 3 reps at 5f70c3cc (x86 veth, mean RTT ms, poll -> block):
#   rmw_tickle          0.369-0.383 -> 0.134-0.154   (-230 us)
#   rmw_cyclonedds_cpp  0.414-0.427 -> 0.146-0.177   (-255 us)
#   rmw_fastrtps_cpp    0.445-0.454 -> 0.183-0.215   (-255 us)
#   The loop is ~240 us of every row, for every vendor. poll also inverted the minimum: rmw_tickle 0.28
#   against CycloneDDS 0.18-0.23 in poll, 0.063-0.066 against 0.068-0.075 in block. The rig decides.
#
# SEEN with the LOOP: line, 2 reps at ac2dd490 (x86 veth, poll mode): iterations per round trip,
# spin_some() mean, real sleep mean
#   rmw_tickle          2.14-2.21   5.5 us    160 us   -> 0.373-0.385 ms
#   rmw_cyclonedds_cpp  2.00-2.01   41-42 us  156-161  -> 0.422-0.426
#   rmw_fastrtps_cpp    2.02-2.03   43-46 us  155-159  -> 0.431-0.450
#   A poll row is iterations x (spin + sleep). rmw_tickle's spin_some() is ~8x cheaper, so its cycle is
#   shorter and its reply is caught one iteration later more often: the poll-mode gap between vendors
#   is where each one's true round trip falls on the loop's grid, not a cost in the round trip.
#
# Usage: rmw_ping_wait_mode.sh <reps>   Results: $OUT (default /tmp/rmw_ping_wait_mode.txt)
set -u
REPS=${1:?usage: rmw_ping_wait_mode.sh <reps>}
OUT=${OUT:-/tmp/rmw_ping_wait_mode.txt}
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
NS1=rpw-ns1
NS2=rpw-ns2
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

setup_ns() {
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add rpw1 type veth peer name rpw2 || exit 1
    sudo -n ip link set rpw1 netns "$NS1"
    sudo -n ip link set rpw2 netns "$NS2"
    sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev rpw1
    sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev rpw2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set rpw1 up
    sudo -n ip -n "$NS2" link set rpw2 up
}

run_one() { # <rmw> <wait mode>
    local rmw=$1 mode=$2
    setup_ns
    local env=". /opt/ros/lyrical/setup.bash; . $REPO/install/setup.bash; export RMW_IMPLEMENTATION=$rmw TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_HOME=/tmp/rpw_roshome ROS_DOMAIN_ID=42"
    # shellcheck disable=SC2024 # the logs are this shell's
    sudo -n ip netns exec "$NS2" bash -c "$env; timeout -s INT 14 ros2 run rmw_perf_pingpong pong_node" >/tmp/rpw_pong.log 2>&1 &
    local pong=$!
    sleep 3
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" bash -c "$env; timeout -s INT 12 ros2 run rmw_perf_pingpong ping_node -d 4 -i 0.01 --wait $mode" >/tmp/rpw_ping.log 2>&1
    wait "$pong"
    echo "rmw=$rmw mode=$mode $(grep -h '^RESULT' /tmp/rpw_ping.log | tr ' ' '\n' | grep -E '^(wait|sent|recv|rtt_min_ms|rtt_avg_ms|rtt_max_ms)=' | paste -sd' ') $(grep -h '^LOOP:' /tmp/rpw_ping.log | sed 's/^LOOP: //')" >>"$OUT"
}

: >"$OUT"
echo "build: $(git -C "$REPO" rev-parse --short HEAD) + working tree" >>"$OUT"
for _ in $(seq 1 "$REPS"); do
    for rmw in rmw_tickle rmw_cyclonedds_cpp rmw_fastrtps_cpp; do
        for mode in poll block; do
            run_one "$rmw" "$mode"
        done
    done
done
echo "DONE" >>"$OUT"
