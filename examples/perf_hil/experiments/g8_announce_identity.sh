#!/usr/bin/env bash
# g8_announce_identity.sh - does a lone rmw_tickle context send the same bytes with g8 as before it? (RMW_GAPS_PLAN.md
# g8, pre-registered 2026-09-28: "a one-process rmw_tickle context takes the same id and sends announce bytes identical
# to its parent's".)
#
# One talker process alone in a netns, its datagrams captured on the far end of the veth for its whole 4 s life - the
# announces and summaries of a context with a publisher that never publishes. Three arms, each a librmw_tickle.so copied over this checkout's
# installed one (md5 checked, the original put back on exit): A = the parent, A2 = the parent again (the control: what
# differs between two runs of the same bytes - timestamps, the random entity-id base), B = g8.
# Reading, fixed before the run: B is identical to A where A2 is identical to A - the same datagram count and lengths,
# the same source id, and every byte position where B differs from A also differs between A2 and A.
# Amended after the first runs, which showed the parent alone sending its startup either as one datagram (summary and
# first list together, 156 B) or as two (24 + 128 B), whichever of rmw's poll thread and the first publisher wins: the
# three runs are compared from the end, over the datagrams all three have, and the startup framing is reported, not
# judged. A and A2 differing in it is the control showing the race is the parent's own. And a time field's high byte
# can match between A and A2 in one datagram by chance, so a header-area position that varies in any datagram of the
# control counts as varying in all of them.
#
# Usage: g8_announce_identity.sh <parent lib> <g8 lib>     Results: $OUT (default /tmp/g8_announce_identity)
# IFACES: an interface overlay's local_setup.bash with std_msgs and rcl_interfaces for rmw_tickle (default: the
# acceptance workspace's, /tmp/dev_accept/ifaces/install/local_setup.bash).
set -u
LIB_A=${1:?usage: g8_announce_identity.sh <parent lib> <g8 lib>}
LIB_B=${2:?}
OUT=${OUT:-/tmp/g8_announce_identity}
IFACES=${IFACES:-/tmp/dev_accept/ifaces/install/local_setup.bash}
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
LIB="$REPO/install/rmw_tickle/lib/librmw_tickle.so"
KEEP="$(mktemp /tmp/g8_announce_keep.XXXXXX)"
cp "$LIB" "$KEEP"
NS1=g8an1-$$
NS2=g8an2-$$
mkdir -p "$OUT/home"
cleanup() {
    for ns in "$NS1" "$NS2"; do
        for p in $(sudo -n ip netns pids "$ns" 2>/dev/null); do sudo -n ip netns exec "$ns" kill -TERM "$p" 2>/dev/null; done
        sudo -n ip netns del "$ns" 2>/dev/null
    done
    cp "$KEEP" "$LIB" && rm -f "$KEEP"
    return 0
}
trap cleanup EXIT
cat >"$OUT/talker.py" <<'PY'
import time, rclpy
from std_msgs.msg import String
rclpy.init()
node = rclpy.create_node('g8_talker')
pub = node.create_publisher(String, '/g8_chatter', 10)
end = time.time() + 4.0
while time.time() < end:
    rclpy.spin_once(node, timeout_sec=0.05)
