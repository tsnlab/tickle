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
# sockets instead of hal_freertos.c's lwIP ones. Reuses the ping/pong example binaries
# unmodified (not a purpose-built harness like main_ping.c/main_pong.c) - real user-facing code,
# exercised the same way `make runping`/`make runpong` would run it by hand.

set -u
cd "$(dirname "$0")"

PING_COUNT=${PING_COUNT:-20}
PING_INTERVAL_S=${PING_INTERVAL_S:-0.2}
MIN_REPLIES=5

# Idempotent: clear out any namespaces a previous (e.g. interrupted) run left behind before
# creating fresh ones - createns/deletens themselves aren't safe to call twice in a row.
sudo ip netns delete ns1 >/dev/null 2>&1
sudo ip netns delete ns2 >/dev/null 2>&1
make createns

make ping_pong

rm -f ping.log pong.log

# pong has no fixed end condition of its own (see pong.c) - -d bounds it generously past however
# long ping's own -c/-i bound their side to, so it always exits on its own rather than needing a
# kill/wait dance like run_pair.sh's backgrounded QEMU instance does.
# stdin redirected from /dev/null: pong.c doesn't itself read stdin, but leaving a backgrounded
# process attached to the invoking terminal's stdin is a latent SIGTTIN/job-control hazard (see
# run_pair.sh's own fix for the concrete failure mode this avoids) - cheap to rule out here too.
sudo ip netns exec ns2 ./pong -d 15 </dev/null >pong.log 2>&1 &
pong_pid=$!

sleep 1

sudo ip netns exec ns1 ./ping -c "$PING_COUNT" -i "$PING_INTERVAL_S" >ping.log 2>&1
ping_status=$?

wait "$pong_pid" 2>/dev/null

make deletens

echo "=== pong.log ==="
cat pong.log
echo "=== ping.log ==="
cat ping.log

if [ "$ping_status" -ne 0 ]; then
    echo "netns_run_pair: ping exited unexpectedly (status $ping_status)"
    exit 1
fi

# pong.c never logs per-request (unlike main_pong.c's FreeRTOS counterpart, which does purely
# for this kind of check) - but a "seq=N time=X ms" line in ping's own log can only appear from
# a real decoded CallResponse, so that alone is sufficient evidence of a real round trip.
replies=$(grep -c '^seq=' ping.log)
echo "netns_run_pair: ping received $replies real repl(y/ies)"

if [ "$replies" -lt "$MIN_REPLIES" ]; then
    echo "netns_run_pair: fewer than $MIN_REPLIES real round trips observed - treating as a failure"
    exit 1
fi
