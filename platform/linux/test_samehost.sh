#!/bin/sh
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# Two nodes on ONE host must be able to talk to each other. Needs no root, no namespaces and no
# second machine, so unlike test.sh it can run on every push - which is the whole point.
#
# Why this exists (2026-09-23): TickLE used to bind every node to the well-known port and nothing
# else, so two nodes on one host shared an address, and a unicast addressed to it was delivered by
# the kernel to exactly one of the two sockets - sometimes the sender's own. Discovery still
# worked, because a broadcast reaches every socket bound to that port, so the pair matched and the
# graph looked healthy; data then went nowhere. In the rmw_tickle benchmark that showed up as a
# Publisher receiving 10009 of its own 10010 datagrams while its Subscriber got 25, and as roughly
# 10% of the stream going to the sender in runs that passed and were recorded as clean.
#
# The mechanism was not unknown: test.sh's own header already recorded that "a kernel delivers a
# unicast packet aimed at one wildcard-bound socket to whichever such socket bound last", and the
# response at the time was to give that test two network namespaces so it would stop hitting it.
# That made the test pass and left the product broken, which is why this script exists as a test
# of the product rather than as a note in a comment.
#
# What it pins is specifically the path that only breaks AFTER discovery succeeds: a Publisher
# that has already learned a same-host peer must still reach it. A naive smoke test that only
# checks discovery would have passed throughout.

set -eu

HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
PUB="$HERE/publisher"
SUB="$HERE/subscriber"
# Loopback broadcast: both nodes are on this host, and this keeps the test's traffic off whatever
# network the machine is attached to. That is not a detail - on 2026-09-23 TickLE's compiled-in
# default of 255.255.255.255 put benchmark traffic onto a shared lab network.
BCAST=127.255.255.255
COUNT=5
PUB_ID=52
SUB_ID=51
SUB_LOG=$(mktemp)
PUB_LOG=$(mktemp)

cleanup() {
    [ -n "${SUB_PID:-}" ] && kill -9 "$SUB_PID" 2>/dev/null
    rm -f "$SUB_LOG" "$PUB_LOG"
    return 0
}
trap cleanup EXIT

if [ ! -x "$PUB" ] || [ ! -x "$SUB" ]; then
    echo "same-host test: build the examples first (make examples)" >&2
    exit 1
fi

# A leftover node from an earlier run is not a stale process to be tidied up, it is a second node
# wearing this test's own node id - which is a different failure entirely (two nodes sharing an id
# discard each other's traffic as self-sent) and would be misread as this bug. Refuse rather than
# guess. Matched on the executable, never on a command line: a pattern matching a command line
# matches the very command doing the matching.
for pid in $(pgrep -x subscriber 2>/dev/null) $(pgrep -x publisher 2>/dev/null); do
    exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || continue
    case "$exe" in
    "$PUB" | "$SUB")
        echo "same-host test: a previous $exe (pid $pid) is still running - refusing to run" >&2
        exit 1
        ;;
    esac
done

# -d bounds the subscriber's own run so it exits and prints its statistics by itself; the
# publisher is given a shorter -c/-i budget so it finishes first and the count is complete.
"$SUB" -b "$BCAST" -I "$SUB_ID" -d 14 >"$SUB_LOG" 2>&1 </dev/null &
SUB_PID=$!
sleep 2

timeout 30 "$PUB" -b "$BCAST" -I "$PUB_ID" -c "$COUNT" -i 1 >"$PUB_LOG" 2>&1 || true
wait "$SUB_PID" 2>/dev/null || true
SUB_PID=

fail() {
    echo "same-host test: FAIL - $1" >&2
    echo "--- subscriber ---" >&2
    cat "$SUB_LOG" >&2
    echo "--- publisher ---" >&2
    cat "$PUB_LOG" >&2
    exit 1
}

received=$(grep -c '^seq=' "$SUB_LOG" || true)
[ "$received" -eq "$COUNT" ] || fail "subscriber received $received of $COUNT messages"

# The assertion that catches the mechanism rather than the symptom. The first version of this
# check was `pub_self -lt pub_tx`, which the benchmark signature this test exists to catch passes
# comfortably: 10009 self-received of 10010 sent is "less than". The delivery check above would
# still have failed, but only on the symptom, and a test that names a mechanism should test it.
#
# The arithmetic, so a reader can check it rather than trust it: tx_datagrams counts one send per
# destination, so it is COUNT data samples unicast to the one peer, plus however many broadcast
# announces went out. A node receives its own broadcasts (that is why self_sent is never zero and
# why process_packet() has a self_sent branch at all), but it must never receive back a data
# sample it unicast to somebody else. So at least COUNT of what it sent must not come back.
# Exact by construction rather than by margin: every extra announce adds one to each side, and an
# announce that goes out as unicast once a peer is known adds to tx alone.
pub_tx=$(sed -n 's/.*Node .* traffic: tx_datagrams=\([0-9]*\).*/\1/p' "$PUB_LOG" | tail -1)
pub_self=$(sed -n 's/.*Node .* traffic: .*rx_self_sent=\([0-9]*\).*/\1/p' "$PUB_LOG" | tail -1)
[ -n "$pub_tx" ] && [ -n "$pub_self" ] || fail "publisher printed no traffic counters"
[ "$((pub_tx - pub_self))" -ge "$COUNT" ] ||
    fail "publisher sent $pub_tx and received $pub_self of them back, leaving fewer than the $COUNT data samples unaccounted for - its own unicast is landing on its own socket"

echo "same-host test: PASS ($received/$COUNT delivered, publisher self-received $pub_self of $pub_tx sent)"
