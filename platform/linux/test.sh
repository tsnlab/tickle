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
# address (tickle-ns1-<pid> = <net>.1, tickle-ns2-<pid> = <net>.2, a subnet claimed per run - see
# RUN_ID below). This needs root (creating a namespace/veth is CAP_NET_ADMIN, and entering one via
# `ip netns exec` needs it too) - the quick inner loop that doesn't is `make test` (unit tests, mock HAL). An earlier version of this script
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

# Several sessions run this suite on one PC at the same time. On 2026-10-06 two concurrent runs failed and passed
# on rerun: both used the fixed namespaces tickle-ns1/tickle-ns2, and each run's setup/teardown deleted the other's
# mid-run. So everything this run creates in a shared place is its own:
#   - Namespaces are suffixed with this shell's PID, unique among live processes, and teardown deletes only those.
#   - The veth pair is created directly inside the two namespaces, so its names never exist in the root namespace
#     and cannot collide with anyone's; deleting a namespace deletes its end, so there is nothing else to tidy.
#   - The subnet is claimed, not fixed. Namespaces isolate addresses but NOT /dev/shm, and the shared-memory
#     segments and the context-id registry are named by address (src/tickle.c segment_name(), src/hal_linux.c
#     registry_path()). Two runs on 192.168.10.0/24 would therefore attach to each other's segments. A run takes
#     10.<128+k/256>.<k%256>.0/24 for the first k, starting at PID mod 32768, whose claim directory it can mkdir
#     (atomic), and removes the claim on exit. A run killed with SIGKILL leaves its claim behind; the next run
#     skips it, and /tmp is cleared at boot. 10.128.0.0/9 stays clear of the rig's 10.1.1.x management LAN.
#   - Two runs in the SAME checkout would still share the binaries and *.log files in this directory, which CI
#     uploads by those names, so they are serialized by a lock keyed on this directory instead.
RUN_ID=$$
NS1=tickle-ns1-$RUN_ID
NS2=tickle-ns2-$RUN_ID
VETH1=tickle-veth1
VETH2=tickle-veth2
PREFIX=24
CLAIM_ROOT=${TMPDIR:-/tmp}/tickle-test-linux
CLAIM=""

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

# Only this run's own names: a namespace another run is using is never touched.
# shellcheck disable=SC2329  # invoked by the EXIT trap below
teardown_ns() {
    sudo ip netns del "$NS1" 2>/dev/null || true
    sudo ip netns del "$NS2" 2>/dev/null || true
    [ -n "$CLAIM" ] && rm -rf "$CLAIM"
    return 0
}

claim_subnet() {
    mkdir -p "$CLAIM_ROOT" || exit 1
    k=$((RUN_ID % 32768))
    tries=0
    while ! mkdir "$CLAIM_ROOT/slot-$k" 2>/dev/null; do
        tries=$((tries + 1))
        if [ "$tries" -ge 256 ]; then
            echo "ERROR: no free test subnet - 256 claims in $CLAIM_ROOT are taken (stale ones are safe to delete)" >&2
            exit 1
        fi
        k=$(((k + 1) % 32768))
    done
    CLAIM="$CLAIM_ROOT/slot-$k"
    echo "$RUN_ID" >"$CLAIM/pid"
    NET=10.$((128 + k / 256)).$((k % 256))
    BROADCAST=$NET.255
    NS1_ADDR=$NET.1
    NS2_ADDR=$NET.2
}

