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
# Runs the two sides in two network namespaces joined by a veth pair, each with its own distinct
# address (tickle-ns1 = 192.168.10.1, tickle-ns2 = 192.168.10.2). This needs root (creating a
# namespace/veth is CAP_NET_ADMIN, and entering one via `ip netns exec` needs it too) - the quick
# inner loop that doesn't is `make test` (unit tests, mock HAL). An earlier version of this script
# put both sides in one shared network namespace over loopback to avoid that, distinguished only
# by an -I node-id override - but a kernel delivers a *unicast* packet aimed at one wildcard-bound
# socket to whichever such socket bound last, not by any real address distinction, so that setup
# silently could not validate any of the unicast paths (a server's CallResponse, a Publisher/
# Client that has discovered a peer - see tt_UNICAST_PEER_THRESHOLD, the reactive discovery reply
# in process_announce()). Two genuinely distinct addresses have no such ambiguity, and node IDs come
# from tt_get_node_id()'s normal auto-detection (last octet of the address on the broadcast
# subnet) rather than an override, so that path gets exercised for real too.
#
# platform/linux/netns.mk still has manual createns/deletens/runX targets for poking at a single
# process inside a namespace interactively; this script manages its own namespaces (torn down on
# exit, set up fresh each run) and doesn't depend on those.
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
cd "$(dirname "$0")" || exit 1

MIN_COUNT=5
BROADCAST=192.168.10.255
NS1=tickle-ns1
NS2=tickle-ns2
VETH1=tickle-veth1
VETH2=tickle-veth2
NS1_ADDR=192.168.10.1
NS2_ADDR=192.168.10.2
PREFIX=24

# Probe the exact privilege this needs (sudo + ip), not a blanket `sudo -n true` - a scoped
# `NOPASSWD: /usr/sbin/ip` sudoers rule (which is all this wants) passes the former and not the
# latter.
if ! sudo -n ip netns list >/dev/null 2>&1; then
    if [ -t 0 ]; then
        echo "test-linux runs its two nodes in network namespaces and needs sudo - it will prompt."
    else
        echo "ERROR: test-linux needs passwordless sudo for 'ip' (it sets up network namespaces)." >&2
        echo "       e.g.  echo '$(id -un) ALL=(ALL) NOPASSWD: /usr/sbin/ip' | sudo tee /etc/sudoers.d/tickle-test" >&2
        echo "       The unit tests (make test) need no privilege." >&2
        exit 1
    fi
fi

teardown_ns() {
    sudo ip netns del "$NS1" 2>/dev/null || true
    sudo ip netns del "$NS2" 2>/dev/null || true
    # If a previous run died between creating the veth and moving it into a namespace, both ends
    # are still in the root namespace - deleting either end removes the pair.
    sudo ip link del "$VETH1" 2>/dev/null || true
}

setup_ns() {
    teardown_ns # idempotent: start from a clean slate even if a previous run left something behind

    sudo ip netns add "$NS1"
    sudo ip netns add "$NS2"
    sudo ip link add "$VETH1" type veth peer name "$VETH2"
    sudo ip link set "$VETH1" netns "$NS1"
    sudo ip link set "$VETH2" netns "$NS2"
    sudo ip -n "$NS1" addr add "$NS1_ADDR/$PREFIX" dev "$VETH1"
    sudo ip -n "$NS2" addr add "$NS2_ADDR/$PREFIX" dev "$VETH2"
    sudo ip -n "$NS1" link set "$VETH1" up
    sudo ip -n "$NS2" link set "$VETH2" up

    if ! sudo ip netns exec "$NS1" ping -c 1 -W 1 "$NS2_ADDR" >/dev/null 2>&1; then
        echo "ERROR: namespace setup failed - $NS1 ($NS1_ADDR) cannot reach $NS2 ($NS2_ADDR)" >&2
        exit 1
    fi
}

trap teardown_ns EXIT
setup_ns

