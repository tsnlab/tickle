#!/usr/bin/env bash
# hybrid_run_rx.sh - runs received for real (DESIGN.md section 8, "Hybrid transport", step 2): hand-built FRAG_RUN and
# FRAG_RUN_L datagrams above 1,472 bytes, IP-fragmented by the sender's kernel, reassembled by the receiver's, and put
# together by a TickLE context on the real Linux HAL. PC only, two private network namespaces joined by a veth (MTU
# 1500), the receiver with its own /dev/shm. Needs passwordless `sudo -n ip`.
#
# ARMS (hybrid_run_rx.c says what each sends):
#   runs  a 20,000-byte sample as one 13-unit FRAG_RUN + its last unit, a 100,000-byte one as FRAG_RUN_L of 45 and 23
#         units + its last unit: three datagrams above 1,472 bytes.
#   mtu   the control: the same two samples as MTU fragments only - nothing for IP to reassemble.
#
# READING RULES, enforced below:
#   runs PASS iff the receiver delivers both samples byte for byte (exit 0, small_ok=1 large_ok=1 torn=0), the sender
#     sent exactly 3 datagrams above 1,472 bytes, and the receiver namespace's Ip ReasmOKs rose by at least 3 - each
#     run arrived as IP fragments and was reassembled by the kernel, not merely sent.
#   mtu PASS iff both samples are delivered and ReasmOKs did not move: the rise in the runs arm is the runs', not the
#     namespace's or the HAL's.
#   Anything else is FAIL, and the run's logs are printed.
# Usage: hybrid_run_rx.sh     env: OUT (results file, default /tmp/hybrid_run_rx_<stamp>.txt)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT=${OUT:-/tmp/hybrid_run_rx_$(date +%Y%m%d-%H%M%S).txt}
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }

WORK=$(mktemp -d /tmp/hybrid_run_rx.XXXXXX) || exit 1
NSA=hrr-a-$$
NSB=hrr-b-$$
# shellcheck disable=SC2329 # invoked by the EXIT trap below
cleanup() {
    sudo -n ip netns del "$NSA" 2>/dev/null
    sudo -n ip netns del "$NSB" 2>/dev/null
    [ -n "${WORK:-}" ] && [ -d "$WORK" ] && rm -rf "$WORK"
    return 0
}
trap cleanup EXIT

BIN="$WORK/hybrid_run_rx"
if ! gcc -O2 -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -I"$REPO/include" -I"$REPO/src" \
    -o "$BIN" "$HERE/hybrid_run_rx.c" "$REPO/src/tickle.c" "$REPO/src/hal_linux.c" "$REPO/src/encoding.c" \
    "$REPO/src/log.c" -lm -lpthread >"$WORK/build.log" 2>&1; then
    say "FATAL build"
    cat "$WORK/build.log"
    exit 1
fi

reasm_ok() { # the receiver namespace's Ip ReasmOKs
    sudo -n ip netns exec "$NSB" cat /proc/net/snmp |
        awk '/^Ip:/ { if (!n) { for (i = 1; i <= NF; i++) col[$i] = i; n = 1 } else print $col["ReasmOKs"] }'
}

failed=0
for arm in runs mtu; do
    sudo -n ip netns del "$NSA" 2>/dev/null
    sudo -n ip netns del "$NSB" 2>/dev/null
    if ! { sudo -n ip netns add "$NSA" && sudo -n ip netns add "$NSB" &&
        sudo -n ip -n "$NSA" link add hrr0 type veth peer name hrr1 netns "$NSB" &&
        sudo -n ip -n "$NSA" addr add 10.231.7.1/24 brd + dev hrr0 &&
        sudo -n ip -n "$NSB" addr add 10.231.7.2/24 brd + dev hrr1 &&
        sudo -n ip -n "$NSA" link set lo up && sudo -n ip -n "$NSB" link set lo up &&
        sudo -n ip -n "$NSA" link set hrr0 up && sudo -n ip -n "$NSB" link set hrr1 up; }; then
        say "FATAL namespaces"
        exit 1
    fi
    before=$(reasm_ok)
    # shellcheck disable=SC2016,SC2024 # $1/$@ are the inner sh's; the log is this shell's, as intended
    sudo -n ip netns exec "$NSB" sh -c 'mount -t tmpfs -o size=64m tmpfs /dev/shm && exec "$@"' sh \
        "$BIN" recv 10.231.7.255 2 10 >"$WORK/recv_$arm.log" 2>&1 </dev/null &
    rpid=$!
    for _ in $(seq 1 50); do
        grep -q '^READY' "$WORK/recv_$arm.log" 2>/dev/null && break
        sleep 0.1
    done
    # shellcheck disable=SC2024 # the log is written by this shell, as the invoking user: intended
    sudo -n ip netns exec "$NSA" "$BIN" send 10.231.7.2 "$arm" >"$WORK/send_$arm.log" 2>&1
    wait "$rpid"
    rrc=$?
    after=$(reasm_ok)
    result=$(grep '^RESULT:' "$WORK/recv_$arm.log")
    above=$(sed -n 's/^SENT: .*datagrams_above_1472=\([0-9]*\).*/\1/p' "$WORK/send_$arm.log")
    delta=$((after - before))
    verdict=FAIL
    if [ "$arm" = runs ] && [ "$rrc" = 0 ] && [ "${above:-x}" = 3 ] && [ "$delta" -ge 3 ]; then
        verdict=PASS
    elif [ "$arm" = mtu ] && [ "$rrc" = 0 ] && [ "${above:-x}" = 0 ] && [ "$delta" = 0 ]; then
        verdict=PASS
    fi
    say "$verdict  $arm  receiver_rc=$rrc sent_above_1472=${above:-none} ReasmOKs ${before}->${after} (+$delta)  ${result:-no RESULT}"
    if [ "$verdict" != PASS ]; then
        failed=1
        cat "$WORK/recv_$arm.log" "$WORK/send_$arm.log"
    fi
done
say "results: $OUT"
exit "$failed"