setup_ns() {
    # Leftovers of an earlier run can only carry this name if it had this PID, so this cannot hit a live run.
    sudo ip netns del "$NS1" 2>/dev/null || true
    sudo ip netns del "$NS2" 2>/dev/null || true

    sudo ip netns add "$NS1"
    sudo ip netns add "$NS2"
    sudo ip link add "$VETH1" netns "$NS1" type veth peer name "$VETH2" netns "$NS2"
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
# The EXIT trap does not run on a signal death in every sh; turning the signal into an exit makes it run.
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# One run per checkout at a time (see the comment at RUN_ID). Held on fd 9 until this shell exits.
LOCK_FILE=${TMPDIR:-/tmp}/tickle-test-linux-$(pwd | cksum | cut -d' ' -f1).lock
exec 9>"$LOCK_FILE"
if ! flock -n 9; then
    echo "test-linux: another run is using this checkout ($(pwd)) - waiting for it (up to 15 min)"
    flock -w 900 9 || {
        echo "ERROR: still locked after 15 min: $LOCK_FILE" >&2
        exit 1
    }
fi

claim_subnet
setup_ns
echo "test-linux: run $RUN_ID - namespaces $NS1/$NS2, subnet $NET.0/$PREFIX"

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
# A second delivery check, because the first one has a hole. On 2026-09-30 a run reported loss_pct=0.0 while the
# publisher's own counters said 54,328 of 10,583,484 samples - 0.513% - had been refused by a full shared-memory ring
# and, under drop-on-full, never sent at all. The accounting closes to 23 samples in flight
# (tx_shm + tx_udp + shm_full_dropped = sent), so those samples are really gone; the subscriber's loss figure did not
# see them because it is computed over its own counted window while the counter covers the whole process. A delivery
# loss the delivery check cannot see is the same defect as "need >= 5", one level in - so the ring's own refusals are
# now checked against the same limit, and printed on every run whether or not they trip it.
PERF_MAX_LOSS_PCT=${PERF_MAX_LOSS_PCT:-2.0}
PERF_MIN_MBPS=${PERF_MIN_MBPS:-400}
perf_loss=$(printf '%s' "$perf_result" | sed -n 's/.*loss_pct=\([0-9.]*\).*/\1/p')
perf_mbps=$(printf '%s' "$perf_result" | sed -n 's/.*avg_mbps=\([0-9,.]*\).*/\1/p' | tr -d ',')
if [ -z "$perf_loss" ] || [ -z "$perf_mbps" ]; then
    echo "perf_server: FAIL - RESULT line carries no loss_pct/avg_mbps, so neither can be checked: ${perf_result:-<no RESULT line>}"
    status=1
else
    echo "perf_server loss_pct=$perf_loss (limit $PERF_MAX_LOSS_PCT applies to the kernel path), avg_mbps=$perf_mbps (need >= $PERF_MIN_MBPS)"
    # The loss limit binds the KERNEL path and not the shared-memory one, and the asymmetry is deliberate.
    #
    # A best-effort ring that drops when full is the shared-memory module's documented behaviour and the correct
    # repair for the reordering that rerouting past a full ring caused (97.4% of delivery, 2026-09-29). On
    # 7eaad571 the module-on arm sent 53,405,295 samples, had 5,017,606 refused by a full ring, and delivered 8.6%
    # fewer at +117% the throughput of the module-off arm, which lost none. That is a trade, not a defect.
    #
    # Bounding it at any number would be choosing what the module promises, which is the user's decision 6 and not
    # a test's. Raising the limit to admit today's 8.6% would be `need >= 5` with decimal places: a limit picked to
    # stop failing. So the module-on arm's loss is REPORTED and the kernel path's is BOUNDED - the path that
    # promises not to lose must not lose, and the path that documents dropping is measured rather than judged.
    #
    # The gap this leaves, stated rather than hidden: a module-on arm losing 90% would not fail here. Closing it
    # needs the two arms compared in one place - both steps writing their figures out and a third comparing them,
    # so loss is admissible only when it buys throughput the off arm does not reach. That is the proper version and
    # it belongs in the workflow, not in a script that runs once per arm and cannot see the other.
    # Discriminate on what the run DID, not on whether a field exists. The first version of this grepped for
    # `shm_full_dropped=`, which the traffic lines print as 0 even when the module is compiled out - so it matched the
    # kernel arm too and quietly removed the bound from the one path that must keep it. Caught on 32b43c15, where the
    # module-off arm printed "reported, not bounded" about a kernel path. `tx_shm` above zero is the outcome test:
    # samples actually went through a segment in this run.
    perf_tx_shm=$(grep -oE 'tx_shm=[0-9]+' perf_client.log 2>/dev/null | tail -1 | cut -d= -f2)
    if [ -n "${perf_tx_shm:-}" ] && [ "$perf_tx_shm" -gt 0 ] 2>/dev/null; then
        echo "perf_server: loss on the shared-memory path is reported, not bounded - see the shm_full_dropped line"
        echo "  (a full ring dropping is documented behaviour; what it should be allowed to cost is decision 6)"
    else
        awk -v l="$perf_loss" -v m="$PERF_MAX_LOSS_PCT" 'BEGIN { exit !(l <= m) }' || {
            echo "perf_server: FAIL - the kernel path lost $perf_loss% of the stream (limit $PERF_MAX_LOSS_PCT%)"
            status=1
        }
    fi
    awk -v t="$perf_mbps" -v f="$PERF_MIN_MBPS" 'BEGIN { exit !(t >= f) }' || {
        echo "perf_server: FAIL - $perf_mbps Mbps is below the $PERF_MIN_MBPS Mbps floor"
        status=1
    }
fi

# The shared-memory ring's own refusals, from the publisher's line. Absent when the module is compiled out, which is
# not a failure - the field only exists when there is a ring to refuse.
perf_client_result=$(grep '^RESULT:' perf_client.log 2>/dev/null | tail -1)
perf_full_dropped=$(printf '%s' "$perf_client_result" | sed -n 's/.*shm_full_dropped=\([0-9,]*\).*/\1/p' | tr -d ',')
perf_sent=$(printf '%s' "$perf_client_result" | sed -n 's/.*sent=\([0-9,]*\).*/\1/p' | tr -d ',')
if [ -n "$perf_full_dropped" ] && [ -n "$perf_sent" ] && [ "$perf_sent" -gt 0 ]; then
    perf_drop_pct=$(awk -v d="$perf_full_dropped" -v s="$perf_sent" 'BEGIN { printf "%.3f", 100 * d / s }')
    echo "perf_client shm_full_dropped=$perf_full_dropped of sent=$perf_sent ($perf_drop_pct%, need <= $PERF_MAX_LOSS_PCT)"
    awk -v d="$perf_drop_pct" -v m="$PERF_MAX_LOSS_PCT" 'BEGIN { exit !(d <= m) }' || {
        echo "perf_client: NOTE - a full shared-memory ring refused $perf_drop_pct% of the stream, above the $PERF_MAX_LOSS_PCT% the kernel path is held to."
        echo "  Those samples were never sent, and the subscriber's loss figure may read 0.0 and still be right about"
        echo "  what it saw, because it counts a window and this counts the whole run."
        echo "  Not a failure: what a documented drop-on-full policy should be allowed to cost is decision 6."
    }
fi
add_summary perf perf_server.log

# The DEFAULT broadcast, 255.255.255.255 - rmw_tickle's, unless TICKLE_BROADCAST_ADDR is set - which no interface
# owns. Every pair above names a directed broadcast, and so did every rig harness, which is how a context on the
# default never learning its own address went unseen: it then treated no peer as same-host, built no segment, and two
# processes on one host never used shared memory at all (found 2026-10-06; tests/test_own_address.c). Two steps, one
# each way, so neither can pass by the other's mechanism:
#   - same host: perf_client and perf_server both in $NS1. The publisher must send MOST of the stream over shared
#     memory (tx_shm > tx_udp), and the server must have counted a same-host peer. Not tx_shm > 0: the first version
#     of this step passed on 13 datagrams of 539,152, with the stream itself broadcast over UDP.
#   - two hosts: perf_client in $NS1, perf_server in $NS2, on the same default. Traffic must arrive (or the next
#     check is about nothing), and neither side may count a same-host peer or send over shared memory - the two
#     namespaces share /dev/shm, so a context that took a peer on another address for its own host would find out
#     only by its traffic vanishing into a ring nobody reads.
# The limited broadcast leaves by the default route, so each namespace gets one: without it sendto() has nowhere to
# go. And loopback comes up, as on any real host: a unicast between two processes on one address is delivered through
# it, and with it down the two never learn each other - on a directed broadcast too (checked 2026-10-06: 16 of 667,558
# datagrams over shared memory either way), so the same-host step would measure this script's namespace and not the
# product. Both added after every pair above, which never needed them.
sudo ip -n "$NS1" link set lo up
sudo ip -n "$NS2" link set lo up
sudo ip -n "$NS1" route add default dev "$VETH1"
sudo ip -n "$NS2" route add default dev "$VETH2"
DEFAULT_BCAST=255.255.255.255
field() { # field <name> <file>: the last value printed for <name>=, digits only, or nothing
    grep -oE "$1=[0-9,]+" "$2" 2>/dev/null | tail -1 | cut -d= -f2 | tr -d ','
}

rm -f samehost_server.log samehost_client.log
# shellcheck disable=SC2024 # the redirect is the calling shell's, on purpose (user-owned logs)
sudo ip netns exec "$NS1" ./perf_server -b "$DEFAULT_BCAST" -d 4 </dev/null >samehost_server.log 2>&1 &
samehost_pid=$!
sleep 1
# shellcheck disable=SC2024
sudo ip netns exec "$NS1" ./perf_client -b "$DEFAULT_BCAST" -d 4 </dev/null >samehost_client.log 2>&1
wait "$samehost_pid" 2>/dev/null
# From the RESULT line, which counts the whole process; absent reads as 0 and fails.
samehost_result=$(grep '^RESULT:' samehost_client.log | tail -1)
tx_shm=$(printf '%s' "$samehost_result" | sed -n 's/.*tx_shm=\([0-9]*\).*/\1/p')
tx_udp=$(printf '%s' "$samehost_result" | sed -n 's/.*tx_udp=\([0-9]*\).*/\1/p')
same_host=$(field shm_same_host_peers samehost_server.log)
echo "default broadcast, one host: tx_shm=${tx_shm:-<absent>} tx_udp=${tx_udp:-<absent>}" \
    "server shm_same_host_peers=${same_host:-<absent>}"
# The module-off control (.github/scripts/test_linux_module_off.sh runs this with CPPFLAGS=-Dtt_SEGMENT_ENABLED=0,
# which make exports to this script) has no segment to use: there the same step must show the stream on UDP and no
# segment at all, the inverse of the module-on expectation - not be skipped.
case " ${CPPFLAGS:-} " in *"-Dtt_SEGMENT_ENABLED=0"*) module_off=1 ;; *) module_off=0 ;; esac
if [ "$module_off" = 1 ]; then
    if [ "${tx_shm:-1}" -eq 0 ] && [ "${tx_udp:-0}" -gt 0 ]; then
        echo "default broadcast, one host: PASS - module compiled out, the stream went over UDP"
    else
        echo "default broadcast, one host: FAIL - module compiled out, yet tx_shm=${tx_shm:-<absent>} tx_udp=${tx_udp:-<absent>}"
        status=1
    fi
