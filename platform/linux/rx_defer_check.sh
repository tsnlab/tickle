#!/bin/sh
# Copyright (c) 2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# Deferred waits while a large sample arrives (hal_linux.c rx_defer(), core's large_expect()). Needs passwordless
# `sudo ip`; every TickLE process runs in a private namespace. Exit 0 pass, 1 fail, 2 could not run.
#   1. rx_defer_check.c in a namespace with only loopback up: as built, with TT_RX_DEFER=0, and under
#      refuse_epoll.so (no wait set: the deferral waits with ppoll()).
#   2. End to end, two namespaces joined by a veth: large_sample_bench, 1 MB RELIABLE samples at 30 Hz for DUR s,
#      paced as the rig's link delivers them (netem rate 1gbit slot 49us at the publisher; one datagram per send,
#      TT_UDP_OFFLOAD=0 there) - deferral on, then TT_RX_DEFER=0 at the subscriber as the control. The rules,
#      written before the first run: both arms deliver every published sample with torn=0 (the bench checks every
#      byte); the deferral arm deferred (rx_defers > 0), the control did not (rx_defers = 0).
# shellcheck disable=SC2024 # the redirects are to our own files: sudo is for ip netns, not for them
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
BIN=$HERE/rx_defer_check
BENCH=$HERE/large_sample_bench
SHIM=$HERE/refuse_epoll.so
DUR=3
NS=rxdc-$$
NSP=rxdc-pub-$$
NSS=rxdc-sub-$$
WORK=$(mktemp -d)
for f in "$BIN" "$BENCH" "$SHIM"; do
    [ -e "$f" ] || { echo "rx_defer_check.sh: build $f first" >&2; exit 2; }
done
# shellcheck disable=SC2329  # invoked by the EXIT trap below
cleanup() {
    for ns in "$NS" "$NSP" "$NSS"; do
        sudo -n ip netns del "$ns" 2>/dev/null
    done
    rm -rf "$WORK"
    return 0
}
trap cleanup EXIT
status=0

if ! sudo -n ip netns add "$NS" || ! sudo -n ip -n "$NS" link set lo up; then
    echo "rx_defer_check.sh: no namespace" >&2
    exit 2
fi
sudo -n ip netns exec "$NS" "$BIN" || status=1
sudo -n ip netns exec "$NS" "$BIN" off || status=1
sudo -n ip netns exec "$NS" env LD_PRELOAD="$SHIM" "$BIN" >"$WORK/shim.log" 2>&1 || status=1
cat "$WORK/shim.log"
grep -q 'budget - .* epoll off' "$WORK/shim.log" ||
    { echo "rx_defer_check.sh: FAIL - the shim did not take epoll away, so the ppoll() path was not checked"; status=1; }

if ! sudo -n ip netns add "$NSP" || ! sudo -n ip netns add "$NSS"; then
    echo "rx_defer_check.sh: no namespaces" >&2
    exit 2
fi
sudo -n ip link add rxdcv1 netns "$NSP" type veth peer name rxdcv2 netns "$NSS" || exit 2
sudo -n ip -n "$NSP" addr add 192.168.10.1/24 brd + dev rxdcv1
sudo -n ip -n "$NSS" addr add 192.168.10.2/24 brd + dev rxdcv2
for ns in "$NSP" "$NSS"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NSP" link set rxdcv1 up
sudo -n ip -n "$NSS" link set rxdcv2 up
sudo -n ip netns exec "$NSP" tc qdisc add dev rxdcv1 root netem limit 100000 rate 1gbit slot 49us 49us || exit 2

e2e() { # ARM DEFER
    sudo -n ip netns exec "$NSS" env TT_RX_DEFER="$2" "$BENCH" sub -s 1048576 -d $((DUR + 7)) -R >"$WORK/sub.log" 2>&1 &
    sub=$!
    sleep 1
    sudo -n ip netns exec "$NSP" env TT_UDP_OFFLOAD=0 "$BENCH" pub -s 1048576 -r 30 -d "$DUR" -R >"$WORK/pub.log" 2>&1
    wait "$sub"
    published=$(grep -m1 '^RESULT' "$WORK/pub.log" | grep -o ' published=[0-9]*' | cut -d= -f2)
    res=$(grep -m1 '^RESULT' "$WORK/sub.log")
    delivered=$(echo "$res" | grep -o ' delivered=[0-9]*' | cut -d= -f2)
    torn=$(echo "$res" | grep -o ' torn=[0-9]*' | cut -d= -f2)
    defers=$(echo "$res" | grep -o ' rx_defers=[0-9]*' | cut -d= -f2)
    line="published=${published:-none} delivered=${delivered:-none} torn=${torn:-none} rx_defers=${defers:-none}"
    ok=0
    [ -n "$published" ] && [ "$published" -gt 0 ] && [ "${delivered:-0}" = "$published" ] && [ "${torn:-1}" = 0 ] && ok=1
    case "$2:${defers:-x}" in
    1:0 | 1:x | 0:x) ok=0 ;;
    0:0 | 1:*) ;;
    *) ok=0 ;;
    esac
    if [ $ok = 1 ]; then
        echo "rx_defer_check.sh: PASS e2e $1 - $line"
    else
        echo "rx_defer_check.sh: FAIL e2e $1 - $line (must deliver all, torn 0, rx_defers $([ "$2" = 1 ] && echo '> 0' || echo 0))"
        tail -3 "$WORK/sub.log" "$WORK/pub.log"
        status=1
    fi
}
e2e deferral 1
e2e control 0
exit $status
