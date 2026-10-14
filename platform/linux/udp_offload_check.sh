#!/bin/sh
# Copyright (c) 2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# UDP offload through the real Linux HAL (udp_offload_check.c): the wire carries the same datagrams with offload on as
# with it off, none above 1472 bytes but pattern 8's hybrid-transport runs (each one IP-fragmented datagram, sent as
# such with offload on as off), and core is handed every one of them whole. Two private network namespaces and a
# veth pair; needs passwordless `sudo ip` (ethtool and tc run inside the namespaces through `ip netns exec`).
#
# Arms, each one run of the sender's fixed patterns (udp_offload_check.c):
#   B  control: TT_UDP_OFFLOAD=0 at the sender, plain receiver. What the wire carried without offload.
#   A  send offload on, the veth's own UDP segmentation off - the kernel cuts every run in software before the device,
#      as for a NIC without it - plain receiver.
#   C  send offload on, the veth's UDP segmentation on - a run crosses the veth whole, as a NIC would cut it after the
#      host - receiver through the HAL with receive offload on: runs arrive merged and are handed out one at a time.
#   D  as C with TT_UDP_OFFLOAD=0 at the receiver: the kernel cuts the run for a socket without UDP_GRO.
#   E  as C, the patterns sent REPEAT times, read by a build that leaves receive offload after 2 quiet reads
#      (TT_GRO_QUIET_READS=2, udp_offload_check_flap): it goes on at every large-fragment pattern and back to plain
#      reads in the pauses, so every switch - and its race with merged datagrams still queued - is crossed many times.
#      A merged datagram read with the option off would come back as one block of several datagrams: a size no
#      pattern has, which the receiver fails on.
#   F  as A with TT_UDP_CSUM0=1 at the sender (hal_linux.c "UDP checksum 0", 2026-10-14): IP-fragmented datagrams go
#      with UDP checksum 0, everything else with its checksum, and send offload's runs as before.
#   G  as C with TT_UDP_CSUM0=1 at the sender: the HAL receiver, receive offload on, takes the zero-checksum runs.
#
# The rules, enforced below (written before the first run):
#   - every arm's list of datagrams (pattern, index, size, hash, in order) is B's, line for line;
#   - B has every test datagram (EXPECTED) and the largest is a run (RUN_SIZE, pattern 8);
#   - A and B: the receiver's ingress counted the same packets and bytes to the test port (tc u32, not tcpdump; whole
#     datagrams only, so a run's fragments are not among them), and exactly the IP fragments of pattern 8's runs (MF
#     set: RUN_MF_FRAGMENTS) - so offload changed nothing on the wire and never fragmented an MTU datagram;
#   - runs under receive offload (2026-10-11, the hybrid transport): C read runs while UDP_GRO was on (runs_gro_on > 0),
#     and each came whole - a run merged by GRO would be cut at the cmsg's segment size and C's list would differ.
#   - treatment: A and C sent with gso_sends > 0, B none; C merged (gro_merged > 0), D did not (gro_reads = 0).
#   - which runs (2026-10-11, large fragments only): A, C and E sent exactly the runs the sender expects of its
#     patterns (gso_sends/gso_datagrams = *_expected) - every large-fragment run, and nothing of the small patterns.
#   - E: its list is the control's REPEAT times over; it merged, and went back to plain (gro_commits) and on again
#     (gro_enables) at least REPEAT - 1 times each.
#   - checksum 0 (F, G; B, A and C the controls): the receiver's ingress counts the datagrams to the test port that carry
#     UDP checksum 0 (tc u32 on the UDP header of a whole datagram, or of a run's first fragment). F and G: exactly the
#     runs' first fragments carry it (RUNS) and, whole, exactly the datagrams the sender reports beyond the runs
#     (csum0_datagrams - RUNS, its own count matching csum0_datagrams_expected) - so nothing else of the patterns lost
#     its checksum, and the option was off for every UDP_SEGMENT call; csum0=1 and csum0_retries=0 (no UDP_SEGMENT call
#     refused), and SO_NO_CHECK switched exactly csum0_switches_expected times (only where the kind of call changes).
#     B, A and
#     C: none at all, and csum0=0. Every arm's list is still B's, so the receivers took every zero-checksum datagram.
# Exit 0 pass, 1 fail, 2 could not run.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
BIN=$HERE/udp_offload_check
FLAP=$HERE/udp_offload_check_flap
REPEAT=10
EXPECTED=993 # 723 + 88 + 30 + 64 + 20 + 24 + 20 + 24 (pattern 5 sends half its 40 to a port nobody reads)
RUNS=10                 # pattern 8's run datagrams
RUN_SIZE=65372          # each: 65380 B of UDP in 45 IP fragments of 1480, the first 44 with MF set
RUN_MF_FRAGMENTS=440
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

