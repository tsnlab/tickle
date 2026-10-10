#!/bin/sh
# Copyright (c) 2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# UDP offload through the real Linux HAL (udp_offload_check.c): the wire carries the same datagrams with offload on as
# with it off, none above 1472 bytes, and core is handed every one of them whole. Two private network namespaces and a
# veth pair; needs passwordless `sudo ip` (ethtool and tc run inside the namespaces through `ip netns exec`).
#
# Arms, each one run of the sender's fixed patterns (udp_offload_check.c):
#   B  control: TT_UDP_OFFLOAD=0 at the sender, plain receiver. What the wire carried without offload.
#   A  send offload on, the veth's own UDP segmentation off - the kernel cuts every run in software before the device,
#      as for a NIC without it - plain receiver.
#   C  send offload on, the veth's UDP segmentation on - a run crosses the veth whole, as a NIC would cut it after the
#      host - receiver through the HAL with receive offload on: runs arrive merged and are handed out one at a time.
#   D  as C with TT_UDP_OFFLOAD=0 at the receiver: the kernel cuts the run for a socket without UDP_GRO.
#
# The rules, enforced below (written before the first run):
#   - every arm's list of datagrams (pattern, index, size, hash, in order) is B's, line for line;
#   - B has every test datagram (EXPECTED) and the largest is 1472;
#   - A and B: the receiver's ingress counted the same packets and bytes to the test port (tc u32, not tcpdump), and
#     no IP fragment at all (MF set) - so offload changed nothing on the wire;
#   - treatment: A and C sent with gso_sends > 0, B none; C merged (gro_merged > 0), D did not (gro_reads = 0).
# Exit 0 pass, 1 fail, 2 could not run.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
BIN=$HERE/udp_offload_check
EXPECTED=924 # 723 + 88 + 29 + 64 + 20 (pattern 5 sends half its 40 to a port nobody reads)
PORT=7461
NS1=uoc-tx-$$
NS2=uoc-rx-$$
WORK=$(mktemp -d)
NET=10.231.$(($$ % 250))
BCAST=$NET.255

# shellcheck disable=SC2329  # invoked by the EXIT trap below
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    rm -rf "$WORK"
    return 0
}
trap cleanup EXIT

[ -x "$BIN" ] || {
    echo "udp_offload_check: build it first (make -C platform/linux udp_offload_check)" >&2
    exit 2
}
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 2
sudo -n ip link add uoc1 netns "$NS1" type veth peer name uoc2 netns "$NS2" || exit 2
sudo -n ip -n "$NS1" addr add "$NET.1/24" dev uoc1
sudo -n ip -n "$NS2" addr add "$NET.2/24" dev uoc2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set uoc1 up
sudo -n ip -n "$NS2" link set uoc2 up
# The receiver's ingress counters: IP fragments first (prio 1), then whatever reaches the test port.
sudo -n ip netns exec "$NS2" tc qdisc add dev uoc2 ingress || exit 2
sudo -n ip netns exec "$NS2" tc filter add dev uoc2 parent ffff: protocol ip prio 1 u32 match u16 0x2000 0x2000 at 6 \
    action ok || exit 2
sudo -n ip netns exec "$NS2" tc filter add dev uoc2 parent ffff: protocol ip prio 2 u32 match ip protocol 17 0xff \
    match ip dport "$PORT" 0xffff action ok || exit 2

# "packets bytes" counted by the filter at PRIO since the last call (tc -s keeps totals; the caller subtracts).
counted() {
    sudo -n ip netns exec "$NS2" tc -s filter show dev uoc2 parent ffff: prio "$1" |
        awk '/Sent/ { print $4, $2; exit }'
}

fail=0
die() {
    echo "udp_offload_check: FAIL - $*" >&2
    fail=1
}

