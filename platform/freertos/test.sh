#!/bin/sh
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# Milestone 4 (see /home/semih/.claude/plans/lively-sauteeing-rainbow.md): builds a receiver
# (server, node 2) and a sender (client, node 1) image and runs both at once, joined to the same
# QEMU `-netdev socket,mcast=` group - the same "two independent guests on one broadcast segment"
# setup the two real Raspberry Pis simulate for the actual HIL performance workflow
# (.github/workflows/performance.yml), just emulated instead of on real hardware.
#
# Runs two such round trips: ping/pong (RPC, call/response) and publisher/subscriber (pub/sub).
# The latter specifically exercises tt_Publisher_publish()'s batched (not immediately flushed)
# send path, which ping/pong's always-immediately-flushed tt_Client_call() never reaches at all -
# see main_publisher.c's file-level comment.

set -u
cd "$(dirname "$0")"

MCAST_GROUP=230.0.0.1:5000
DURATION_S=${DURATION_S:-10}
MIN_COUNT=5

# Builds and runs one two-instance round trip: $server_role (background, for the whole test) and
# $client_role (foreground, time-bounded by $DURATION_S) joined to the same QEMU
# `-netdev socket,mcast=` group - $client_role is what actually drives the test, so once its
# `timeout` returns, there's nothing further to wait for.
#
# Neither QEMU process crashing nor $client_role's own process exiting cleanly actually proves a
# real round trip happened - $server_role could have silently failed to answer/receive anything
# and this would still look like a clean run by exit status alone. $client_grep/$server_grep
# require a real minimum number ($MIN_COUNT) of protocol-level log lines on each side instead of
# only checking that nothing crashed.
run_round_trip() {
    client_role=$1
    client_node=$2
    server_role=$3
    server_node=$4
    client_grep=$5
    server_grep=$6

    make ROLE=$server_role NODE_ID=$server_node all
    make ROLE=$client_role NODE_ID=$client_node all

    rm -f "$server_role.pcap" "$client_role.pcap" "$server_role.log" "$client_role.log"

    # stdin explicitly redirected from /dev/null on both: -nographic multiplexes the guest's
    # serial console AND the QEMU monitor over stdin, and a *backgrounded* process left attached
    # to the invoking terminal's stdin can get suspended by the shell's job control (SIGTTIN) the
    # moment it tries to read - at which point it's stopped, not running, and a later `kill`
    # (SIGTERM) can't actually terminate a stopped process, so `wait` below would block forever.
    qemu-system-riscv32 -machine virt -nographic -bios none -kernel "RTOSDemo-$server_role-$server_node.elf" \
        -global virtio-mmio.force-legacy=off \
        -netdev socket,id=net0,mcast=$MCAST_GROUP -device virtio-net-device,netdev=net0 \
        -object filter-dump,id=dump0,netdev=net0,file="$server_role.pcap" \
        </dev/null >"$server_role.log" 2>&1 &
    server_pid=$!

    # Give the background instance a moment to finish booting/negotiating its virtio-net link
    # before the foreground one starts sending - not load-bearing (a request during that window
    # would just time out and retry), but avoids a guaranteed-to-be-wasted first attempt.
    sleep 1

    timeout "$DURATION_S" qemu-system-riscv32 -machine virt -nographic -bios none \
        -kernel "RTOSDemo-$client_role-$client_node.elf" \
        -global virtio-mmio.force-legacy=off \
        -netdev socket,id=net1,mcast=$MCAST_GROUP -device virtio-net-device,netdev=net1 \
        -object filter-dump,id=dump1,netdev=net1,file="$client_role.pcap" \
        </dev/null >"$client_role.log" 2>&1
    client_status=$?

    kill "$server_pid" 2>/dev/null
    wait "$server_pid" 2>/dev/null

    echo "=== $server_role.log ==="
    cat "$server_role.log"
    echo "=== $client_role.log ==="
    cat "$client_role.log"

    # timeout's own exit code for "killed after the time limit" (124) is the expected, successful
    # outcome here - $client_role has no other way to stop on its own (see main_ping.c/
    # main_publisher.c: both are just infinite send/poll loops).
    if [ "$client_status" -ne 124 ] && [ "$client_status" -ne 0 ]; then
        echo "run_round_trip($client_role/$server_role): $client_role exited unexpectedly (status $client_status)"
        return 1
    fi

    client_seen=$(grep -c "$client_grep" "$client_role.log")
    server_seen=$(grep -c "$server_grep" "$server_role.log")
    echo "run_round_trip($client_role/$server_role): $server_role saw $server_seen, $client_role saw $client_seen"

    if [ "$client_seen" -lt "$MIN_COUNT" ] || [ "$server_seen" -lt "$MIN_COUNT" ]; then
        echo "run_round_trip($client_role/$server_role): fewer than $MIN_COUNT real round trips observed - treating as a failure"
        return 1
    fi
    return 0
}

status=0
run_round_trip ping 1 pong 2 'ping: seq=.*rtt=' 'pong: request seq=' || status=1
run_round_trip publisher 1 subscriber 2 'publisher: sent data=' 'subscriber: seq=' || status=1

exit $status
