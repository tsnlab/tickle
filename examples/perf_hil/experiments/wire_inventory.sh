#!/usr/bin/env bash
# W0 of rmw_tickle/WIRE_PLAN.md (2026-09-26): what the native campaign's TickLE cells put on the wire, by kind.
# Each cell of campaign_sweep.sh's matrix is run once, TickLE only, over a veth pair between two private
# netns on this PC, with the same arguments, the same QoS (common_args) and the same netem on the client's
# egress. A capture is taken in the client's netns, capped at CAP_PACKETS so the throughput cells stay small,
# and wire_inventory.py splits it. This counts bytes, which a PC counts as exactly as the rig; it times
# nothing, and the throughput cells' HEARTBEAT/ACKNACK shares depend somewhat on timing, which the PC's
# faster link changes. Read those shares as indicative; the header and payload shares are exact.
#
# Usage: wire_inventory.sh [cell filter regex]   Output: $OUT (default /tmp/wire_inventory.txt), pcaps in $OUT.pcaps/
set -u
FILTER=${1:-.}
OUT=${OUT:-/tmp/wire_inventory.txt}
CAP_PACKETS=${CAP_PACKETS:-200000}
DUR=${DUR:-5}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
TK="$REPO/examples/perf_hil/tickle"
# SHA=<commit> (2026-09-27): build from a git worktree of that commit under its own HOME (so build.sh's install
# prefix cannot collide with another build), for WIRE_PLAN's bundle against its parent. Unset: this checkout.
BHOME=$HOME
if [ -n "${SHA:-}" ]; then
    SHA=$(git -C "$REPO" rev-parse "$SHA") || exit 1
    WT=/tmp/wirinv_wt_${SHA:0:8}; BHOME=/tmp/wirinv_home_${SHA:0:8}
    [ -d "$WT" ] || git -C "$REPO" worktree add -q --detach "$WT" "$SHA" || exit 1
    mkdir -p "$BHOME"
    TK="$WT/examples/perf_hil/tickle"
fi
NS1=wirinv-ns1
NS2=wirinv-ns2
MATRIX=(
    "T|p1|Q0|N0|reliable_throughput|-Q"
    "T|p2|Q0|N0|reliable_throughput|-Q"
    "T|p3|Q0|N0|reliable_throughput|-Q"
    "T|p4|Q0|N0|reliable_throughput|-Q"
    "T|p1|Q0|N1|reliable_throughput|-Q"
    "T|p4|Q0|N1|reliable_throughput|-Q"
    "T|p1|Q0|N3|reliable_throughput|-Q"
    "T|p1|Q1|N0|best_effort_throughput|"
    "T|p1|Q2|N0|reliable_throughput|"
    "L|p1|Q0|N0|reliable_latency|"
    "L|p2|Q0|N0|reliable_latency|"
    "L|p1|Q0|N2|reliable_latency|"
)
keepall() { case "$1" in p1) echo 2048 ;; p2) echo 405 ;; p3) echo 368 ;; p4) echo 187 ;; esac; }
common_args() {
    case "$1:$3" in
    reliable_throughput:Q0) echo "-N $(keepall "$2") -B 100" ;;
    reliable_throughput:Q2) echo "-K 64 -B 100" ;;
    esac
}
netem() {
    case "$1" in
    N1) echo "loss 5%" ;;
    N2) echo "delay 10ms 2ms" ;;
    N3) echo "delay 1ms reorder 5% 50%" ;;
    esac
}
kill_ns() { # every process in the netns, found by namespace membership (ip netns pids), not by name
    local p
    for p in $(sudo -n ip netns pids "$1" 2>/dev/null); do sudo -n ip netns exec "$1" kill -TERM "$p" 2>/dev/null; done
}
cleanup() {
    kill_ns "$NS1"; kill_ns "$NS2"
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT
say() { echo "$*" | tee -a "$OUT"; }

: > "$OUT"
mkdir -p "$OUT.pcaps"
say "=== wire inventory (WIRE_PLAN W0), $(date -Is), core ${SHA:-HEAD $(git -C "$REPO" rev-parse --short HEAD)}, -d $DUR, capture cap $CAP_PACKETS packets ==="
for cell in "${MATRIX[@]}"; do
    IFS='|' read -r shape payload qos net scenario extra <<< "$cell"
    name="${scenario}_${payload}_${qos}_${net}"
    [[ "$name" =~ $FILTER ]] || continue
    HOME=$BHOME "$TK/build.sh" "$scenario" "$payload" > /tmp/wire_inventory_build.log 2>&1 || { say "$name | VOID(build failed)"; continue; }
    dir="$TK/${scenario}_${payload}"
    args="-d $DUR"; [ "$shape" = L ] && args="-i 0.1 -d 10"
    args="$args $extra $(common_args "$scenario" "$payload" "$qos")"
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add wirinv1 type veth peer name wirinv2
    sudo -n ip link set wirinv1 netns "$NS1"
    sudo -n ip link set wirinv2 netns "$NS2"
    sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev wirinv1
    sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev wirinv2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set wirinv1 up
    sudo -n ip -n "$NS2" link set wirinv2 up
    ne=$(netem "$net")
    read -ra NE <<< "$ne"
    read -ra ARGS <<< "$args"
    [ -n "$ne" ] && { sudo -n ip netns exec "$NS1" tc qdisc add dev wirinv1 root netem "${NE[@]}" || exit 1; }
    pcap="$OUT.pcaps/$name.pcap"
    # shellcheck disable=SC2024 # logs are meant to be written as this user, not root
    sudo -n ip netns exec "$NS1" tcpdump -i wirinv1 -n -s 0 -c "$CAP_PACKETS" -U -w "$pcap" udp > /tmp/wire_inventory_tcpdump.log 2>&1 &
    sleep 1
    # shellcheck disable=SC2024 # as above
    (cd "$dir" && sudo -n ip netns exec "$NS2" ./server "${ARGS[@]}" > /tmp/wire_inventory_server.log 2>&1) &
    sleep 1
    res=$(cd "$dir" && sudo -n ip netns exec "$NS1" ./client "${ARGS[@]}" 2>/dev/null | grep '^RESULT:' | head -1)
    sleep 1
    kill_ns "$NS1"; kill_ns "$NS2"
    wait
    sudo -n chown "$(id -u):$(id -g)" "$pcap" 2>/dev/null || sudo -n ip netns exec "$NS1" chown "$(id -u):$(id -g)" "$pcap" 2>/dev/null
    verdict=ok; [ -n "$res" ] || verdict="VOID(no RESULT)"
    say "$name | $verdict | args: $args | netem: ${ne:-none} | ${res#RESULT: }"
    python3 "$HERE/wire_inventory.py" "$pcap" 2>&1 | sed 's/^/    /' | tee -a "$OUT" > /dev/null
done
say "=== done ==="