[ -x "$BIN" ] && [ -x "$FLAP" ] || {
    echo "udp_offload_check: build it first (make -C platform/linux udp_offload_check udp_offload_check_flap)" >&2
    exit 2
}
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 2
sudo -n ip link add uoc1 netns "$NS1" type veth peer name uoc2 netns "$NS2" || exit 2
sudo -n ip -n "$NS1" addr add "$NET.1/24" dev uoc1
sudo -n ip -n "$NS2" addr add "$NET.2/24" dev uoc2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set uoc1 up
sudo -n ip -n "$NS2" link set uoc2 up
# The receiver's ingress counters: datagrams to the test port with UDP checksum 0 - a run's first fragment (prio 1) and
# a whole datagram (prio 2), each passing the packet on to the next filter - then IP fragments (prio 3), then whatever
# reaches the test port (prio 4). The UDP checksum is at byte 26: the namespaces' IP headers have no options.
sudo -n ip netns exec "$NS2" tc qdisc add dev uoc2 ingress || exit 2
sudo -n ip netns exec "$NS2" tc filter add dev uoc2 parent ffff: protocol ip prio 1 u32 match ip protocol 17 0xff \
    match u16 0x2000 0x3fff at 6 match u16 0 0xffff at 26 match ip dport "$PORT" 0xffff action continue || exit 2
sudo -n ip netns exec "$NS2" tc filter add dev uoc2 parent ffff: protocol ip prio 2 u32 match ip protocol 17 0xff \
    match u16 0 0x3fff at 6 match u16 0 0xffff at 26 match ip dport "$PORT" 0xffff action continue || exit 2
sudo -n ip netns exec "$NS2" tc filter add dev uoc2 parent ffff: protocol ip prio 3 u32 match u16 0x2000 0x2000 at 6 \
    action ok || exit 2
sudo -n ip netns exec "$NS2" tc filter add dev uoc2 parent ffff: protocol ip prio 4 u32 match ip protocol 17 0xff \
    match u16 0 0x1fff at 6 match ip dport "$PORT" 0xffff action ok || exit 2

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

