#!/bin/sh
# Copyright (c) 2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# A discovery reply never holds the polling thread on a full send buffer, and discovery converges when it is left unsent
# (discovery_reply_check.c, whose header has the rules). The link it needs: two private network namespaces joined by a
# veth pair, A's end shaped by tbf (RATE, a queue longer than A's send buffer so the queue drops nothing and the buffer
# is what fills), and an ingress filter on B's end dropping the flood's port. Needs passwordless `sudo ip` (tc runs
# inside the namespaces through `ip netns exec`). Run by test.sh (test-linux).
# Exit as the check: 0 pass, 1 fail, 2 could not run, 3 void.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
BIN=$HERE/discovery_reply_check
RATE="rate 2mbit burst 16kb latency 2s"
SINK_PORT=9
PORT=7471
NSA=drc-a-$$
NSB=drc-b-$$
NET=10.232.$(($$ % 250))

# shellcheck disable=SC2329  # invoked by the EXIT trap below
cleanup() {
    sudo -n ip netns del "$NSA" 2>/dev/null
    sudo -n ip netns del "$NSB" 2>/dev/null
    return 0
}
trap cleanup EXIT

[ -x "$BIN" ] || {
    echo "discovery_reply_check: build it first (make -C platform/linux discovery_reply_check)" >&2
    exit 2
}
sudo -n ip netns add "$NSA" && sudo -n ip netns add "$NSB" || exit 2
sudo -n ip link add drca netns "$NSA" type veth peer name drcb netns "$NSB" || exit 2
sudo -n ip -n "$NSA" addr add "$NET.1/24" dev drca || exit 2
sudo -n ip -n "$NSB" addr add "$NET.2/24" dev drcb || exit 2
for ns in "$NSA" "$NSB"; do sudo -n ip -n "$ns" link set lo up || exit 2; done
sudo -n ip -n "$NSA" link set drca up || exit 2
sudo -n ip -n "$NSB" link set drcb up || exit 2
# shellcheck disable=SC2086  # RATE is tc's arguments, split on purpose
sudo -n ip netns exec "$NSA" tc qdisc add dev drca root tbf $RATE || exit 2
sudo -n ip netns exec "$NSB" tc qdisc add dev drcb ingress || exit 2
sudo -n ip netns exec "$NSB" tc filter add dev drcb parent ffff: protocol ip prio 1 u32 match ip protocol 17 0xff \
    match ip dport "$SINK_PORT" 0xffff action drop || exit 2

sudo -n ip netns exec "$NSA" "$BIN" "/run/netns/$NSB" "$NET.255" "$NET.2" "$PORT"
status=$?
# The link's own account: what tbf sent and dropped, and what B's ingress dropped (the flood).
sudo -n ip netns exec "$NSA" tc -s qdisc show dev drca | sed 's/^/discovery_reply_check: tbf: /'
exit "$status"
