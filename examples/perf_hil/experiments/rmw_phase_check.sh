#!/usr/bin/env bash
# rmw_phase_check.sh - does ping_node's --phase random really put the reply at a random phase of the poll loop?
# (RMW_PERF_PLAN 10.1's proposal (a), branch only, pending the user's decision; 2026-09-27.) PC-only: ping and pong
# in two private netns joined by a veth pair (never the default netns - it reaches the rig over the 10.1.1.x LAN),
# BEST_EFFORT bench, poll mode at a 100 us sleep, a round trip every 20 ms for 10 s (~500 pings). RMW (default
# rmw_tickle) picks the rmw on both sides: --phase random publishes from a second thread, which every rmw must take
# by rclcpp's contract, so each of the three is run through it.
#
# Arms, each read from ping_node's PHASE: line:
#   locked (control)   today's loop. Every publish comes just before the first check, so every ping should sit in
#                      the last of the 10 bins: chi2 far above 21.67. If it is not, the probe cannot tell a locked
#                      phase from a random one and the random arm's result means nothing.
#   random             chi2 below 21.67 (uniform at p = 0.01, 9 degrees of freedom), with at least 90% of the pings
#                      placed. Its callback-after-send spread (p90 - p10) should be most of a poll cycle; the
#                      locked arm's should be a small part of one. (Seen on the first runs: the locked spread is not
#                      small - the reply lands near a check and is taken at the second or the third.)
#   jitter             the same conditions as random: one thread, a uniform pause of up to one cycle between the
#                      publish and the first check (added 2026-09-27, after the random arm passed).
#   locked + callback  --rtt-at callback: its RTT average should sit about one poll cycle (the sleep and its slack,
#                      ~150 us on this PC) below the locked arm's.
# Written before running. Exits 1 if either chi2 condition fails; the rest is printed for reading.
#
# Usage: [RMW=rmw_cyclonedds_cpp|rmw_fastrtps_cpp] rmw_phase_check.sh    Output: $OUT (default /tmp/rmw_phase_check.txt), "DONE" at the end.
set -u
OUT=${OUT:-/tmp/rmw_phase_check.txt}
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN="$REPO/install/rmw_perf_pingpong/lib/rmw_perf_pingpong"
NS1=ph-ns1
NS2=ph-ns2
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT
trap 'exit 130' INT TERM
cleanup
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add ph1 type veth peer name ph2 || exit 1
sudo -n ip link set ph1 netns "$NS1"
sudo -n ip link set ph2 netns "$NS2"
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev ph1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev ph2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set ph1 up
sudo -n ip -n "$NS2" link set ph2 up
RMW=${RMW:-rmw_tickle}
ENV=". /opt/ros/lyrical/setup.bash; . $REPO/install/setup.bash; export RMW_IMPLEMENTATION=$RMW TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_HOME=/tmp/ph_roshome"

run_arm() { # <name> <extra ping args...>
    local name=$1
    shift
    rm -f /tmp/ph_ping_done
    # The pong is stopped by its own shell, by the PID $! gives there - never by a name search, which would see
    # every pong_node on the machine: a netns does not separate PIDs.
    # shellcheck disable=SC2024 # the logs are this shell's
    sudo -n ip netns exec "$NS2" timeout 40 bash -c "$ENV; $BIN/pong_node -m bench & p=\$!; until [ -e /tmp/ph_ping_done ]; do sleep 0.05; done; kill -INT \$p; wait \$p" >/tmp/ph_pong.log 2>&1 &
    local pong=$!
    sleep 3
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" bash -c "$ENV; timeout -s INT 25 $BIN/ping_node -d 10 -i 0.02 --wait poll --poll-sleep-us 100 -m bench $*" >/tmp/ph_ping.log 2>&1
    touch /tmp/ph_ping_done
    wait "$pong"
    {
        echo "arm=$name $(grep -h '^RESULT:' /tmp/ph_ping.log | tr '\n' ' ')"
        echo "arm=$name $(grep -h '^LOOP:' /tmp/ph_ping.log | tr '\n' ' ')"
        echo "arm=$name $(grep -h '^PHASE:' /tmp/ph_ping.log | tr '\n' ' ')"
    } >>"$OUT"
}

: >"$OUT"
echo "rmw_phase_check $(date -Is) repo=$(git -C "$REPO" rev-parse --short=8 HEAD) rmw=$RMW" >>"$OUT"
run_arm locked --phase locked
run_arm random --phase random
run_arm jitter --phase jitter
run_arm locked_callback --phase locked --rtt-at callback

chi2() { grep "^arm=$1 PHASE:" "$OUT" | grep -o 'chi2=[0-9.]*' | cut -d= -f2; }
verdict=0
awk -v c="$(chi2 locked)" 'BEGIN { exit !(c != "" && c > 21.67) }' || { echo "CONTROL FAIL: locked chi2=$(chi2 locked)" >>"$OUT"; verdict=1; }
for arm in random jitter; do
    awk -v c="$(chi2 $arm)" 'BEGIN { exit !(c != "" && c < 21.67) }' || { echo "FAIL: $arm chi2=$(chi2 $arm)" >>"$OUT"; verdict=1; }
    placed=$(grep "^arm=$arm PHASE:" "$OUT" | grep -o 'placed=[0-9]*' | cut -d= -f2)
    recv=$(grep "^arm=$arm RESULT:" "$OUT" | grep -o 'recv=[0-9]*' | cut -d= -f2)
    awk -v p="${placed:-0}" -v r="${recv:-0}" 'BEGIN { exit !(r > 0 && p >= 0.9 * r) }' || { echo "FAIL: $arm placed=$placed of recv=$recv" >>"$OUT"; verdict=1; }
done
echo "VERDICT=$verdict" >>"$OUT"
echo "DONE" >>"$OUT"
exit "$verdict"
