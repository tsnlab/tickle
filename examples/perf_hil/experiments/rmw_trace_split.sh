#!/usr/bin/env bash
# One rmw_tickle ping-pong on a veth pair between two private netns, block mode, both processes dumping their
# latency stamps (RMW_PERF_PLAN.md 9, 2026-09-27); rmw_trace_split.py then splits the responder's receive-to-reply
# path by stage. PC-only: no rig lock.
#
# Needs this checkout's rmw_tickle built with the stamps compiled in:
#   colcon build --packages-select rmw_tickle --cmake-args -DBUILD_SHARED_LIBS=ON -DRMW_TICKLE_TRACE=ON
# and rmw_perf_pingpong installed beside it. The dumps are the identity check: a build without the stamps
# writes none, and the script says so rather than splitting nothing.
#
# Usage: GAP=0.005 DUR=5 [MSG=bench|array1k|struct16] rmw_trace_split.sh     Results: /tmp/rmw_trace_{pong,ping}.txt, /tmp/rmw_trace_stamps.txt
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN="$REPO/install/rmw_perf_pingpong/lib/rmw_perf_pingpong"
GAP=${GAP:-0.005}
DUR=${DUR:-5}
MSG=${MSG:-bench} # bench, array1k or struct16 (RMW_PERF_PLAN.md 11, 2026-09-27)
NS1=rt-ns1-$$
NS2=rt-ns2-$$
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT
cleanup
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add rt1 netns "$NS1" type veth peer name rt2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev rt1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev rt2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set rt1 up
sudo -n ip -n "$NS2" link set rt2 up

env=". /opt/ros/lyrical/setup.bash; . $REPO/install/setup.bash; export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_HOME=/tmp/rt_roshome"
started=$(date +%s)
rm -f /tmp/rt_ping_done
# The dumps are written by root (ip netns exec); a stale one from an earlier run is told apart by its mtime.
# shellcheck disable=SC2024 # the logs are this shell's
sudo -n ip netns exec "$NS2" timeout $((DUR + 15)) bash -c "$env; export RMW_TICKLE_TRACE_FILE=/tmp/rmw_trace_pong.txt; $BIN/pong_node -m $MSG & p=\$!; until [ -e /tmp/rt_ping_done ]; do sleep 0.05; done; kill -INT \$p; wait \$p" >/tmp/rmw_trace_pong.log 2>&1 &
pong=$!
sleep 4
# shellcheck disable=SC2024
sudo -n ip netns exec "$NS1" bash -c "$env; export RMW_TICKLE_TRACE_FILE=/tmp/rmw_trace_ping.txt; timeout -s INT $((DUR + 5)) $BIN/ping_node -d $DUR -i $GAP --wait block -m $MSG --stamps /tmp/rmw_trace_stamps.txt" >/tmp/rmw_trace_ping.log 2>&1
touch /tmp/rt_ping_done
wait "$pong"
for dump in /tmp/rmw_trace_pong.txt /tmp/rmw_trace_ping.txt; do
    if [ "$(stat -c %Y "$dump" 2>/dev/null || echo 0)" -lt "$started" ]; then
        echo "no fresh $dump: is rmw_tickle built with -DRMW_TICKLE_TRACE=ON, and is it the one loaded?" >&2
        exit 1
    fi
done
grep -h '^RESULT' /tmp/rmw_trace_ping.log
awk '!/^#/ && NF == 3 { print ($3 - $2) / 1000 }' /tmp/rmw_trace_stamps.txt | sort -n |
    awk '{ a[NR] = $1 } END { if (NR) printf "rtt n=%d median %.1f us\n", NR, a[int((NR + 1) / 2)] }'
python3 "$REPO/examples/perf_hil/experiments/rmw_trace_split.py" /tmp/rmw_trace_pong.txt
