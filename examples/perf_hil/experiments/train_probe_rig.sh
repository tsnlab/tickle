#!/usr/bin/env bash
# train_probe_rig.sh - runs train_probe (train_probe.c) across a shaped link: what the reader's receive path makes of a packet
# train's spacing (adaptive_pacing/DESIGN.md 27.6). Two commands:
#
#   train_probe_rig.sh build OUT        build the probe against this checkout's HAL (run on each host; OUT = the binary)
#   train_probe_rig.sh run              the sweep below, printing every cell's result to stdout (and OUTDIR/*.txt)
#
# `run` is driven from any host that can reach both ends, through a command prefix per end (empty = this host), so it fits
# the tc agent's netns rig: the sender and the receiver each run in their namespace, the link between them shaped to a rate
# by RATE_CMD. All settings are environment variables:
#
#   SEND_EXEC / RECV_EXEC  prefix that runs a command at that end, e.g. "ssh pi1 sudo ip netns exec wtc_a"
#   SEND_PROBE / RECV_PROBE  the binary at that end (default /tmp/train_probe)
#   SEND_BCAST / RECV_BCAST  the broadcast address of the interface the HAL binds there (BENCH_BROADCAST)
#   RECV_IP                the receiver's address as the sender reaches it
#   RATE_CMD               a command run with the link rate in kbit/s as its only argument, which shapes the link (both
#                          directions, or the data direction); it must return when the rate is in force. Empty: the
#                          link is left as it is and RATES names its rate for the ideal span (RATE_KBIT_FIXED)
#   RATES                  kbit/s list, default "4000 30000 100000 200000"
#   TRAIN_SIZES            datagrams per train, default "10 20 40"
#   TRAINS                 trains per cell, default 200
#   OVERHEAD               bytes the shaper counts on top of the UDP payload per datagram (default 42: IP, UDP, Ethernet)
#   OUTDIR                 where each cell's output goes (default ./train_probe_out)
#   SEND_HOW               hal (tt_send_batch(): sendmmsg, UDP_SEGMENT when the HAL turns it on) or plain; default hal
#
# Cells per rate and train size: the receiver by the HAL (TT_UDP_OFFLOAD=0 and 1, each with and without --drain), and by a
# plain socket one datagram at a time (each), in batches of 32 (batch), and with UDP_GRO (each-gro). The sender sends
# TRAINS trains, one per PERIOD ms, PERIOD at least twice the train's time on the link so trains never overlap.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

if [ "${1:-}" = build ]; then
    out="${2:?usage: train_probe_rig.sh build OUT}"
    # the library's HAL as the reader's build has it for large samples (drain and receive offload compiled in)
    gcc -O2 -I"$REPO/include" -I"$REPO/src" -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -w -o "$out" \
        "$HERE/train_probe.c" "$REPO/src/tickle.c" "$REPO/src/hal_linux.c" "$REPO/src/encoding.c" "$REPO/src/log.c" -lm -lpthread
    echo "built $out"
    exit 0
fi
[ "${1:-}" = run ] || {
    echo "usage: train_probe_rig.sh build OUT | run" >&2
    exit 2
}

SEND_EXEC=${SEND_EXEC:-}
RECV_EXEC=${RECV_EXEC:-}
SEND_PROBE=${SEND_PROBE:-/tmp/train_probe}
RECV_PROBE=${RECV_PROBE:-/tmp/train_probe}
SEND_BCAST=${SEND_BCAST:-127.255.255.255}
RECV_BCAST=${RECV_BCAST:-127.255.255.255}
RECV_IP=${RECV_IP:-127.0.0.1}
RATE_CMD=${RATE_CMD:-}
RATES=${RATES:-"4000 30000 100000 200000"}
TRAIN_SIZES=${TRAIN_SIZES:-"10 20 40"}
TRAINS=${TRAINS:-200}
OVERHEAD=${OVERHEAD:-42}
OUTDIR=${OUTDIR:-./train_probe_out}
SEND_HOW=${SEND_HOW:-hal}
DGRAM=1468
mkdir -p "$OUTDIR"

# cell RATE N LABEL OFFLOAD RECV-ARGS...: one receiver and one sender, the receiver's report to OUTDIR/LABEL.
cell() {
    local rate=$1 count=$2 label=$3 offload=$4
    shift 4
    local ideal_ms period out
    ideal_ms=$(((count - 1) * (DGRAM + OVERHEAD) * 8 / rate + 1))
    period=$((ideal_ms * 2 + 50))
    [ "$period" -lt 100 ] && period=100
    out="$OUTDIR/${label}_r${rate}_n${count}.txt"
    : > "$out" # the port line is read from this file at once: create it before the (backgrounded) receiver does
    # shellcheck disable=SC2086 # the prefixes are command words
    $RECV_EXEC env BENCH_BROADCAST="$RECV_BCAST" TT_UDP_OFFLOAD="$offload" "$RECV_PROBE" recv --n "$count" --size "$DGRAM" \
        --trains "$TRAINS" --rate-kbit "$rate" --overhead "$OVERHEAD" "$@" > "$out" 2>&1 &
    local receiver=$!
    local port=""
    for _ in $(seq 1 100); do
        port=$(sed -n 's/^port=//p' "$out" | head -1)
        [ -n "$port" ] && break
        sleep 0.1
    done
    if [ -z "$port" ]; then
        echo "FAIL  $label r=$rate n=$count: the receiver never printed its port" >&2
        kill "$receiver" 2> /dev/null || true
        return 1
    fi
    # shellcheck disable=SC2086
    $SEND_EXEC env BENCH_BROADCAST="$SEND_BCAST" TT_UDP_OFFLOAD="$offload" "$SEND_PROBE" send --to "$RECV_IP" --port "$port" \
        --n "$count" --size "$DGRAM" --trains "$TRAINS" --period-ms "$period" --how "$SEND_HOW"
    wait "$receiver" || true
    echo "=== $label rate=${rate}kbit n=$count period=${period}ms"
    cat "$out"
}

for rate in $RATES; do
    if [ -n "$RATE_CMD" ]; then
        $RATE_CMD "$rate"
    fi
    for count in $TRAIN_SIZES; do
        for offload in 0 1; do
            cell "$rate" "$count" "hal_offload${offload}" "$offload" --mode hal
            cell "$rate" "$count" "hal_offload${offload}_drain" "$offload" --mode hal --drain
        done
        cell "$rate" "$count" each 1 --mode each
        cell "$rate" "$count" batch 1 --mode batch
        cell "$rate" "$count" each_gro 1 --mode each-gro
    done
done
echo "done: $OUTDIR"
