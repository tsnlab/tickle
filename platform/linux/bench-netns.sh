#!/bin/sh
# Fast local perf/latency measurement across the two netns.mk namespaces (real distinct IPs), for
# iterating on internal optimizations. Shorter than test-netns.sh and only the two number-
# producing pairs. Needs `sudo make -C platform/linux createns` first. NOT a CI/functional gate -
# test-netns.sh is that; this just prints RTT + throughput so a change's effect is visible.

set -u
cd "$(dirname "$0")"

BROADCAST=192.168.10.255
NS_SENDER=ns1
NS_RECEIVER=ns2
PING_ARGS="${PING_ARGS:--d 15 -i 0.02 -w 3 -W 2}"
PERF_DURATION_S="${PERF_DURATION_S:-15}"
PERF_WARMUP_S="${PERF_WARMUP_S:-3}"
PERF_COOLDOWN_S="${PERF_COOLDOWN_S:-2}"

make -s ping_pong perf >/dev/null

run_pair() {
    sender=$1 sender_args=$2 receiver=$3 receiver_args=$4
    rm -f "$receiver.log" "$sender.log"
    # shellcheck disable=SC2086
    sudo ip netns exec "$NS_RECEIVER" "./$receiver" -b "$BROADCAST" $receiver_args </dev/null >"$receiver.log" 2>&1 &
    rpid=$!
    sleep 1
    # shellcheck disable=SC2086
    sudo ip netns exec "$NS_SENDER" "./$sender" -b "$BROADCAST" $sender_args </dev/null >"$sender.log" 2>&1
    wait "$rpid" 2>/dev/null
}

echo "== ping_pong =="
run_pair ping "$PING_ARGS" pong "-d 30"
grep -E '^RESULT:|packet loss' ping.log

echo "== perf =="
run_pair perf_client "-d $((PERF_DURATION_S + PERF_WARMUP_S + PERF_COOLDOWN_S))" \
    perf_server "-d $PERF_DURATION_S -w $PERF_WARMUP_S -W $PERF_COOLDOWN_S"
grep -E '^RESULT:' perf_client.log perf_server.log
