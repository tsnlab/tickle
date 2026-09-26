#!/usr/bin/env bash
# recvmmsg on the receive path (hal_linux.c, tt_RX_BATCH): how many datagrams a batch really takes, and
# what it does to receive syscalls, kernel time, memory and latency - off the rig, on a veth pair, to size
# tt_RX_BATCH before Plan's rig run (CORE_HEADROOM.md: 1.066 receive syscalls a sample after 1af57e3e,
# against CycloneDDS's 0.74).
#
# ARMS - p1 reliable_throughput, RELIABLE + KEEP_ALL (-Q -N 2048 on both sides), no loss:
#   before  origin/main as it was (recvfrom), built from its own worktree and install prefix
#   b1      tt_RX_BATCH 1 - recvmmsg, one datagram a call: the same syscall pattern as before
#   b16     tt_RX_BATCH 16 (the default)
#   b64     tt_RX_BATCH 64
# plus reliable_latency p1, before vs b16, for the latency half of the kill criterion.
#
# HOW TO READ IT, written before running:
#   CONTROL: before and b1 must agree on receive syscalls per sample (within ~5%) - b1 is the same
#   pattern through a different call. If they do not, the counting is broken and nothing else here holds.
#   Receive syscalls per sample (server ppoll + recvfrom + recvmmsg, from the straced rep): prediction
#   b16 <= 0.8 x before, b64 no better than b16 by more than a few percent if 16 is enough.
#   Batch fill (unstraced reps): mean datagrams per recvmmsg and the share of calls that came back full.
#   A full share above ~10% at b16 says 16 binds, and b64's fill says how much further it would go.
#   Server stime_s per Msample falls with the syscall count, or batching is not buying kernel time and
#   the change is not worth keeping (Plan's kill criterion). Peak RSS rises by at most N x ~1.5 KB.
#   Throughput is expected unchanged; veth is not the rig, read direction.
#   Latency: b16's rtt_avg/rtt_max within noise of before's. A batch never waits for more datagrams
#   (MSG_DONTWAIT after ppoll), so a rise here would mean a pending datagram was held back.
#
# Usage: veth_rx_batch.sh <reps>     Results: $OUT (default /tmp/veth_rx_batch.txt)
set -u
REPS=${1:?usage: veth_rx_batch.sh <reps>}
OUT=${OUT:-/tmp/veth_rx_batch.txt}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PERF="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$PERF/../.." && pwd)"
ARMS_DIR=${ARMS_DIR:-/tmp/veth_rx_batch_arms}
BEFORE_WT=/tmp/veth_rx_batch_before_wt
NS1=rxb-ns1
NS2=rxb-ns2
CLIENT_LOG=/tmp/veth_rx_batch_client.log
SERVER_LOG=/tmp/veth_rx_batch_server.log

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

copy_arm() { # <name> <perf dir>
    mkdir -p "$ARMS_DIR/$1"
    for scenario in reliable_throughput_p1 reliable_latency_p1; do
        mkdir -p "$ARMS_DIR/$1/$scenario"
        cp "$2/tickle/$scenario/client" "$2/tickle/$scenario/server" "$ARMS_DIR/$1/$scenario/"
    done
}

build_arm() { # <name> <perf dir> <env...>
    local name=$1 perf=$2
    shift 2
    for scenario in reliable_throughput reliable_latency; do
        env "$@" "$perf/tickle/build.sh" "$scenario" p1 >>"$ARMS_DIR/$name.build.log" 2>&1 ||
            { echo "build $name $scenario failed - $ARMS_DIR/$name.build.log" >>"$OUT"; exit 1; }
    done
    copy_arm "$name" "$perf"
}

setup_ns() {
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add rxb1 type veth peer name rxb2 || exit 1
    sudo -n ip link set rxb1 netns "$NS1"
    sudo -n ip link set rxb2 netns "$NS2"
    sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev rxb1
    sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev rxb2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set rxb1 up
    sudo -n ip -n "$NS2" link set rxb2 up
}