elif [ "${tx_shm:-0}" -gt "${tx_udp:-0}" ] && [ "${same_host:-0}" -gt 0 ]; then
    echo "default broadcast, one host: PASS - shared memory carried the stream"
else
    echo "default broadcast, one host: FAIL - two processes on one host did not use shared memory"
    echo "=== samehost_server.log ===" && cat samehost_server.log
    echo "=== samehost_client.log ===" && cat samehost_client.log
    status=1
fi

rm -f crosshost_server.log crosshost_client.log
# shellcheck disable=SC2024
sudo ip netns exec "$NS2" ./perf_server -b "$DEFAULT_BCAST" -d 4 </dev/null >crosshost_server.log 2>&1 &
crosshost_pid=$!
sleep 1
# shellcheck disable=SC2024
sudo ip netns exec "$NS1" ./perf_client -b "$DEFAULT_BCAST" -d 4 </dev/null >crosshost_client.log 2>&1
wait "$crosshost_pid" 2>/dev/null
cross_recv=$(grep '^RESULT:' crosshost_server.log | tail -1 | sed -n 's/.*recv=\([0-9,]*\).*/\1/p' | tr -d ',')
cross_tx_shm=$(grep '^RESULT:' crosshost_client.log | tail -1 | sed -n 's/.*tx_shm=\([0-9]*\).*/\1/p')
cross_same_server=$(field shm_same_host_peers crosshost_server.log)
cross_same_client=$(field shm_same_host_peers crosshost_client.log)
echo "default broadcast, two hosts: recv=${cross_recv:-<absent>} tx_shm=${cross_tx_shm:-<absent>}" \
    "shm_same_host_peers server=${cross_same_server:-<absent>} client=${cross_same_client:-<absent>}"
# Absent is a failure, not a zero: a field nobody printed is a check that did not look.
if [ "${cross_recv:-0}" -ge "$MIN_COUNT" ] && [ "${cross_tx_shm:-x}" = 0 ] && [ "${cross_same_server:-x}" = 0 ] &&
    [ "${cross_same_client:-x}" = 0 ]; then
    echo "default broadcast, two hosts: PASS - delivered over UDP, and neither side took the other for same-host"
else
    echo "default broadcast, two hosts: FAIL"
    echo "=== crosshost_server.log ===" && cat crosshost_server.log
    echo "=== crosshost_client.log ===" && cat crosshost_client.log
    status=1
fi

# UDP offload (udp_offload_check.sh): the same datagrams on the wire and in core with it on as off. Its own namespaces.
make udp_offload_check
if ./udp_offload_check.sh; then
    SUMMARY="$SUMMARY
udp offload: PASS"
else
    echo "udp offload: FAIL"
    SUMMARY="$SUMMARY
udp offload: FAIL"
    status=1
fi

echo
echo "=== Summary ==="
if [ "$status" -eq 0 ]; then
    echo "Overall: PASS"
else
    echo "Overall: FAIL"
fi
printf '%s\n' "$SUMMARY"

exit $status
