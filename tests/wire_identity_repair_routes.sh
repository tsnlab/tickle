#!/usr/bin/env bash
# The reliable send paths' routes (2026-10-23): a default context sends what its parent sent - every datagram, its
# bytes, its destination and the moment it left - except where the change means to send differently: a
# solicitation (tt_Publisher_request_ack()) made while something is batched in tx_buffer, whose batched content now
# goes where its flush tick would send it and no longer to the solicited peers. Captures
# tests/wire_identity_repair_routes.c in the core default build and rmw_tickle's large one (-Dtt_MAX_BUFFER_LENGTH=65507
# -Dtt_LARGE_SAMPLES=1), with the parent's tickle.c and this tree's, and checks for each build:
#   control    the parent against itself, two separate builds, both scenarios - identical, or the capture is not
#              deterministic and nothing below could decide;
#   identity   the scenario without batched content at a solicitation (ROUTE_BATCHED=0): this tree against the parent,
#              identical, bytes and times;
#   intended   the scenario with it (ROUTE_BATCHED=1): this tree DIFFERS from the parent, and only so - the same
#              submessages, byte for byte, in the same order, at the same times; every submessage whose destination
#              changed is the batched content (a discovery summary, the batching Publisher's sample), which the parent
#              sent to the solicited reader and this tree broadcasts; no HEARTBEAT, ACKNACK or reliable sample moved;
#   mutant     this tree with the flush before the solicitation removed, ROUTE_BATCHED=1 - identical to the parent, or
#              the scenario does not reach the change and the intended check shows nothing.
#
# Usage: tests/wire_identity_repair_routes.sh [PARENT]     PARENT defaults to 896d4a29, the change's parent: a commit,
# checked out in a temporary worktree, or a directory holding that tree. Prints one line per check, exits non-zero if
# any fails.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=${1:-896d4a29}
CC=${CC:-gcc}

