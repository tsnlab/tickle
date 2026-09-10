#!/bin/sh
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# Same four-pair round trip as test.sh, but run across the two real network namespaces
# platform/linux/netns.mk's createns target sets up (ns1=192.168.10.1, ns2=192.168.10.2, joined
# by a veth pair) instead of test.sh's shared-loopback/shared-wildcard-address setup. This exists
# specifically to check what test.sh's own comment on ping_pong/perf flags as unverifiable there:
# a kernel with multiple UDP sockets wildcard-bound to the *same* address delivers unicast to
# whichever bound last, not by any real address distinction, so test.sh's own loss/RTT numbers
# for anything that goes through tt_UNICAST_PEER_THRESHOLD's unicast path aren't a reliable
# verdict on delivery correctness there. Two genuinely distinct addresses don't have that
# ambiguity - confirmed separately via a minimal two-socket repro (127.0.0.2 vs 127.0.0.3) - so a
# clean pass here is real end-to-end evidence, not just unit-test-level confidence.
#
# Also drops test.sh's -I override entirely: each namespace has its own genuinely distinct
# address, so tt_get_node_id()'s normal auto-detection (last byte of whichever local address
# matches the broadcast subnet - see hal_linux.c) works unmodified, exercising that path for real
# instead of bypassing it. This is the one thing a real separate-host/namespace deployment gets
# for free that test.sh's shared loopback can't (see that file's own header comment).
#
# Needs: `sudo make -C platform/linux createns` already run (root/CAP_NET_ADMIN to create the
# namespace+veth - this script's own commands need root too, since entering a namespace via
# `ip netns exec` needs it regardless of who created it). Binaries are built the normal way first
# (`make examples` at the repo root, or let this script's own `make` lines below do it).

set -u
cd "$(dirname "$0")"

MIN_COUNT=5
BROADCAST=192.168.10.255
NS_SENDER=ns1   # 192.168.10.1
NS_RECEIVER=ns2 # 192.168.10.2

if ! sudo -n ip netns list 2>/dev/null | grep -q "^$NS_SENDER"; then
    echo "Namespace $NS_SENDER not found - run 'sudo make -C platform/linux createns' first." >&2
    exit 1
fi

# Same shape as test.sh's own run_pair(), except $receiver/$sender run inside their own real
# namespace (via sudo ip netns exec) instead of directly, and neither gets -I - see this file's
# own header comment on why that's the whole point here.
run_pair() {
    sender=$1
    sender_args=$2
    receiver=$3
    receiver_args=$4

    rm -f "$receiver.log" "$sender.log"

    # shellcheck disable=SC2086 - receiver_args is a deliberately unquoted, space-separated flag list
    sudo ip netns exec "$NS_RECEIVER" "./$receiver" -b "$BROADCAST" $receiver_args </dev/null >"$receiver.log" 2>&1 &
    receiver_pid=$!

    sleep 1

    # shellcheck disable=SC2086 - sender_args is a deliberately unquoted, space-separated flag list
    sudo ip netns exec "$NS_SENDER" "./$sender" -b "$BROADCAST" $sender_args </dev/null >"$sender.log" 2>&1
    sender_status=$?

    wait "$receiver_pid" 2>/dev/null

    echo "=== $receiver.log ==="
    cat "$receiver.log"
    echo "=== $sender.log ==="
    cat "$sender.log"

    if [ "$sender_status" -ne 0 ]; then
        echo "run_pair($sender/$receiver): $sender exited unexpectedly (status $sender_status)"
        return 1
    fi
    return 0
}

check_count() {
    file=$1
    pattern=$2
    n=$(grep -c "$pattern" "$file")
    echo "check_count($file): $n matching '$pattern' (need >= $MIN_COUNT)"
    [ "$n" -ge "$MIN_COUNT" ]
}

check_pass() {
    file=$1
    if grep -q '^RESULT: PASS' "$file"; then
        echo "check_pass($file): PASS"
        return 0
    fi
    echo "check_pass($file): no RESULT: PASS line found"
    return 1
}

SUMMARY=""
add_summary() {
    label=$1
    file=$2
    result_line=$(grep '^RESULT:' "$file" | tail -1)
    SUMMARY="$SUMMARY
$(printf '%-10s %s' "$label" "$result_line")"
}

make uint64
make set_bool
make ping_pong
make perf

status=0

run_pair publisher "-c 20 -i 0.2" subscriber "-d 15" || status=1
check_pass subscriber.log || status=1
add_summary uint64 subscriber.log

run_pair client "-c 20 -i 0.2" server "-d 15" || status=1
check_pass client.log || status=1
add_summary set_bool client.log

run_pair ping "-d 60 -i 0.2 -w 5 -W 5" pong "-d 80" || status=1
check_count ping.log '^seq=' || status=1
add_summary ping_pong ping.log

PERF_WARMUP_S=5
PERF_COOLDOWN_S=5
PERF_DURATION_S=${PERF_DURATION_S:-60}
run_pair perf_client "-d $((PERF_DURATION_S + PERF_WARMUP_S + PERF_COOLDOWN_S))" \
    perf_server "-d $PERF_DURATION_S -w $PERF_WARMUP_S -W $PERF_COOLDOWN_S" || status=1
recv=$(grep '^RESULT:' perf_server.log | tail -1 | sed -n 's/.*recv=\([0-9,]*\).*/\1/p' | tr -d ',')
recv=${recv:-0}
echo "perf_server received $recv message(s) (need >= $MIN_COUNT)"
[ "$recv" -ge "$MIN_COUNT" ] || status=1
add_summary perf perf_server.log

echo
echo "=== Summary ==="
if [ "$status" -eq 0 ]; then
    echo "Overall: PASS"
else
    echo "Overall: FAIL"
fi
printf '%s\n' "$SUMMARY"

exit $status
