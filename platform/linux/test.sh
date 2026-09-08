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
# platform/freertos/test.sh's QEMU test, just exercising src/hal_linux.c's real kernel UDP
# sockets instead of hal_freertos.c's lwIP ones. Reuses the examples/linux/ binaries unmodified
# (not a purpose-built harness like examples/freertos/'s main_ping.c/main_pong.c) - real
# user-facing code, exercised the same way `make runping`/`make runpong` would run it by hand.
#
# Runs two such round trips: ping/pong (RPC, call/response) and publisher/subscriber (pub/sub).
# The latter specifically exercises tt_Publisher_publish()'s batched (not immediately flushed)
# send path, which ping/pong's always-immediately-flushed tt_Client_call() never reaches at all -
# see examples/freertos/uint64/main_publisher.c's file-level comment (same reasoning, different HAL).
#
# Unlike platform/freertos/test.sh (fully self-contained: its own Makefile builds and its own
# QEMU-launched ELFs run entirely within platform/freertos/), the binaries this test runs are
# built by the top-level Makefile and land at the repo root - so this script reaches up to ../..
# for both the build and the built binaries, while still keeping its own working files (logs)
# local to this directory.

set -u
cd "$(dirname "$0")"

ROOT=../..
MIN_COUNT=5

# Runs one real round trip: $sender (foreground, bounded via $sender_args - typically -c/-i) on
# ns1, against $receiver (background, bounded via its own -d as a generous cap) on ns2.
#
# A clean exit on either side doesn't prove any packet actually arrived - but unlike
# platform/freertos/test.sh's QEMU pairs (where both purpose-built role mains log per-message on
# both sides), these are the real, unmodified example binaries, and only one side of each pair
# actually logs real evidence: ping.c logs every reply it receives, but pong.c never logs
# per-request; subscriber.c's callback logs every message it decodes, but publisher.c only logs
# on error. So exactly one of $sender_grep/$receiver_grep is expected to be non-empty per call -
# whichever side's log this pair's real evidence lives in - and only that one is checked against
# at least $MIN_COUNT matches. (Passing both, or neither, would also work mechanically, just
# isn't needed today.)
run_round_trip() {
    sender=$1
    sender_args=$2
    receiver=$3
    sender_grep=$4
    receiver_grep=$5

    rm -f "$receiver.log" "$sender.log"

    # $receiver has no fixed end condition of its own (see pong.c/subscriber.c) - -d 15 bounds it
    # generously past however long $sender's own -c/-i bound their side to, so it always exits on
    # its own rather than needing a kill/wait dance like platform/freertos/test.sh's backgrounded
    # QEMU instance does. stdin redirected from /dev/null: neither binary reads it, but leaving a
    # backgrounded process attached to the invoking terminal's stdin is a latent SIGTTIN/
    # job-control hazard (see platform/freertos/test.sh's own fix for the concrete failure mode
    # this avoids) - cheap to rule out.
    sudo ip netns exec ns2 "$ROOT/$receiver" -d 15 </dev/null >"$receiver.log" 2>&1 &
    receiver_pid=$!

    sleep 1

    # shellcheck disable=SC2086 - sender_args is a deliberately unquoted, space-separated flag list
    sudo ip netns exec ns1 "$ROOT/$sender" $sender_args </dev/null >"$sender.log" 2>&1
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

    ok=1
    if [ -n "$sender_grep" ]; then
        sender_seen=$(grep -c "$sender_grep" "$sender.log")
        echo "run_round_trip($sender/$receiver): $sender saw $sender_seen real message(s)"
        [ "$sender_seen" -ge "$MIN_COUNT" ] || ok=0
    fi
    if [ -n "$receiver_grep" ]; then
        receiver_seen=$(grep -c "$receiver_grep" "$receiver.log")
        echo "run_round_trip($sender/$receiver): $receiver saw $receiver_seen real message(s)"
        [ "$receiver_seen" -ge "$MIN_COUNT" ] || ok=0
    fi
    if [ "$ok" -ne 1 ]; then
        echo "run_round_trip($sender/$receiver): fewer than $MIN_COUNT real messages observed - treating as a failure"
        return 1
    fi
    return 0
}

# Idempotent: clear out any namespaces a previous (e.g. interrupted) run left behind before
# creating fresh ones - createns/deletens themselves aren't safe to call twice in a row.
sudo ip netns delete ns1 >/dev/null 2>&1
sudo ip netns delete ns2 >/dev/null 2>&1
make -C "$ROOT" createns

make -C "$ROOT" ping_pong
make -C "$ROOT" uint64

status=0
# pong.c never logs per-request - but a "seq=N time=X ms" line in ping's own log can only appear
# from a real decoded CallResponse, so that alone (checked against ping.log, the sender) is
# sufficient evidence of a real round trip.
run_round_trip ping "-c 20 -i 0.2" pong '^seq=' '' || status=1
# The mirror image of the ping/pong case: publisher.c only logs on error, never on a successful
# publish, but subscriber.c's callback logs every message it decodes - checked against
# subscriber.log, the receiver this time.
run_round_trip publisher "-c 20 -i 0.2" subscriber '' '  seq_no:' || status=1

make -C "$ROOT" deletens

exit $status
