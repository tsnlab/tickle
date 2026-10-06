#!/usr/bin/env bash
# copy_map_netns.sh - rmw_tickle/RMW_PERF_PLAN.md 11 M-a's shim half (2026-09-27): every memcpy/memmove of 64 B and up
# on an rmw_tickle ping-pong's per-sample path, per call site, for bench, array1k and struct16. Ping and pong run in
# two private netns (never the default netns), BEST_EFFORT, block wait, a round trip every 5 ms for 3 s, each with
# copy_count_shim.so preloaded. A site counts as per-sample when it ran at least half as many times as there were
# round trips; its bytes per round trip are printed with its function and line (addr2line).
#
# First the shim's own control: a 4096-B memcpy loop of 1000 iterations must read exactly 1000 calls, 4096000 B.
#
# Usage: copy_map_netns.sh    Output: $OUT (default /tmp/copy_map.txt), "COPY_MAP_DONE" at the end.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT=${OUT:-/tmp/copy_map.txt}
W="$(mktemp -d /tmp/copy_map_XXXXXX)"
BIN="$REPO/install/rmw_perf_pingpong/lib/rmw_perf_pingpong"
NS1=cm-ns1-$$
NS2=cm-ns2-$$
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    rm -rf "$W"
    return 0
}
trap cleanup EXIT
gcc -O2 -fno-builtin -fno-tree-loop-distribute-patterns -shared -fPIC -o "$W/shim.so" "$HERE/copy_count_shim.c" -ldl || exit 1
printf '#include <string.h>\nstatic char a[4096], b[4096];\nint main(void) { void* (*volatile mc)(void*, const void*, size_t) = memcpy; for (int i = 0; i < 1000; i++) mc(b, a, sizeof(b)); return b[0]; }\n' >"$W/control.c"
gcc -O2 -o "$W/control" "$W/control.c" || exit 1
: >"$OUT"
COPY_COUNT_OUT="$W/control.txt" LD_PRELOAD="$W/shim.so" "$W/control"
control=$(awk '{c += $4; b += $5} END {print c " " b}' "$W/control.txt")
echo "CONTROL calls_bytes=$control (expected 1000 4096000)" >>"$OUT"
[ "$control" = "1000 4096000" ] || { echo "CONTROL FAILED" >>"$OUT"; exit 1; }

sudo -n ip netns del "$NS1" 2>/dev/null
sudo -n ip netns del "$NS2" 2>/dev/null
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add cm1 netns "$NS1" type veth peer name cm2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev cm1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev cm2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set cm1 up
sudo -n ip -n "$NS2" link set cm2 up
ENV=". /opt/ros/lyrical/setup.bash; . $REPO/install/setup.bash; export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_HOME=/tmp/copy_map_roshome"

for msg in bench array1k struct16; do
    # shellcheck disable=SC2024 # the logs are this shell's
    sudo -n ip netns exec "$NS2" bash -c "$ENV; export COPY_COUNT_OUT=$W/pong_$msg.txt LD_PRELOAD=$W/shim.so; timeout -k 2 -s INT 9 $BIN/pong_node -m $msg" >"$W/pong_$msg.log" 2>&1 &
    pong=$!
    sleep 3
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" bash -c "$ENV; export COPY_COUNT_OUT=$W/ping_$msg.txt LD_PRELOAD=$W/shim.so; timeout -k 2 -s INT 6 $BIN/ping_node -d 3 -i 0.005 --wait block -m $msg" >"$W/ping_$msg.log" 2>&1
    wait "$pong"
    rtts=$(grep -oE 'recv=[0-9]+' "$W/ping_$msg.log" | head -1 | cut -d= -f2)
    echo "== $msg round_trips=${rtts:-0}" >>"$OUT"
    for side in ping pong; do
        [ -f "$W/${side}_$msg.txt" ] || { echo "  $side: no shim output" >>"$OUT"; continue; }
        while read -r _ obj off calls bytes; do
            [ "${rtts:-0}" -gt 0 ] && [ $((calls * 2)) -ge "$rtts" ] || continue
            where=$(addr2line -f -C -e "$obj" "$off" 2>/dev/null | tr '\n' ' ')
            awk -v side="$side" -v obj="$(basename "$obj")" -v calls="$calls" -v bytes="$bytes" -v rtts="$rtts" -v where="$where" \
                'BEGIN { printf "  %s %-28s calls/rt=%.2f bytes/rt=%.1f  %s\n", side, obj, calls / rtts, bytes / rtts, where }' >>"$OUT"
        done <"$W/${side}_$msg.txt"
    done
done
echo "COPY_MAP_DONE" >>"$OUT"
