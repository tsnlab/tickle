#!/usr/bin/env bash
# g15_zerocopy_e2e.sh - does tx_udp count every datagram publish_zerocopy() hands the real Linux HAL? (RMW_GAPS g15)
#
# g15 (2026-09-29): publish_zerocopy() called tt_send_iov() directly and incremented nothing, so five datagrams left
# while tx_datagrams said none had. The transport seam fixed it by moving the counting into the seam_send* wrappers,
# and tests/test_transport_seam.c pins it against the MOCK HAL's call count. This is the same claim on the real HAL and
# the real kernel, which S1's numbers are read from: the KERNEL's count of packets the interface transmitted against
# the context's own tx_udp, over a window in which the zero-copy path sent every sample.
#
# g15_zerocopy_e2e.c is the publisher (why not a perf_hil bench: its header says). Two shapes of the zero-copy path:
#   iov   default build, a 1440-byte body: one datagram per sample, publish_zerocopy() -> seam_send_iov() -> sendmsg().
#   frag  -Dtt_MAX_SAMPLE_LENGTH=4096, a 2800-byte body (p4's): two fragments per sample, publish_zerocopy() ->
#         send_fragments() -> seam_send_batch() -> sendmmsg().
# and for each a MUTANT with that shape's own counting removed - the iov arm's count_udp() in seam_send_iov(), the frag
# arm's in seam_send_batch() - which MUST fail the check, or the check cannot decide.
#
# HOW TO READ IT, written before the first run and enforced below, per arm:
#   WITNESS  inplace_encodes == published == COUNT: the in-place encode ran for every sample, and only
#            try_publish_zerocopy() calls it, so the zero-copy path sent them all. Otherwise the arm is VOID.
#   PASS     |tx_packets - tx_udp| <= 2 and tx_udp >= datagrams per sample x COUNT (the interface saw what was counted).
#   g15 CLOSED end to end iff both real arms PASS and both mutants FAIL. A real arm FAIL = an uncounted send path
#   today. A mutant that PASSES = VOID: the comparison cannot see the counting it exists to check.
# Usage: g15_zerocopy_e2e.sh      env: COUNT (20000) OUT (~/rig_results_safe/g15_zerocopy_e2e/<stamp>)
# PC only, no rig. The publisher runs in a private network namespace whose veth leads to another, empty one.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
COUNT=${COUNT:-20000}
OUTD=${OUT:-$HOME/rig_results_safe/g15_zerocopy_e2e/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUTD" || exit 1
WORK=$(mktemp -d /tmp/g15_e2e.XXXXXX) || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
# shellcheck disable=SC2329 # invoked by the EXIT trap
cleanup() {
    local ns
    while read -r ns; do [ -n "$ns" ] && sudo -n ip netns del "$ns" 2>/dev/null; done <"$NS_LIST"
    [ -n "${WORK:-}" ] && [ -d "$WORK" ] && rm -rf "$WORK"
    return 0
}
trap cleanup EXIT
say() { echo "$*" | tee -a "$OUTD/summary.txt"; }
say "=== g15 zero-copy end to end $(date -Is), tree $(git -C "$REPO" rev-parse --short HEAD), $COUNT samples an arm ==="

# The core's sources, mutated or not, compiled straight into the publisher (as platform/linux/Makefile does).
build() { # $1 name, $2 extra cflags, $3 tickle.c to use
    # shellcheck disable=SC2086 # $2 is a list of flags, deliberately word-split
    gcc -O2 -Wall -Wextra $2 -I"$REPO/include" -I"$REPO/src" -o "$WORK/$1" "$HERE/g15_zerocopy_e2e.c" "$3" \
        "$REPO/src/encoding.c" "$REPO/src/log.c" "$REPO/src/hal_linux.c" -lpthread -lm >"$OUTD/build_$1.log" 2>&1
}
mutate() { # $1 out file, $2 old, $3 new: exactly one occurrence, or the mutant is refused
    python3 - "$REPO/src/tickle.c" "$1" "$2" "$3" <<'EOF'
import sys
src, out, old, new = sys.argv[1], sys.argv[2], sys.argv[3].encode().decode("unicode_escape"), sys.argv[4].encode().decode("unicode_escape")
s = open(src).read()
if s.count(old) != 1:
    sys.exit("mutant anchor found %d times, not once" % s.count(old))
open(out, "w").write(s.replace(old, new))
EOF
}
mutate "$WORK/tickle_iov_mutant.c" \
    '    count_udp(node, reason, 1);\n    return tt_send_iov(node, hdr, hdr_len, body, body_len, ip, port);' \
    '    (void)reason;\n    return tt_send_iov(node, hdr, hdr_len, body, body_len, ip, port);' ||
    { say "FATAL: the iov mutant could not be made"; exit 1; }
mutate "$WORK/tickle_frag_mutant.c" \
    '        count_udp(node, reason, 1);\n        remaining[left++] = datagrams[i];' \
    '        remaining[left++] = datagrams[i];' ||
    { say "FATAL: the frag mutant could not be made"; exit 1; }
FRAG_FLAGS="-Dtt_MAX_SAMPLE_LENGTH=4096"
if ! { build iov "" "$REPO/src/tickle.c" && build frag "$FRAG_FLAGS" "$REPO/src/tickle.c" &&
    build iov_mutant "" "$WORK/tickle_iov_mutant.c" && build frag_mutant "$FRAG_FLAGS" "$WORK/tickle_frag_mutant.c"; }; then
    say "FATAL: a build failed (see $OUTD/build_*.log)"
    exit 1
fi

field() { grep -oE "(^| )$2=[^ ]*" <<<"$1" | head -1 | sed 's/^ //; s/^[^=]*=//'; }
verdicts=""
id=0
for arm in iov frag iov_mutant frag_mutant; do
    id=$((id + 1))
    nsa="g15$$-${id}a" nsb="g15$$-${id}b" va="g15$$v${id}a" vb="g15$$v${id}b"
    printf '%s\n%s\n' "$nsa" "$nsb" >>"$NS_LIST"
    if ! { sudo -n ip netns add "$nsa" && sudo -n ip netns add "$nsb" &&
        sudo -n ip link add "$va" type veth peer name "$vb" &&
        sudo -n ip link set "$va" netns "$nsa" && sudo -n ip link set "$vb" netns "$nsb" &&
        sudo -n ip -n "$nsa" addr add 192.168.10.1/24 brd + dev "$va" &&
        sudo -n ip -n "$nsb" addr add 192.168.10.2/24 brd + dev "$vb" &&
        sudo -n ip -n "$nsa" link set lo up && sudo -n ip -n "$nsa" link set "$va" up &&
        sudo -n ip -n "$nsb" link set "$vb" up; }; then
        say "FATAL: cannot build the veth pair"
        exit 1
    fi
    body=1440 per=1
    case "$arm" in frag*) body=2800 per=2 ;; esac
    # Its own /dev/shm too, as every PC run here has, so nothing it does reaches the host's.
    # shellcheck disable=SC2016,SC2024 # $1..$4 are the inner sh's; the log is this shell's, as intended
    sudo -n ip netns exec "$nsa" sh -c 'mount -t tmpfs -o size=16m tmpfs /dev/shm && exec "$1" "$2" "$3" "$4"' \
        sh "$WORK/$arm" "$va" "$body" "$COUNT" >"$OUTD/$arm.log" 2>&1
    sudo -n ip netns del "$nsa" 2>/dev/null
    sudo -n ip netns del "$nsb" 2>/dev/null
    line=$(grep '^G15:' "$OUTD/$arm.log" | head -1)
    published=$(field "$line" published) inplace=$(field "$line" inplace_encodes)
    tx_udp=$(field "$line" tx_udp) packets=$(field "$line" tx_packets)
    if [ -z "$line" ] || [ -z "$packets" ]; then
        say "$arm: VOID - no G15 line (see $OUTD/$arm.log)"
        verdicts="$verdicts $arm=VOID"
        continue
    fi
    say "$arm: $line"
    if [ "$inplace" != "$COUNT" ] || [ "$published" != "$COUNT" ]; then
        verdicts="$verdicts $arm=VOID(no-zerocopy)"
    elif [ $((packets - tx_udp)) -le 2 ] && [ $((tx_udp - packets)) -le 2 ] && [ "$tx_udp" -ge $((per * COUNT)) ]; then
        verdicts="$verdicts $arm=PASS"
    else
        verdicts="$verdicts $arm=FAIL"
    fi
done
case "$verdicts" in
" iov=PASS frag=PASS iov_mutant=FAIL frag_mutant=FAIL") say "VERDICT g15 CLOSED end to end:$verdicts" ;;
" iov=FAIL"* | *" frag=FAIL"*) say "VERDICT g15 OPEN - an uncounted send path today:$verdicts" ;;
*) say "VERDICT VOID - the check could not decide:$verdicts" ;;
esac