# arm NAME SENDER_OFFLOAD RECEIVER(plain|hal) RECEIVER_OFFLOAD VETH_SEGMENTATION(on|off)
arm() {
    name=$1
    sudo -n ip netns exec "$NS1" ethtool -K uoc1 tx-udp-segmentation "$5" >/dev/null 2>&1 || {
        echo "udp_offload_check: cannot set tx-udp-segmentation $5 on the veth" >&2
        exit 2
    }
    frag0=$(counted 1)
    port0=$(counted 2)
    # The logs are written by this shell, as the invoking user: intended.
    # shellcheck disable=SC2024
    if [ "$3" = plain ]; then
        sudo -n ip netns exec "$NS2" env TT_UDP_OFFLOAD="$4" "$BIN" plain "$PORT" >"$WORK/$name.rx" 2>"$WORK/$name.rx.err" &
    else
        sudo -n ip netns exec "$NS2" env TT_UDP_OFFLOAD="$4" "$BIN" hal "$PORT" "$BCAST" >"$WORK/$name.rx" \
            2>"$WORK/$name.rx.err" &
    fi
    rx=$!
    sleep 0.5
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" env TT_UDP_OFFLOAD="$2" "$BIN" send "$NET.2" "$PORT" "$BCAST" >"$WORK/$name.tx" \
        2>"$WORK/$name.tx.err"
    tx_rc=$?
    wait "$rx"
    rx_rc=$?
    frag1=$(counted 1)
    port1=$(counted 2)
    eval "${name}_frags=$(($(echo "$frag1" | cut -d' ' -f1) - $(echo "$frag0" | cut -d' ' -f1)))"
    eval "${name}_packets=$(($(echo "$port1" | cut -d' ' -f1) - $(echo "$port0" | cut -d' ' -f1)))"
    eval "${name}_bytes=$(($(echo "$port1" | cut -d' ' -f2) - $(echo "$port0" | cut -d' ' -f2)))"
    grep -v '^RESULT:' "$WORK/$name.rx" >"$WORK/$name.list"
    eval "echo \"$name: \$(grep -h '^RESULT:' '$WORK/$name.tx' '$WORK/$name.rx' | tr '\n' ' ')wire_packets=\$${name}_packets wire_bytes=\$${name}_bytes ip_fragments=\$${name}_frags\""
    [ "$tx_rc" -eq 0 ] || die "$name: sender exited $tx_rc ($(tail -1 "$WORK/$name.tx.err"))"
    [ "$rx_rc" -eq 0 ] || die "$name: receiver exited $rx_rc ($(tail -1 "$WORK/$name.rx.err"))"
    grep -q '^RESULT:.*ended=1' "$WORK/$name.rx" || die "$name: the receiver never saw the end marker"
}

field() { # FIELD FILE: the value of FIELD= on the file's RESULT line, or "missing"
    v=$(grep -h '^RESULT:' "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1)
    echo "${v:-missing}"
}

arm B 0 plain 0 off
arm A 1 plain 0 off
arm C 1 hal 1 on
arm D 1 hal 0 on

n=$(wc -l <"$WORK/B.list")
[ "$n" -eq "$EXPECTED" ] || die "control B received $n of $EXPECTED test datagrams - the setup is broken, nothing else holds"
[ "$(field largest "$WORK/B.rx")" = 1472 ] || die "control B: largest datagram $(field largest "$WORK/B.rx"), not 1472"
for a in A C D; do
    cmp -s "$WORK/B.list" "$WORK/$a.list" || die "$a's datagrams differ from the control's: $(diff "$WORK/B.list" "$WORK/$a.list" | head -3 | tr '\n' ' ')"
done
# shellcheck disable=SC2154 # set by eval in arm()
{
    [ "$A_packets" -eq "$B_packets" ] && [ "$A_bytes" -eq "$B_bytes" ] ||
        die "wire: A put $A_packets packets / $A_bytes bytes on the veth to the test port, the control $B_packets / $B_bytes"
    [ "$B_packets" -eq $((EXPECTED + 1)) ] || die "wire: the control counted $B_packets packets, not $((EXPECTED + 1))"
    [ "$A_frags" -eq 0 ] && [ "$B_frags" -eq 0 ] || die "wire: IP fragments seen (A $A_frags, control $B_frags)"
}
[ "$(field gso_sends "$WORK/B.tx")" = 0 ] || die "control B sent with UDP_SEGMENT"
[ "$(field gso_sends "$WORK/A.tx")" -gt 0 ] 2>/dev/null || die "A: no UDP_SEGMENT send - the treatment was not applied"
[ "$(field gso_sends "$WORK/C.tx")" -gt 0 ] 2>/dev/null || die "C: no UDP_SEGMENT send"
[ "$(field gro_merged "$WORK/C.rx")" -gt 0 ] 2>/dev/null || die "C: nothing merged - receive offload was not exercised"
[ "$(field gro_reads "$WORK/D.rx")" = 0 ] || die "D: merged with TT_UDP_OFFLOAD=0"

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "udp_offload_check: PASS - $EXPECTED datagrams identical in all four arms, wire identical with send offload, no fragments"
exit 0