# Launches $receiver in ns2 (background, bounded via $receiver_args - typically -d 15, a generous
# cap) then $sender in ns1 (foreground, bounded via $sender_args), waits for both, dumps their
# logs. Doesn't judge pass/fail itself - each call site below does that afterward with
# check_count/check_pass, however fits that pair. stdin redirected from /dev/null on both: neither
# binary reads it, but leaving a backgrounded process attached to the invoking terminal's stdin is
# a latent SIGTTIN/job-control hazard (see platform/freertos/test.sh's own fix for the concrete
# failure mode this avoids). No -I: each namespace's own distinct address auto-detects a distinct
# node ID the normal way (see this file's header comment).
run_pair() {
    sender=$1
    sender_args=$2
    receiver=$3
    receiver_args=$4

    rm -f "$receiver.log" "$sender.log"

    # receiver_args: deliberately unquoted, space-separated flag list. SC2024: the log redirect is the
    # calling (non-root) shell's, on purpose - keeps $receiver.log/$sender.log user-owned.
    # shellcheck disable=SC2086,SC2024
    sudo ip netns exec "$NS2" "./$receiver" -b "$BROADCAST" $receiver_args </dev/null >"$receiver.log" 2>&1 &
    receiver_pid=$!

    sleep 1

    # sender_args: deliberately unquoted, space-separated flag list. SC2024: the log redirect is the
    # calling (non-root) shell's, on purpose - keeps $receiver.log/$sender.log user-owned.
    # shellcheck disable=SC2086,SC2024
    sudo ip netns exec "$NS1" "./$sender" -b "$BROADCAST" $sender_args </dev/null >"$sender.log" 2>&1
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
# sufficient evidence of a real round trip. Because the two sides now have genuinely distinct
# addresses, this also actually exercises unicast delivery: once ping's Client has discovered
# pong's Server (via the periodic UPDATE announce / the reactive first-contact reply), its
# CallRequests go out unicast, and pong's CallResponses always do - a shared-loopback setup could
# not have told a working unicast path from a broken one here.
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
perf_result=$(grep '^RESULT:' perf_server.log | tail -1)
recv=$(printf '%s' "$perf_result" | sed -n 's/.*recv=\([0-9,]*\).*/\1/p' | tr -d ',')
recv=${recv:-0}
echo "perf_server received $recv message(s) (need >= $MIN_COUNT)"
[ "$recv" -ge "$MIN_COUNT" ] || status=1

# The liveness check above cannot fail for any reason short of total silence, and on 2026-09-29 that mattered: the
# shared-memory module's full-ring fallback reordered one logical stream across two paths, best-effort delivery
# discarded the older half, and this tier reported PASS through all of it - 97.6% loss and an 8x throughput collapse,
# from 1,017.9 Mbps to 122.7, green every time, for most of a day. "At least 5 messages" against a run that normally
# delivers 5.3 million is a check that cannot fail, which is the same defect class as a pattern that matches nothing.
# So the tier now also asserts what the run was supposed to achieve, not merely that it happened.
#
# WHAT THESE TWO NUMBERS ARE, stated plainly because a floor nobody can justify is the next version of this problem:
#   - PERF_MAX_LOSS_PCT=2.0. Both observed green runs lost exactly 0.0% (dropped=0), so any loss at all is already
#     outside what this test has ever done on a healthy build; 2.0 is slack for a loaded runner, not a budget.
#   - PERF_MIN_MBPS=400. This is deliberately loose - about 40% of the 1,017.9 Mbps last-green figure - because two
#     green data points give no variance estimate and a floor tuned on a guess would be flaky, which is how a
#     threshold gets raised until it cannot fail again. It is set to catch a COLLAPSE (it rejects both 122.7 and the
#     147.3 of the broken build) while tolerating a machine having a bad day. It should be tightened once several
#     green runs on the same runner give a real spread; until then, loose and honest beats tight and arbitrary.
# Both are overridable so a slower machine can run the suite without editing it, and an ABSENT field fails rather
# than passes: the fields are always printed by a current harness, so their absence means a stale binary or a
# changed RESULT line, and "the number I wanted to check was missing" must never read as "the check passed".
PERF_MAX_LOSS_PCT=${PERF_MAX_LOSS_PCT:-2.0}
PERF_MIN_MBPS=${PERF_MIN_MBPS:-400}
perf_loss=$(printf '%s' "$perf_result" | sed -n 's/.*loss_pct=\([0-9.]*\).*/\1/p')
perf_mbps=$(printf '%s' "$perf_result" | sed -n 's/.*avg_mbps=\([0-9,.]*\).*/\1/p' | tr -d ',')
if [ -z "$perf_loss" ] || [ -z "$perf_mbps" ]; then
    echo "perf_server: FAIL - RESULT line carries no loss_pct/avg_mbps, so neither can be checked: ${perf_result:-<no RESULT line>}"
    status=1
else
    echo "perf_server loss_pct=$perf_loss (need <= $PERF_MAX_LOSS_PCT), avg_mbps=$perf_mbps (need >= $PERF_MIN_MBPS)"
    awk -v l="$perf_loss" -v m="$PERF_MAX_LOSS_PCT" 'BEGIN { exit !(l <= m) }' || {
        echo "perf_server: FAIL - lost $perf_loss% of the stream (limit $PERF_MAX_LOSS_PCT%)"
        status=1
    }
    awk -v t="$perf_mbps" -v f="$PERF_MIN_MBPS" 'BEGIN { exit !(t >= f) }' || {
        echo "perf_server: FAIL - $perf_mbps Mbps is below the $PERF_MIN_MBPS Mbps floor"
        status=1
    }
fi
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
