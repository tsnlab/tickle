#!/bin/sh
# Copyright (c) 2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# Receiving a large sample's fragments in place (hal_linux.c direct_read(), core's large_direct_aim()), end to end:
# large_sample_bench between two private namespaces joined by a veth, 1 MB samples, the subscriber reading in place.
# The bench checks every byte of every sample it delivers (torn=), so a fragment put in the wrong place, or one read in
# place that was not the one expected, shows. Needs passwordless `sudo ip`. Exit 0 pass, 1 fail, 2 could not run.
#
# First rx_direct_hal_check (crafted reads, a namespace with only loopback: what no traffic below reaches on its own -
# a run of another size, a cancel, an empty datagram); then the arms (the rules written before the first run; all must
# hold):
#   merged   RELIABLE, sender's send offload on, no qdisc: runs arrive merged (UDP_GRO) - every sample delivered,
#            torn 0, fragments read in place (direct_split > 0) and found there (direct_placed = direct_split).
#   single   RELIABLE, one datagram per send, netem rate 1gbit slot 49us: reads of one datagram each - as merged.
#   loss     RELIABLE, as single with netem loss 3%: resends come in the full form, not as aimed - every sample
#            delivered, torn 0, and reads that did not go as aimed were put back together (direct_gathered > 0).
#   reorder  BEST_EFFORT, netem delay 1ms reorder 25%: fragments out of order inside a read - torn 0, some delivered,
#            direct_gathered > 0.
#   control  merged with TT_RX_DIRECT unset at the subscriber (the default, off): every sample, torn 0, and no read in
#            place (direct_reads 0). Every other arm sets TT_RX_DIRECT=1.
# shellcheck disable=SC2024 # the redirects are to our own files: sudo is for ip netns, not for them
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
BENCH=$HERE/large_sample_bench
HALCHECK=$HERE/rx_direct_hal_check
NS=rxdi-lo-$$
DUR=3
NSP=rxdi-pub-$$
NSS=rxdi-sub-$$
WORK=$(mktemp -d)
[ -x "$BENCH" ] && [ -x "$HALCHECK" ] || { echo "rx_direct_check.sh: build large_sample_bench, rx_direct_hal_check" >&2; exit 2; }
# shellcheck disable=SC2329  # invoked by the EXIT trap below
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    sudo -n ip netns del "$NSP" 2>/dev/null
    sudo -n ip netns del "$NSS" 2>/dev/null
    rm -rf "$WORK"
    return 0
}
trap cleanup EXIT
status=0
if ! sudo -n ip netns add "$NS" || ! sudo -n ip -n "$NS" link set lo up; then
    echo "rx_direct_check.sh: no namespace" >&2
    exit 2
fi
sudo -n ip netns exec "$NS" "$HALCHECK" || status=1
if ! sudo -n ip netns add "$NSP" || ! sudo -n ip netns add "$NSS"; then
    echo "rx_direct_check.sh: no namespaces" >&2
    exit 2
fi
sudo -n ip link add rxdiv1 netns "$NSP" type veth peer name rxdiv2 netns "$NSS" || exit 2
sudo -n ip -n "$NSP" addr add 192.168.10.1/24 brd + dev rxdiv1
sudo -n ip -n "$NSS" addr add 192.168.10.2/24 brd + dev rxdiv2
for ns in "$NSP" "$NSS"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NSP" link set rxdiv1 up
sudo -n ip -n "$NSS" link set rxdiv2 up

field() { # NAME LINE
    echo "$2" | grep -o " $1=[0-9]*" | head -1 | cut -d= -f2
}

# arm NAME QDISC PUB_OFFLOAD RELIABLE(-R or -) SUB_DIRECT
arm() {
    sudo -n ip netns exec "$NSP" tc qdisc del dev rxdiv1 root 2>/dev/null
    if [ "$2" != none ]; then
        # shellcheck disable=SC2086
        sudo -n ip netns exec "$NSP" tc qdisc add dev rxdiv1 root netem limit 100000 $2 || { status=1; return; }
    fi
    rel=$4
    [ "$rel" = - ] && rel=""
    # shellcheck disable=SC2086
    sudo -n ip netns exec "$NSS" env ${5:+TT_RX_DIRECT=$5} "$BENCH" sub -s 1048576 -d $((DUR + 7)) $rel >"$WORK/sub.log" 2>&1 &
    sub=$!
    sleep 1
    # shellcheck disable=SC2086
    sudo -n ip netns exec "$NSP" env TT_UDP_OFFLOAD="$3" "$BENCH" pub -s 1048576 -r 30 -d "$DUR" $rel >"$WORK/pub.log" 2>&1
    wait "$sub"
    pres=$(grep -m1 '^RESULT' "$WORK/pub.log")
    res=$(grep -m1 '^RESULT' "$WORK/sub.log")
    published=$(field published "$pres")
    delivered=$(field delivered "$res")
    torn=$(field torn "$res")
    reads=$(field direct_reads "$res")
    split=$(field direct_split "$res")
    gathered=$(field direct_gathered "$res")
    placed=$(field direct_placed "$res")
    line="published=${published:-none} delivered=${delivered:-none} torn=${torn:-none} direct_reads=${reads:-none}"
    line="$line direct_split=${split:-none} direct_gathered=${gathered:-none} direct_placed=${placed:-none}"
    ok=1
    [ -n "$published" ] && [ "${torn:-1}" = 0 ] && [ -n "$reads" ] || ok=0
    case $1 in
    merged | single) [ "$delivered" = "$published" ] && [ "$split" -gt 0 ] && [ "$placed" = "$split" ] || ok=0 ;;
    loss) [ "$delivered" = "$published" ] && [ "$gathered" -gt 0 ] || ok=0 ;;
    reorder) [ "${delivered:-0}" -gt 0 ] && [ "$gathered" -gt 0 ] || ok=0 ;;
    control) [ "$delivered" = "$published" ] && [ "$reads" = 0 ] || ok=0 ;;
    esac
    if [ $ok = 1 ]; then
        echo "rx_direct_check.sh: PASS $1 - $line"
    else
        echo "rx_direct_check.sh: FAIL $1 - $line"
        tail -3 "$WORK/sub.log" "$WORK/pub.log"
        status=1
    fi
}
arm merged none 1 -R 1
arm single "rate 1gbit slot 49us 49us" 0 -R 1
arm loss "rate 1gbit slot 49us 49us loss 3%" 0 -R 1
arm reorder "delay 1ms reorder 25%" 0 - 1
arm control none 1 -R ""
exit $status
