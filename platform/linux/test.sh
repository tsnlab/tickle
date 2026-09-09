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
# (not a purpose-built harness like examples/freertos/'s main_*.c) - real user-facing code,
# exercised the same way `make runping`/`make runpong` would run it by hand.
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

# Launches $receiver (background, bounded via $receiver_args - typically -d 15, a generous cap)
# then $sender (foreground, bounded via $sender_args), waits for both, and dumps their logs.
# Doesn't judge pass/fail itself - each call site below does that afterward with check_count/
# check_pass, however fits that pair. stdin redirected from /dev/null on both: neither binary
# reads it, but leaving a backgrounded process attached to the invoking terminal's stdin is a
# latent SIGTTIN/job-control hazard (see platform/freertos/test.sh's own fix for the concrete
# failure mode this avoids) - cheap to rule out here too.
run_pair() {
    sender=$1
    sender_args=$2
    receiver=$3
    receiver_args=$4

    rm -f "$receiver.log" "$sender.log"

    # shellcheck disable=SC2086 - receiver_args is a deliberately unquoted, space-separated flag list
    sudo ip netns exec ns2 "./$receiver" $receiver_args </dev/null >"$receiver.log" 2>&1 &
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

# Idempotent: clear out any namespaces a previous (e.g. interrupted) run left behind before
# creating fresh ones - createns/deletens themselves aren't safe to call twice in a row.
sudo ip netns delete ns1 >/dev/null 2>&1
sudo ip netns delete ns2 >/dev/null 2>&1
make createns

make uint64
make set_bool
make ping_pong
make perf

status=0

run_pair publisher "-c 20 -i 0.2" subscriber "-d 15" || status=1
check_pass subscriber.log || status=1

run_pair client "-c 20 -i 0.2" server "-d 15" || status=1
check_pass client.log || status=1

# pong.c never logs per-request - but a "seq=N time=X ms" line in ping's own log can only appear
# from a real decoded CallResponse, so that alone (checked against ping.log, the sender) is
# sufficient evidence of a real round trip.
run_pair ping "-c 20 -i 0.2" pong "-d 15" || status=1
check_count ping.log '^seq=' || status=1

# perf_server.c has no per-message log line the way ping.c/subscriber.c do (only periodic
# aggregated interval reports and the final summary), so its own RESULT: recv=N tally is checked
# numerically instead of counting individual lines. No -s/-i: perf's whole purpose is measuring
# throughput, so this uses perf_client's own defaults (fills a full Ethernet frame, sends as fast
# as poll() allows), the same as running it by hand would - the FreeRTOS/QEMU pair does the same
# now (see main_perf_client.c's own comment).
#
# A longer window than the other three pairs (PERF_DURATION_S, default 20s vs. their fixed -c 20/
# ~4s): a short burst is a noisier throughput sample than a longer one, and unlike the other
# pairs' fixed message count, there's no equivalent "stop after N" for a rate this is meant to
# maximize. perf_server's own -d is a tight few seconds past perf_client's, not the generous
# +11s margin the other pairs' receivers use (pong/server/subscriber have no periodic output
# while idle, so a generous margin there is silent) - perf_server's report() task fires every
# second regardless of whether anything arrived, so a receiver bound as generous as the other
# pairs' would spend most of it printing pointless "recv 0 msgs ... 0.000 Mbps" lines after
# perf_client has already finished.
PERF_DURATION_S=${PERF_DURATION_S:-20}
run_pair perf_client "-d $PERF_DURATION_S" perf_server "-d $((PERF_DURATION_S + 3))" || status=1
recv=$(grep '^RESULT:' perf_server.log | tail -1 | sed -n 's/.*recv=\([0-9,]*\).*/\1/p' | tr -d ',')
recv=${recv:-0}
echo "perf_server received $recv message(s) (need >= $MIN_COUNT)"
[ "$recv" -ge "$MIN_COUNT" ] || status=1

make deletens

exit $status