# arm NAME SENDER_OFFLOAD RECEIVER(plain|hal) RECEIVER_OFFLOAD VETH_SEGMENTATION(on|off) [RECEIVER_BINARY REPEAT]
arm() {
    name=$1
    rx_bin=${6:-$BIN}
    repeat=${7:-1}
    sudo -n ip netns exec "$NS1" ethtool -K uoc1 tx-udp-segmentation "$5" >/dev/null 2>&1 || {
        echo "udp_offload_check: cannot set tx-udp-segmentation $5 on the veth" >&2
        exit 2
    }
    zfrag0=$(counted 1)
    zwhole0=$(counted 2)
    frag0=$(counted 3)
    port0=$(counted 4)
    # The logs are written by this shell, as the invoking user: intended.
    # shellcheck disable=SC2024
    if [ "$3" = plain ]; then
        sudo -n ip netns exec "$NS2" env TT_UDP_OFFLOAD="$4" "$BIN" plain "$PORT" >"$WORK/$name.rx" 2>"$WORK/$name.rx.err" &
    else
        sudo -n ip netns exec "$NS2" env TT_UDP_OFFLOAD="$4" "$rx_bin" hal "$PORT" "$BCAST" >"$WORK/$name.rx" \
            2>"$WORK/$name.rx.err" &
    fi
    rx=$!
    sleep 0.5
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" env TT_UDP_OFFLOAD="$2" TT_UDP_CSUM0="$CSUM0" "$BIN" send "$NET.2" "$PORT" "$BCAST" "$repeat" >"$WORK/$name.tx" \
        2>"$WORK/$name.tx.err"
    tx_rc=$?
    wait "$rx"
    rx_rc=$?
    zfrag1=$(counted 1)
    zwhole1=$(counted 2)
    frag1=$(counted 3)
    port1=$(counted 4)
    eval "${name}_zfrags=$(($(echo "$zfrag1" | cut -d' ' -f1) - $(echo "$zfrag0" | cut -d' ' -f1)))"
    eval "${name}_zwhole=$(($(echo "$zwhole1" | cut -d' ' -f1) - $(echo "$zwhole0" | cut -d' ' -f1)))"
    eval "${name}_frags=$(($(echo "$frag1" | cut -d' ' -f1) - $(echo "$frag0" | cut -d' ' -f1)))"
    eval "${name}_packets=$(($(echo "$port1" | cut -d' ' -f1) - $(echo "$port0" | cut -d' ' -f1)))"
    eval "${name}_bytes=$(($(echo "$port1" | cut -d' ' -f2) - $(echo "$port0" | cut -d' ' -f2)))"
    grep -v '^RESULT:' "$WORK/$name.rx" >"$WORK/$name.list"
    eval "echo \"$name: \$(grep -h '^RESULT:' '$WORK/$name.tx' '$WORK/$name.rx' | tr '\n' ' ')wire_packets=\$${name}_packets wire_bytes=\$${name}_bytes ip_fragments=\$${name}_frags csum0_first_fragments=\$${name}_zfrags csum0_whole=\$${name}_zwhole\""
    [ "$tx_rc" -eq 0 ] || die "$name: sender exited $tx_rc ($(tail -1 "$WORK/$name.tx.err"))"
    [ "$rx_rc" -eq 0 ] || die "$name: receiver exited $rx_rc ($(tail -1 "$WORK/$name.rx.err"))"
    grep -q '^RESULT:.*ended=1' "$WORK/$name.rx" || die "$name: the receiver never saw the end marker"
}

