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
# Every TickLE example doubles as a functional/performance test of the library itself (see
# README's "Run examples"), so this runs all four pairs, in order:
#   - uint64 (publisher/subscriber): pub/sub - also the only pair that exercises
#     tt_Publisher_publish()'s batched (not immediately flushed) send path, which ping/pong's and
#     set_bool's always-immediately-flushed tt_Client_call() never reaches at all - see
#     main_publisher.c's file-level comment.
#   - set_bool (client/server): RPC call/response, the same shape as ping/pong but exercising a
#     different codec/service.
#   - ping_pong (ping/pong): RPC call/response, latency-flavored. Given a longer window
#     ($PING_DURATION_S, default 70s = 5s warm-up + 60s real data + 5s cool-down - see
#     main_ping.c's own WARMUP_S/COOLDOWN_S/TOTAL_DURATION_S comment) than the other two functional
#     pairs' $DURATION_S (default 10s), the same split platform/linux/test.sh's own ping pair uses.
#   - perf (perf_client/perf_server): pub/sub, throughput-flavored - fills a full Ethernet frame
#     and republishes as fast as tt_Node_poll() allows (see main_perf_client.c's own comment),
#     same as examples/linux/perf/perf_client.c's own defaults now - real max throughput, not a
#     fixed rate. Same 70s ($PERF_DURATION_S) 5+60+5 split as ping_pong above, for the same reason
#     (a short burst is a noisier throughput sample than a longer one) platform/linux/test.sh's own
#     perf pair follows.
# All four purpose-built role mains log one line per message/call (or, for perf, one aggregated
# line per second - see main_perf_client.c's own comment on why) on both sides (unlike the real
# example binaries platform/linux/test.sh reuses, which only do that on one side per pair - see
# its own comment), so every pair here uses the same two-sided count check.

set -u
cd "$(dirname "$0")" || exit 1

MCAST_GROUP=230.0.0.1:5000
DURATION_S=${DURATION_S:-10}
# 5s warm-up + 60s of real measured data + 5s cool-down = 70s, matching
# examples/freertos/ping_pong/main_ping.c's/main_perf_server.c's own WARMUP_S/COOLDOWN_S/
# TOTAL_DURATION_S constants exactly (see their own comments on why this can't just be a CLI
# flag here the way platform/linux/test.sh's -w/-W are) - and the same split
# platform/linux/test.sh's own ping/perf pairs use.
PING_DURATION_S=${PING_DURATION_S:-70}
PERF_DURATION_S=${PERF_DURATION_S:-70}
MIN_COUNT=5

# Builds and runs one two-instance round trip: $server_role (background, for the whole test) and
# $client_role (foreground, time-bounded by $duration, defaulting to $DURATION_S) joined to the
# same QEMU `-netdev socket,mcast=` group - $client_role is what actually drives the test, so
# once its `timeout` returns, there's nothing further to wait for.
#
# Neither QEMU process crashing nor $client_role's own process exiting cleanly actually proves a
# real round trip happened - $server_role could have silently failed to answer/receive anything
# and this would still look like a clean run by exit status alone. $client_grep/$server_grep
# require a real minimum number ($MIN_COUNT) of protocol-level log lines on each side instead of
# only checking that nothing crashed.
#
# Also appends one line to $SUMMARY per call, printed once at the very end (see this script's own
# tail) - $label (a plain pair name, e.g. "ping_pong") plus PASS/FAIL and the two-sided counts that
# decided it, so a run's overall pass/fail and every pair's evidence are visible without scrolling
# back through the full logs above.
SUMMARY=""
run_round_trip() {
    label=$1
    client_role=$2
    client_node=$3
    server_role=$4
    server_node=$5
    client_grep=$6
    server_grep=$7
    duration=${8:-$DURATION_S}

    make ROLE="$server_role" NODE_ID="$server_node" all
    make ROLE="$client_role" NODE_ID="$client_node" all

    rm -f "$server_role.log" "$client_role.log"

    # stdin explicitly redirected from /dev/null on both: -nographic multiplexes the guest's
    # serial console AND the QEMU monitor over stdin, and a *backgrounded* process left attached
    # to the invoking terminal's stdin can get suspended by the shell's job control (SIGTTIN) the
    # moment it tries to read - at which point it's stopped, not running, and a later `kill`
    # (SIGTERM) can't actually terminate a stopped process, so `wait` below would block forever.
    qemu-system-riscv32 -machine virt -nographic -bios none -kernel "RTOSDemo-$server_role-$server_node.elf" \
        -global virtio-mmio.force-legacy=off \
        -netdev socket,id=net0,mcast=$MCAST_GROUP -device virtio-net-device,netdev=net0 \
        </dev/null >"$server_role.log" 2>&1 &
    server_pid=$!

    # Give the background instance a moment to finish booting/negotiating its virtio-net link
    # before the foreground one starts sending - not load-bearing (a request during that window
    # would just time out and retry), but avoids a guaranteed-to-be-wasted first attempt.
    sleep 1

    timeout "$duration" qemu-system-riscv32 -machine virt -nographic -bios none \
        -kernel "RTOSDemo-$client_role-$client_node.elf" \
        -global virtio-mmio.force-legacy=off \
        -netdev socket,id=net1,mcast=$MCAST_GROUP -device virtio-net-device,netdev=net1 \
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
        SUMMARY="$SUMMARY
$(printf '%-10s FAIL (%s exited unexpectedly, status %s)' "$label" "$client_role" "$client_status")"
        return 1
    fi

    client_seen=$(grep -c "$client_grep" "$client_role.log")
    server_seen=$(grep -c "$server_grep" "$server_role.log")
    echo "run_round_trip($client_role/$server_role): $server_role saw $server_seen, $client_role saw $client_seen"

    if [ "$client_seen" -lt "$MIN_COUNT" ] || [ "$server_seen" -lt "$MIN_COUNT" ]; then
        echo "run_round_trip($client_role/$server_role): fewer than $MIN_COUNT real round trips observed - treating as a failure"
        SUMMARY="$SUMMARY
$(printf '%-10s FAIL (%s saw %s, %s saw %s)' "$label" "$server_role" "$server_seen" "$client_role" "$client_seen")"
        return 1
    fi
    SUMMARY="$SUMMARY
$(printf '%-10s PASS (%s saw %s, %s saw %s)' "$label" "$server_role" "$server_seen" "$client_role" "$client_seen")"
    return 0
}

status=0
run_round_trip uint64 publisher 1 subscriber 2 'publisher: sent data=' 'subscriber: seq=' || status=1
run_round_trip set_bool client 1 server 2 'client: call=.*success=' 'server: request data=' || status=1
run_round_trip ping_pong ping 1 pong 2 'ping: seq=.*time=' 'pong: request seq=' "$PING_DURATION_S" || status=1
# [1-9]: only count intervals with real activity (an aggregated "sent 0 msgs"/"recv 0 msgs" line
# existing proves nothing - see main_perf_client.c's report()) - unlike the other three pairs'
# per-event lines, which only ever appear when that event genuinely happened.
run_round_trip perf perf_client 1 perf_server 2 'perf_client: sent [1-9]' 'perf_server: recv [1-9]' \
    "$PERF_DURATION_S" || status=1

echo
echo "=== Summary ==="
if [ "$status" -eq 0 ]; then
    echo "Overall: PASS"
else
    echo "Overall: FAIL"
fi
printf '%s\n' "$SUMMARY"

exit $status
