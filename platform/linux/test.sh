#!/bin/sh
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# Automated round-trip test for the Linux HAL - the same "two independent nodes on one broadcast
# segment" shape as platform/freertos/test.sh's QEMU test, just exercising src/hal_linux.c's real
# kernel UDP sockets instead of hal_freertos.c's lwIP ones. Reuses the examples/linux/ binaries
# unmodified (not a purpose-built harness like examples/freertos/'s main_*.c) - real user-facing
# code, exercised the same way running it by hand would.
#
# Runs both sides as plain processes in the current network namespace, broadcasting over loopback
# (127.255.255.255) rather than the real veth-pair-between-two-namespaces setup platform/linux/
# netns.mk's createns/runX targets still offer for manual, closer-to-real-network testing. That
# needs root (creating a namespace/veth is CAP_NET_ADMIN); this doesn't need any privilege at all
# - broadcasting to a directed local address is a plain, unprivileged SO_BROADCAST send, the same
# one Linux already accepts on any real interface. The other half of the old setup - each side
# auto-detecting a distinct node ID from its own namespace's own IP - has no unprivileged
# replacement, since both sides now share the same loopback interface and would otherwise
# auto-detect the *same* ID and start silently dropping each other's packets as "self sent" (see
# process_packet() in tickle.c). -I gives each side an explicit, distinct ID instead (see
# _tt_CONFIG.node_id's own comment in config.h) - the only thing this setup relies on that a
# real separate-host/namespace deployment gets automatically from having a real distinct IP.
#
# Every TickLE example doubles as a functional/performance test of the library itself (see
# README's "Run examples"), so this runs all four pairs, in order:
#   - uint64 (publisher/subscriber): functional - does pub/sub actually deliver? Also the only
#     pair that exercises tt_Publisher_publish()'s batched (not immediately flushed) send path -
#     ping/pong's and set_bool's always-immediately-flushed tt_Client_call() never reaches it.
#   - set_bool (client/server): functional - does RPC call/response actually work?
#   - ping_pong (ping/pong): performance - round-trip latency.
#   - perf (perf_client/perf_server): performance - throughput.
# Functional pairs report their own PASS/FAIL (see subscriber.c/client.c's print_result()) - this
# script just checks for that verdict instead of reimplementing one. Performance pairs report
# numbers, not a verdict (see ping.c/perf_server.c's own comments on why) - this script instead
# requires real evidence of at least $MIN_COUNT genuine round trips, the same "a clean exit alone
# doesn't prove anything actually arrived" reasoning platform/freertos/test.sh's check applies.
#
# Fully self-contained, like platform/freertos/test.sh: platform/linux/Makefile builds the
# binaries this test runs right here in platform/linux/.

set -u
cd "$(dirname "$0")"

MIN_COUNT=5
BROADCAST=127.255.255.255

# Launches $receiver (background, bounded via $receiver_args - typically -d 15, a generous cap)
# then $sender (foreground, bounded via $sender_args), waits for both, and dumps their logs.
# Doesn't judge pass/fail itself - each call site below does that afterward with check_count/
# check_pass, however fits that pair. stdin redirected from /dev/null on both: neither binary
# reads it, but leaving a backgrounded process attached to the invoking terminal's stdin is a
# latent SIGTTIN/job-control hazard (see platform/freertos/test.sh's own fix for the concrete
# failure mode this avoids) - cheap to rule out here too. $sender is always node id 1, $receiver
# always node id 2 (see this file's own header comment on why an explicit id is needed here).
run_pair() {
    sender=$1
    sender_args=$2
    receiver=$3
    receiver_args=$4

    rm -f "$receiver.log" "$sender.log"

    # shellcheck disable=SC2086 - receiver_args is a deliberately unquoted, space-separated flag list
    "./$receiver" -b "$BROADCAST" -I 2 $receiver_args </dev/null >"$receiver.log" 2>&1 &
    receiver_pid=$!

    sleep 1

    # shellcheck disable=SC2086 - sender_args is a deliberately unquoted, space-separated flag list
    "./$sender" -b "$BROADCAST" -I 1 $sender_args </dev/null >"$sender.log" 2>&1
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

# Requires at least $MIN_COUNT lines matching $2 in file $1 - performance pairs' evidence of a
# real round trip (see run_pair's own comment).
check_count() {
    file=$1
    pattern=$2
    n=$(grep -c "$pattern" "$file")
    echo "check_count($file): $n matching '$pattern' (need >= $MIN_COUNT)"
    [ "$n" -ge "$MIN_COUNT" ]
}

# Requires file $1's own RESULT line to say PASS - functional pairs' evidence (see run_pair's own
# comment): the example already decided pass/fail for itself, this just reads the verdict.
check_pass() {
    file=$1
    if grep -q '^RESULT: PASS' "$file"; then
        echo "check_pass($file): PASS"
        return 0
    fi
    echo "check_pass($file): no RESULT: PASS line found"
    return 1
}

# Appends one line to $SUMMARY: <label> followed by file $2's own RESULT: line, verbatim - reused
# as-is rather than reformatted, so the final summary can't drift out of sync with what each
# example itself already decided to report. Printed once at the very end (see this script's own
# tail) so a run's overall pass/fail and every pair's key numbers are visible without scrolling
# back through the full logs above.
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

# pong.c never logs per-request - but a "seq=N time=X ms" line in ping's own log can only appear
# from a real decoded CallResponse, so that alone (checked against ping.log, the sender) is
# sufficient evidence of a real round trip.
#
# -d 60 -w 5 -W 5: 5s warm-up, 60s of real measured pinging, 5s cool-down (see ping.c's own
# comment on why -W extends past the stop trigger rather than reserving from a known -d). pong's
# own -d is generous past ping's full 5+60+5=70s span, the same margin-over-the-sender reasoning
# perf_server's own -d already uses below. check_count still matches every "(warmup)"/"(cooldown)"-
# tagged line too, not just the counted ones - it's evidence a round trip happened at all, not
# evidence about the stats window.
run_pair ping "-d 60 -i 0.2 -w 5 -W 5" pong "-d 80" || status=1
check_count ping.log '^seq=' || status=1
add_summary ping_pong ping.log

# perf_server.c has no per-message log line the way ping.c/subscriber.c do (only periodic
# aggregated interval reports and the final summary), so its own RESULT: recv=N tally is checked
# numerically instead of counting individual lines. No -s/-i: perf's whole purpose is measuring
# throughput, so this uses perf_client's own defaults (fills a full Ethernet frame, sends as fast
# as poll() allows), the same as running it by hand would - the FreeRTOS/QEMU pair does the same
# now (see main_perf_client.c's own comment).
#
# perf_server gets -w 5 -W 5 (perf_client doesn't - it's not the authoritative side, see its own
# comment): 5s warm-up, 60s of real measured throughput, 5s cool-down - the same split ping/pong
# above uses, and the same "-d means exactly the counted-data length" meaning perf_server.c's own
# -d has (see its own comment). Unlike ping.c (one process controlling both sending and counting),
# perf splits sender and counter across two processes - so for perf_server's full 5+60+5=70s
# window to ever see *real* traffic throughout, perf_client itself has to keep sending for the
# same 70s span, not just the 60s of it that ends up counted. (perf_server's own warm-up still
# anchors to its first *real* received message rather than its process start_time, so it starting
# a moment before perf_client doesn't eat into the warm-up budget - see first_recv_time's own
# comment - but that's a separate concern from perf_client's own send duration here.)
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