field() { # FIELD FILE: the value of FIELD= on the file's RESULT line, or "missing"
    v=$(grep -h '^RESULT:' "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1)
    echo "${v:-missing}"
}

CSUM0=0
arm B 0 plain 0 off
arm A 1 plain 0 off
arm C 1 hal 1 on
arm D 1 hal 0 on
arm E 1 hal 1 on "$FLAP" "$REPEAT"
CSUM0=1
arm F 1 plain 0 off
arm G 1 hal 1 on
CSUM0=0

n=$(wc -l <"$WORK/B.list")
[ "$n" -eq "$EXPECTED" ] || die "control B received $n of $EXPECTED test datagrams - the setup is broken, nothing else holds"
[ "$(field largest "$WORK/B.rx")" = "$RUN_SIZE" ] ||
    die "control B: largest datagram $(field largest "$WORK/B.rx"), not a run's $RUN_SIZE"
for a in A C D F G; do
    cmp -s "$WORK/B.list" "$WORK/$a.list" || die "$a's datagrams differ from the control's: $(diff "$WORK/B.list" "$WORK/$a.list" | head -3 | tr '\n' ' ')"
done
# shellcheck disable=SC2154 # set by eval in arm()
{
    [ "$A_packets" -eq "$B_packets" ] && [ "$A_bytes" -eq "$B_bytes" ] ||
        die "wire: A put $A_packets packets / $A_bytes bytes on the veth to the test port, the control $B_packets / $B_bytes"
    [ "$B_packets" -eq $((EXPECTED + 1 - RUNS)) ] ||
        die "wire: the control counted $B_packets whole datagrams, not $((EXPECTED + 1 - RUNS))"
    [ "$A_frags" -eq "$RUN_MF_FRAGMENTS" ] && [ "$B_frags" -eq "$RUN_MF_FRAGMENTS" ] ||
        die "wire: IP fragments A $A_frags, control $B_frags - not the runs' $RUN_MF_FRAGMENTS"
}
[ "$(field gso_sends "$WORK/B.tx")" = 0 ] || die "control B sent with UDP_SEGMENT"
[ "$(field gso_sends "$WORK/A.tx")" -gt 0 ] 2>/dev/null || die "A: no UDP_SEGMENT send - the treatment was not applied"
[ "$(field gso_sends "$WORK/C.tx")" -gt 0 ] 2>/dev/null || die "C: no UDP_SEGMENT send"
for a in A C E F G; do
    for f in gso_sends gso_datagrams; do
        [ "$(field $f "$WORK/$a.tx")" = "$(field ${f}_expected "$WORK/$a.tx")" ] ||
            die "$a: $f=$(field $f "$WORK/$a.tx"), expected $(field ${f}_expected "$WORK/$a.tx") - runs other than large fragments"
    done
done
[ "$(field gro_merged "$WORK/C.rx")" -gt 0 ] 2>/dev/null || die "C: nothing merged - receive offload was not exercised"
[ "$(field runs_gro_on "$WORK/C.rx")" -gt 0 ] 2>/dev/null ||
    die "C: no run read while UDP_GRO was on - the runs-under-GRO case was not tested"
[ "$(field gro_reads "$WORK/D.rx")" = 0 ] || die "D: merged with TT_UDP_OFFLOAD=0"
for _ in $(seq 1 "$REPEAT"); do cat "$WORK/B.list"; done >"$WORK/B.repeat"
cmp -s "$WORK/B.repeat" "$WORK/E.list" ||
    die "E's datagrams differ from the control's x$REPEAT: $(diff "$WORK/B.repeat" "$WORK/E.list" | head -3 | tr '\n' ' ')"
[ "$(field gro_merged "$WORK/E.rx")" -gt 0 ] 2>/dev/null || die "E: nothing merged"
[ "$(field gro_commits "$WORK/E.rx")" -ge $((REPEAT - 1)) ] 2>/dev/null ||
    die "E: went back to plain reads $(field gro_commits "$WORK/E.rx") times, not $((REPEAT - 1)) - the switch was not crossed"
[ "$(field gro_enables "$WORK/E.rx")" -ge $((REPEAT - 1)) ] 2>/dev/null ||
    die "E: turned receive offload on $(field gro_enables "$WORK/E.rx") times, not $((REPEAT - 1))"

# Checksum 0 (header): the treatment exactly where it belongs on F and G, nowhere on the controls.
# shellcheck disable=SC2154 # set by eval in arm()
for a in B A C; do
    eval "zf=\$${a}_zfrags zw=\$${a}_zwhole"
    [ "$zf" -eq 0 ] && [ "$zw" -eq 0 ] || die "control $a: $zf first fragments and $zw whole datagrams with checksum 0"
    [ "$(field csum0 "$WORK/$a.tx")" = 0 ] || die "control $a: csum0=$(field csum0 "$WORK/$a.tx"), not 0"
done
for a in F G; do
    eval "zf=\$${a}_zfrags zw=\$${a}_zwhole"
    sent0=$(field csum0_datagrams "$WORK/$a.tx")
    [ "$(field csum0 "$WORK/$a.tx")" = 1 ] || die "$a: csum0=$(field csum0 "$WORK/$a.tx") - the option was not applied"
    [ "$sent0" = "$(field csum0_datagrams_expected "$WORK/$a.tx")" ] ||
        die "$a: csum0_datagrams=$sent0, expected $(field csum0_datagrams_expected "$WORK/$a.tx")"
    [ "$(field csum0_retries "$WORK/$a.tx")" = 0 ] || die "$a: csum0_retries=$(field csum0_retries "$WORK/$a.tx")"
    [ "$(field csum0_switches "$WORK/$a.tx")" = "$(field csum0_switches_expected "$WORK/$a.tx")" ] ||
        die "$a: csum0_switches=$(field csum0_switches "$WORK/$a.tx"), expected $(field csum0_switches_expected "$WORK/$a.tx")"
    [ "$zf" -eq "$RUNS" ] || die "$a: $zf runs' first fragments with checksum 0 on the wire, not $RUNS"
    [ "$zw" -eq $((sent0 - RUNS)) ] 2>/dev/null ||
        die "$a: $zw whole datagrams with checksum 0 on the wire, not the sender's $sent0 - $RUNS runs"
done

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "udp_offload_check: PASS - $EXPECTED datagrams identical in all arms (E: $REPEAT passes, $(field gro_commits "$WORK/E.rx") switches back), wire identical with send offload, IP fragments only the runs', runs whole under GRO, checksum 0 on the runs and exactly where the sender counted it (F, G)"
exit 0
