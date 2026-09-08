#!/bin/sh
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# Automated round-trip test for the Linux HAL, over two real network namespaces (see netns.mk) -
# the same "two independent nodes on one broadcast segment" shape as
# platform/freertos/run_pair.sh's QEMU test, just exercising src/hal_linux.c's real kernel UDP
# sockets instead of hal_freertos.c's lwIP ones. Reuses the example binaries unmodified (not a
# purpose-built harness like platform/freertos's main_ping.c/main_pong.c) - real user-facing code,
# exercised the same way `make runping`/`make runpong` would run it by hand.
#
# Runs two such round trips: ping/pong (RPC, call/response) and publisher/subscriber (pub/sub).
# The latter specifically exercises tt_Publisher_publish()'s batched (not immediately flushed)
# send path, which ping/pong's always-immediately-flushed tt_Client_call() never reaches at all -
# see platform/freertos/main_publisher.c's file-level comment (same reasoning, different HAL).

set -u
cd "$(dirname "$0")"

MIN_COUNT=5

# Runs one real round trip: $sender (foreground, bounded via $sender_args - typically -c/-i) on
# ns1, against $receiver (background, bounded via its own -d as a generous cap) on ns2.
#
# A clean exit on either side doesn't prove any packet actually arrived - $receiver_grep is
# checked against $receiver.log for real evidence of at least $MIN_COUNT genuinely decoded
# messages. $sender_grep is optional: pong.c/subscriber.c already log per-message evidence on the
# receiving side, which is enough on its own (see ping/pong's own asymmetric precedent below) -
# only pass it for a sender that also logs its own successes (ping.c does; publisher.c doesn't).
run_round_trip() {
    sender=$1
    sender_args=$2
    receiver=$3
    receiver_grep=$4
    sender_grep=${5:-}

    rm -f "$receiver.log" "$sender.log"

    # $receiver has no fixed end condition of its own (see pong.c/subscriber.c) - -d 15 bounds it
    # generously past however long $sender's own -c/-i bound their side to, so it always exits on
    # its own rather than needing a kill/wait dance like run_pair.sh's backgrounded QEMU instance
    # does. stdin redirected from /dev/null: neither binary reads it, but leaving a backgrounded
    # process attached to the invoking terminal's stdin is a latent SIGTTIN/job-control hazard
    # (see run_pair.sh's own fix for the concrete failure mode this avoids) - cheap to rule out.
    sudo ip netns exec ns2 "./$receiver" -d 15 </dev/null >"$receiver.log" 2>&1 &
    receiver_pid=$!

    sleep 1

    # shellcheck disable=SC2086 - sender_args is a deliberately unquoted, space-separated flag list
    sudo ip netns exec ns1 "./$sender" $sender_args </dev/null >"$sender.log" 2>&1
    sender_status=$?

    wait "$receiver_pid" 2>/dev/null

    echo "=== $receiver.log ==="
    cat "$receiver.log"
    echo "=== $sender.log ==="
    cat "$sender.log"

    if [ "$sender_status" -ne 0 ]; then
        echo "run_round_trip($sender/$receiver): $sender exited unexpectedly (status $sender_status)"
        return 1
    fi

    receiver_seen=$(grep -c "$receiver_grep" "$receiver.log")
    echo "run_round_trip($sender/$receiver): $receiver saw $receiver_seen real message(s)"
    if [ "$receiver_seen" -lt "$MIN_COUNT" ]; then
        echo "run_round_trip($sender/$receiver): fewer than $MIN_COUNT real messages observed - treating as a failure"
        return 1
    fi

    if [ -n "$sender_grep" ]; then
        sender_seen=$(grep -c "$sender_grep" "$sender.log")
        echo "run_round_trip($sender/$receiver): $sender saw $sender_seen real message(s)"
        if [ "$sender_seen" -lt "$MIN_COUNT" ]; then
            echo "run_round_trip($sender/$receiver): fewer than $MIN_COUNT real messages observed - treating as a failure"
            return 1
        fi
    fi
    return 0
}

# Idempotent: clear out any namespaces a previous (e.g. interrupted) run left behind before
# creating fresh ones - createns/deletens themselves aren't safe to call twice in a row.
sudo ip netns delete ns1 >/dev/null 2>&1
sudo ip netns delete ns2 >/dev/null 2>&1
make createns

make ping_pong
make uint64

status=0
# pong.c never logs per-request (unlike its FreeRTOS counterpart's main_pong.c, which does purely
# for this kind of check) - but a "seq=N time=X ms" line in ping's own log can only appear from a
# real decoded CallResponse, so that alone is sufficient evidence of a real round trip.
run_round_trip ping "-c 20 -i 0.2" pong '^seq=' 'pong: request seq=' || status=1
# publisher.c only logs on error, never on a successful publish - subscriber.c's callback already
# logs every message it decodes, which is sufficient evidence on its own (same asymmetric
# reasoning as the ping/pong case above, just with the logging on the other side).
run_round_trip publisher "-c 20 -i 0.2" subscriber '^  seq_no:' || status=1

make deletens

exit $status