TMP=$(mktemp -d)
PARENT_TREE="$TMP/parent"
added_worktree=0
cleanup() {
    if [ "$added_worktree" = 1 ]; then
        git -C "$ROOT" worktree remove --force "$PARENT_TREE" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

if [ -d "$PARENT" ]; then
    PARENT_TREE=$(cd "$PARENT" && pwd)
else
    git -C "$ROOT" worktree add --detach "$PARENT_TREE" "$PARENT" >/dev/null 2>&1
    added_worktree=1
fi

# capture TREE TICKLE_C NAME [CFLAGS...]: this tree's capture source against TREE's headers and the given tickle.c.
capture() {
    local tree=$1 tickle_c=$2 name=$3
    shift 3
    mkdir -p "$TMP/src_$name"
    cp "$ROOT/tests/wire_identity_repair_routes.c" "$TMP/src_$name/"
    "$CC" -O0 -g -Wall -Wextra -I"$tree/include" -I"$tree/src" -I"$tree/tests" -DTICKLE_C="\"$tickle_c\"" \
        -DWIRE_CAPTURE_OUT="\"$TMP/$name.bin\"" "$@" -o "$TMP/bin_$name" "$TMP/src_$name/wire_identity_repair_routes.c" \
        "$tree/src/encoding.c" "$tree/src/log.c" -lm 2>"$TMP/build_$name.log" || {
        cat "$TMP/build_$name.log" >&2
        return 1
    }
    (
        mkdir -p "$TMP/cwd"
        cd "$TMP/cwd"
        ulimit -v 2097152
        ulimit -f 102400
        timeout 300 "$TMP/bin_$name" >"$TMP/run_$name.log" 2>&1
    ) || {
        echo "FAIL  capture $name did not run:"
        tail -5 "$TMP/run_$name.log"
        return 1
    }
    [ -s "$TMP/$name.bin" ] || {
        echo "FAIL  capture $name wrote nothing"
        return 1
    }
}

failed=0
verdict() { # name expected(same|differ) a b
    local same=differ
    cmp -s "$3" "$4" && same=same
    if [ "$same" = "$2" ]; then
        printf 'PASS  %-18s %s\n' "$1" "$(wc -c <"$3") vs $(wc -c <"$4") bytes, $same as expected"
    else
        printf 'FAIL  %-18s %s\n' "$1" "expected $2, got $same"
        failed=1
    fi
}

# intended NAME PARENT_BIN THIS_BIN RUN: the submessage-level comparison the header describes; RUN names the capture
# whose log gives the batching Publisher's endpoint id.
intended() {
    local batched_id
    batched_id=$(grep -o 'batched_endpoint=[0-9]*' "$TMP/run_$4.log" | cut -d= -f2)
    if python3 - "$2" "$3" "${batched_id:-0}" <<'EOF'; then
import struct, sys
READER_IP = 0x0a000178
BATCHED = int(sys.argv[3])

def submessages(path):
    data = open(path, "rb").read()
    out, pos = [], 0
    while pos < len(data):
        length, ip, now = struct.unpack_from("<IIQ", data, pos)
        dgram = data[pos + 16:pos + 16 + length]
        pos += 16 + length
        if dgram[0] & 0x20:  # tt_SINGLE_MARKER_FLAG: one submessage, addressed to everyone
            out.append((now, dgram[3], 0xFF, dgram[4:], ip))
            continue
        off = 4
        while off + 4 <= len(dgram):
            kind, receiver, slen = struct.unpack_from("<BBH", dgram, off)
            if slen == 0:
                break
            body = dgram[off + 4:off + slen]
            # the classic and single forms pad alike; compare the body as sent
            out.append((now, kind, receiver, body, ip))
            off += slen
    return out

a, b = submessages(sys.argv[1]), submessages(sys.argv[2])
if len(a) != len(b):
    sys.exit(f"submessage count {len(a)} vs {len(b)}")
moved = 0
for x, y in zip(a, b):
    if x[:4] != y[:4]:
        sys.exit(f"a submessage differs beyond its destination: type {x[1]} at {x[0]} vs type {y[1]} at {y[0]}")
    if x[4] != y[4]:
        endpoint = struct.unpack_from("<I", x[3], 0)[0] if len(x[3]) >= 4 else None
        # a discovery summary (HEARTBEAT 6 of endpoint 0), or the batching Publisher's sample (DATA 2)
        batched_content = (x[1] == 6 and endpoint == 0) or (x[1] == 2 and endpoint == BATCHED)
        if not batched_content or x[4] != READER_IP or y[4] != 0:
            sys.exit(f"type {x[1]} endpoint {endpoint} moved {x[4]:#x} -> {y[4]:#x}")
        moved += 1
if moved == 0:
    sys.exit("no submessage moved: the scenario did not reach the change")
print(f"{len(a)} submessages identical, {moved} moved from the solicited reader to broadcast")
EOF
        printf 'PASS  %-18s %s\n' "$1" "only the batched content's destination changed"
    else
        printf 'FAIL  %-18s %s\n' "$1" "see above"
        failed=1
    fi
}

# mutate TAG OLD NEW: this tree's tickle.c with OLD (which must occur exactly once) replaced by NEW, as $TMP/TAG.c.
mutate() {
    python3 - "$ROOT/src/tickle.c" "$TMP/$1.c" "$2" "$3" <<'EOF'
import sys
text = open(sys.argv[1]).read()
if text.count(sys.argv[3]) != 1:
    sys.exit(f"mutant anchor matched {text.count(sys.argv[3])} times, not 1")
open(sys.argv[2], "w").write(text.replace(sys.argv[3], sys.argv[4]))
EOF
}
mutate mutant "    if (pub->node->tx_tail != sizeof(struct tt_Header)) {
        flush_batched(pub->node);
    }
    encode_and_send_heartbeat(pub->node, pub, first_seq_no, live_peers, live_count, 0);" \
    "    encode_and_send_heartbeat(pub->node, pub, first_seq_no, live_peers, live_count, 0);"

for build in core large; do
    flags=()
    [ "$build" = large ] && flags=(-Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1)
    for batched in 0 1; do
        b=("-DROUTE_BATCHED=$batched")
        capture "$PARENT_TREE" "$PARENT_TREE/src/tickle.c" "${build}${batched}_parent_a" "${flags[@]}" "${b[@]}"
        capture "$PARENT_TREE" "$PARENT_TREE/src/tickle.c" "${build}${batched}_parent_b" "${flags[@]}" "${b[@]}"
        capture "$ROOT" "$ROOT/src/tickle.c" "${build}${batched}_this" "${flags[@]}" "${b[@]}"
        verdict "$build/$batched control" same "$TMP/${build}${batched}_parent_a.bin" "$TMP/${build}${batched}_parent_b.bin"
    done
    capture "$ROOT" "$TMP/mutant.c" "${build}1_mutant" "${flags[@]}" -DROUTE_BATCHED=1
    printf '      %-18s %s\n' "$build" "$(grep -o 'wire capture: [0-9]* datagrams' "$TMP/run_${build}1_this.log")"
    verdict "$build identity" same "$TMP/${build}0_this.bin" "$TMP/${build}0_parent_a.bin"
    verdict "$build differs" differ "$TMP/${build}1_this.bin" "$TMP/${build}1_parent_a.bin"
    intended "$build intended" "$TMP/${build}1_parent_a.bin" "$TMP/${build}1_this.bin" "${build}1_this"
    verdict "$build mutant" same "$TMP/${build}1_mutant.bin" "$TMP/${build}1_parent_a.bin"
done

cleanup # the trap does it too, on any earlier exit
trap - EXIT
exit "$failed"