run_throughput() { # <arm> <straced 0|1>
    local arm=$1 traced=$2 dir="$ARMS_DIR/$1/reliable_throughput_p1" trace=""
    setup_ns
    if [ "$traced" = 1 ]; then
        trace="strace -f -c -o /tmp/veth_rx_batch_strace_$arm.txt -e trace=ppoll,recvfrom,recvmmsg"
    fi
    # shellcheck disable=SC2024,SC2086 # the log is this shell's; $trace is word-split on purpose
    sudo -n ip netns exec "$NS2" $trace "$dir/server" -d 8 -N 2048 >"$SERVER_LOG" 2>&1 &
    local server_pid=$!
    sleep 2
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" "$dir/client" -d 3 -Q -N 2048 >"$CLIENT_LOG" 2>&1
    wait "$server_pid"
    local srv recv syscalls=na
    srv=$(grep -h "^RESULT" "$SERVER_LOG" | tr ' ' '\n' |
        grep -E "^(recv|lost|stime_s|utime_s|cpu_s_per_Msample|peak_rss_kb|rx_batch|rx_batch_calls|rx_batch_datagrams|rx_batch_full)=" |
        paste -sd' ')
    recv=$(echo "$srv" | grep -o "recv=[0-9]*" | cut -d= -f2)
    if [ "$traced" = 1 ] && [ -f "/tmp/veth_rx_batch_strace_$arm.txt" ] && [ -n "$recv" ] && [ "$recv" -gt 0 ]; then
        local calls
        # strace -c columns: % time, seconds, usecs/call, calls, [errors], syscall.
        calls=$(awk '$NF ~ /^(ppoll|recvfrom|recvmmsg)$/ { n += $4 } END { print n + 0 }' "/tmp/veth_rx_batch_strace_$arm.txt")
        syscalls="$(echo "scale=3; $calls / $recv" | bc) ($(awk '$NF ~ /^(ppoll|recvfrom|recvmmsg)$/ {printf "%s=%s ", $NF, $4}' "/tmp/veth_rx_batch_strace_$arm.txt"))"
    fi
    echo "kind=throughput arm=$arm straced=$traced $(grep -h '^RESULT' "$CLIENT_LOG" | tr ' ' '\n' | grep -E '^(sent|drained)=' | paste -sd' ') $srv rx_syscalls_per_sample=$syscalls" >>"$OUT"
}

run_latency() { # <arm>
    local arm=$1 dir="$ARMS_DIR/$1/reliable_latency_p1"
    setup_ns
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS2" "$dir/server" -d 8 >"$SERVER_LOG" 2>&1 &
    local server_pid=$!
    sleep 2
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" "$dir/client" -d 3 -i 0.001 >"$CLIENT_LOG" 2>&1
    wait "$server_pid"
    echo "kind=latency arm=$arm $(grep -h '^RESULT' "$CLIENT_LOG" | tr ' ' '\n' | grep -E '^(sent|recv|rtt_min_ms|rtt_avg_ms|rtt_max_ms)=' | paste -sd' ')" >>"$OUT"
}

mkdir -p "$ARMS_DIR"
: >"$OUT"
# The before arm: origin/main in a worktree, installed under its own HOME so its prefixes never meet ours.
git -C "$REPO" worktree remove --force "$BEFORE_WT" 2>/dev/null
git -C "$REPO" worktree add --detach "$BEFORE_WT" origin/main >>"$ARMS_DIR/before.build.log" 2>&1 || exit 1
mkdir -p /tmp/veth_rx_batch_home
build_arm before "$BEFORE_WT/examples/perf_hil" HOME=/tmp/veth_rx_batch_home
git -C "$REPO" worktree remove --force "$BEFORE_WT"
build_arm b1 "$PERF" TICKLE_RX_BATCH=1
build_arm b16 "$PERF" TICKLE_RX_BATCH=16
build_arm b64 "$PERF" TICKLE_RX_BATCH=64
echo "before=$(git -C "$REPO" rev-parse --short origin/main) after=working tree on $(git -C "$REPO" rev-parse --short HEAD)" >>"$OUT"

for _ in $(seq 1 "$REPS"); do
    for arm in before b1 b16 b64; do
        run_throughput "$arm" 0
    done
    for arm in before b16; do
        run_latency "$arm"
    done
done
for arm in before b1 b16 b64; do
    run_throughput "$arm" 1
done
echo "DONE" >>"$OUT"