print('RESULT: ran', flush=True)
PY
run_arm() { # ARM LIB
    local arm=$1 lib=$2
    cleanup >/dev/null 2>&1
    cp "$KEEP.orig" "$KEEP" 2>/dev/null
    cp "$lib" "$LIB" || exit 1
    [ "$(md5sum <"$LIB")" = "$(md5sum <"$lib")" ] || { echo "arm $arm: the copy did not take" >&2; exit 1; }
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add g8av1 netns "$NS1" type veth peer name g8av2 netns "$NS2" || exit 1
    sudo -n ip -n "$NS1" addr add 10.80.0.1/24 dev g8av1; sudo -n ip -n "$NS2" addr add 10.80.0.2/24 dev g8av2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set g8av1 up; sudo -n ip -n "$NS2" link set g8av2 up
    sudo -n ip netns exec "$NS2" timeout 8 tcpdump -i g8av2 -w "$OUT/$arm.pcap" -U udp >/dev/null 2>&1 &
    local cap=$!
    sleep 1
    # shellcheck disable=SC2024 # the log is this shell's
    sudo -n ip netns exec "$NS1" sudo -n -u "$USER" bash -c "set +u; source /opt/ros/lyrical/setup.bash; source $REPO/install/setup.bash; source $IFACES; export ROS_DOMAIN_ID=94 RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=10.80.0.255 ROS_LOG_DIR=$OUT/roslog HOME=$OUT/home; python3 $OUT/talker.py" >"$OUT/$arm.log" 2>&1
    wait "$cap"
    grep -q '^RESULT: ran' "$OUT/$arm.log" || { echo "arm $arm: the talker did not run (see $OUT/$arm.log)" >&2; exit 1; }
}
cp "$KEEP" "$KEEP.orig"
run_arm A "$LIB_A"
run_arm A2 "$LIB_A"
run_arm B "$LIB_B"
rm -f "$KEEP.orig"
python3 - "$OUT" <<'PY' | tee "$OUT/summary.txt"
import struct, sys
out = sys.argv[1]
def datagrams(path):
    data = open(path, 'rb').read()
    pos, first, result = 24, None, []
    while pos + 16 <= len(data):
        sec, usec, incl, _ = struct.unpack_from('<IIII', data, pos)
        frame = data[pos + 16:pos + 16 + incl]
        pos += 16 + incl
        t = sec + usec / 1e6
        if frame[12:14] != b'\x08\x00':
            continue
        ihl = (frame[14] & 0x0f) * 4
        payload = frame[14 + ihl + 8:]
        first = t if first is None else first
        result.append(payload)
    return result
a, a2, b = (datagrams(f'{out}/{arm}.pcap') for arm in ('A', 'A2', 'B'))
def positions(x, y):
    return [{i for i in range(min(len(p), len(q))) if p[i] != q[i]} for p, q in zip(x, y)]
print(f'datagrams: A={len(a)} A2={len(a2)} B={len(b)}')
print(f'lengths: A={[len(p) for p in a]}')
print(f'         A2={[len(p) for p in a2]}')
print(f'         B={[len(p) for p in b]}')
print(f'source id (byte 3 or 2): A={sorted({p[3] if p[:2] in (b"KT", b"TK") else p[2] for p in a})} '
      f'B={sorted({p[3] if p[:2] in (b"KT", b"TK") else p[2] for p in b})}')
print(f'startup framing: A={[len(p) for p in a[:len(a) - min(len(a), len(a2), len(b)) + 1]]} '
      f'A2={[len(p) for p in a2[:len(a2) - min(len(a), len(a2), len(b)) + 1]]} '
      f'B={[len(p) for p in b[:len(b) - min(len(a), len(a2), len(b)) + 1]]}')
common = min(len(a), len(a2), len(b)) - 1 # the first datagram may be either framing
a, a2, b = a[-common:], a2[-common:], b[-common:]
print(f'compared from the end: {common} datagrams each')
same_shape = [len(p) for p in a] == [len(p) for p in b] == [len(p) for p in a2]
control = positions(a, a2)
test = positions(a, b)
# A time field can match between two runs by chance in one datagram - its high byte moves every few seconds - so a
# position in the header area (the first 32 bytes, laid out alike in every datagram) that varies between A and A2 in
# any datagram counts as varying in all of them.
header_varying = {i for c in control for i in c if i < 32}
extra = [sorted(t - c - header_varying) for t, c in zip(test, control)]
print(f'positions differing A/A2 per datagram: {[sorted(c) for c in control]}')
print(f'positions differing A/B per datagram:  {[sorted(t) for t in test]}')
verdict = same_shape and all(not e for e in extra)
print('VERDICT:', 'IDENTICAL (B differs from A only where A2 does)' if verdict else f'DIFFERENT (beyond the control: {extra})')
PY
