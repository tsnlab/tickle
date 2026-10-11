#!/bin/bash
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# q(n) on a FreeRTOS + lwIP receiver (QEMU RISC-V, virtio-net): does a UDP datagram of n MTU units (n * 1446 B,
# IP-fragmented above 1472 B) survive with q(n) = (1-p)^n under independent frame loss p, and where do lwIP's
# reassembly limits (IP_REASS_MAX_PBUFS, IP_REASS_MAXAGE, MEMP_NUM_REASSDATA, the pbuf pool) cut it short?
#
# Wiring: one private netns (frq_fr_<pid>) holds a tap device; QEMU (the test tier's image with frag_rx.c as its main,
# freertos_frag/) uses it as its virtio-net backend, and the Linux sender (freertos_frag_send.py) in the same netns
# sends broadcast datagrams out of it. netem on the tap's root qdisc is the sender's egress loss, applied per IP
# fragment. The guest boots fresh for every cell, so no incomplete reassembly carries over between cells. Nothing runs
# in the default netns; everything inside the netns runs through `sudo -n ip netns exec`.
#
# The reading rules (F1-F4, the A/A control, the treatment check) are in freertos_frag_analyze.py, written and
# committed before the run. This script only produces cells.tsv and one guest/sender log per cell.
#
# usage: freertos_frag_q.sh <outdir> smoke
#        freertos_frag_q.sh <outdir> probe                 n=1, p=0 at a ladder of rates: the guest's no-drop ceiling
#        freertos_frag_q.sh <outdir> main <high_rate_Bps>  the full grid at 1 MB/s and <high_rate_Bps>
set -u
OUT=$1
MODE=$2
HI=${3:-0}
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${FRAG_BUILD:-/home/semih/tmp_build/frag_fr}
ELF=$BUILD/out/frag_rx.elf
ELF_POOL16=$BUILD/out/frag_rx-pool16.elf
NS=frq_fr_$$
TAP=frtap0
GUEST_IP=192.168.10.255 # broadcast, as TickLE sends; the guest is 192.168.10.2 and has no ARP
LO=1000000
mkdir -p "$OUT/cells"
TSV=$OUT/cells.tsv

cleanup() {
    for pid in $(sudo -n ip netns pids "$NS" 2>/dev/null); do
        sudo -n ip netns exec "$NS" kill -KILL "$pid" 2>/dev/null
    done
    sudo -n ip netns del "$NS" 2>/dev/null
    echo "cleanup: netns $NS removed (pids in it were killed)"
}
trap cleanup EXIT

[ -x "$ELF" ] || { echo "FATAL no guest image $ELF"; exit 3; }
sudo -n ip netns add "$NS" || { echo "FATAL cannot create netns $NS"; exit 3; }
nsx() { sudo -n ip netns exec "$NS" "$@"; }
nsx ip link set lo up
nsx ip tuntap add "$TAP" mode tap || { echo "FATAL tuntap"; exit 3; }
nsx ip link set "$TAP" txqueuelen 1000
nsx sysctl -qw "net.ipv6.conf.$TAP.disable_ipv6=1" # no IPv6 chatter in the netem counters
nsx ip addr add 192.168.10.100/24 dev "$TAP"
nsx ip link set "$TAP" up
echo "netns $NS tap $TAP up: $(nsx ip -br addr show dev "$TAP")"
[ -f "$TSV" ] || printf 'cell\tlabel\telf\trate_Bps\tn\tp\tsent\tgot\tlog\n' >"$TSV"

CELL=0
# run_cell <label> <elf> <rate_Bps> <n> <p> <seconds>
run_cell() {
    local label=$1 elf=$2 rate=$3 n=$4 p=$5 secs=$6
    CELL=$((CELL + 1))
    local id=$((CELL + 1000))
    local count
    count=$(python3 -c "print(max(50, round($rate * $secs / ($n * 1446))))")
    local base=$OUT/cells/c${id}_${label}_r${rate}_n${n}_p${p}
    local pct
    pct=$(python3 -c "print(f'{$p * 100:g}')")
    nsx tc qdisc replace dev "$TAP" root netem loss "${pct}%" limit 10000 || { echo "FATAL tc"; exit 3; }
    : >"$base.guest"
    sudo -n ip netns exec "$NS" timeout $((secs + 60)) qemu-system-riscv32 -machine virt -nographic -bios none \
        -kernel "$elf" -global virtio-mmio.force-legacy=off \
        -netdev tap,id=net0,ifname="$TAP",script=no,downscript=no -device virtio-net-device,netdev=net0 \
        </dev/null >"$base.guest" 2>&1 &
    local qpid=$!
    local t=0
    until grep -q 'frag_rx: ready' "$base.guest"; do
        sleep 0.2
        t=$((t + 1))
        if [ $t -ge 150 ] || ! kill -0 "$qpid" 2>/dev/null; then
            echo "cell $id: guest not ready after $((t / 5)) s"
            break
        fi
    done
    nsx python3 "$HERE/freertos_frag_send.py" "$TAP" "$GUEST_IP" "$id" "$n" "$rate" "$count" "$base.guest" 20 \
        >"$base.send" 2>&1
    wait "$qpid"
    local got
    got=$(sed -n 's/^frag_rx: final .* got=\([0-9]*\) .*/\1/p' "$base.guest" | tr -d '\r')
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$id" "$label" "$(basename "$elf")" "$rate" "$n" "$p" "$count" \
        "${got:-NA}" "$base" >>"$TSV"
    echo "$(date +%T) cell $id $label rate=$rate n=$n p=$p sent=$count got=${got:-NA} $(grep '^send: done' "$base.send" | sed 's/.*achieved/achieved/')"
}

python3 "$HERE/freertos_frag_analyze.py" plan "$HI"
case $MODE in
smoke)
    run_cell smoke "$ELF" $LO 1 0 3
    run_cell smoke "$ELF" $LO 2 0 3
    run_cell smoke "$ELF" $LO 9 0.01 3
    ;;
probe)
    for r in 1000000 2000000 4000000 6000000 8000000 12000000 16000000 24000000 32000000; do
        run_cell probe "$ELF" $r 1 0 8
    done
    ;;
main)
    [ "$HI" -gt 0 ] || { echo "FATAL main needs the high rate"; exit 2; }
    run_cell aa1 "$ELF" $LO 4 0.01 30
    for rate in $LO "$HI"; do
        for p in 0 0.001 0.01 0.05; do
            for n in 1 2 4 8 9; do
                secs=30
                [ "$p" = 0 ] && secs=10
                [ "$n" = 9 ] && secs=10
                run_cell grid "$ELF" "$rate" $n $p $secs
            done
        done
    done
    run_cell aa2 "$ELF" $LO 4 0.01 30
    if [ -x "$ELF_POOL16" ]; then
        for n in 8 9 10 11; do
            run_cell pool16 "$ELF_POOL16" $LO $n 0 10
        done
    fi
    ;;
*) echo "unknown mode $MODE"; exit 2 ;;
esac
echo "DONE mode=$MODE cells=$CELL"
