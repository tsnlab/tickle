#!/usr/bin/env bash
# The same-host integration suite with the shared-memory module ON and OFF, so a figure from the ON
# arm is read against this machine's own OFF arm rather than against a CI runner's.
#
# Why it exists: on 2026-09-29 the module turned CI's same-host perf cell from 1,018 Mbps and 0%
# loss into 147 Mbps and 97.6% loss, and the tier reported PASS throughout because its criterion is
# "received >= 5 messages" against a run that delivers five million. The numbers inside a passing
# tier are the measurement; the pass is not.
#
# Pre-registered reading, written before the run:
#   ON ~= OFF on loss and throughput          -> the module no longer reorders the stream. What a fix
#                                                has to achieve before stage 1 is measurable at all.
#   ON loses materially more than OFF         -> records are still arriving out of order behind the
#                                                socket; the remaining mixed-stream paths are the
#                                                place to look (oversize services, pre-attach UDP).
#   ON throughput below OFF with loss equal   -> a cost, not a correctness problem: the ring or the
#                                                drain, not the ordering.
#   either arm's subscriber delivering ~0     -> VOID, the run says nothing; do not read the rest.
# out_of_order_discarded is reported beside the RESULT line because it is the quantity that
# distinguishes these, and reading only avg_mbps is how the collapse survived a day.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT=${OUT:-/tmp/samehost_ordering_ab.log}

: >"$OUT"
for arm in on off; do
    case "$arm" in
    on) flags="" ;;
    off) flags="-Dtt_SEGMENT_ENABLED=0" ;;
    *) exit 1 ;;
    esac
    {
        echo "=== arm=$arm flags='${flags:-none}' $(date -Is) ==="
        make -C "$REPO/platform/linux" clean
        # The flag has to reach the compiler, and "the flag changed nothing" and "the flag never
        # arrived" look identical from the numbers alone - so the library's hash is recorded too.
        make -C "$REPO" test-linux CPPFLAGS="$flags" 2>&1
        echo "libtickle.a sha256: $(sha256sum "$REPO/platform/linux/libtickle.a" 2>/dev/null | cut -c1-16)"
        echo "=== arm=$arm done ==="
    } >>"$OUT" 2>&1
done

# The tree is left in the LAST arm's configuration otherwise, and the objects it leaves behind are
# not all rebuilt by the next ordinary `make`: a source file that did not change keeps the object
# compiled with -Dtt_SEGMENT_ENABLED=0, and the archive keeps it too. That produced undefined
# references to tt_segment_detach in a build that had nothing to do with this script.
make -C "$REPO/platform/linux" clean >/dev/null 2>&1

echo "=== summary ==="
awk '
/^=== arm=/ { arm = $0; sub(/^=== arm=/, "", arm); sub(/ .*/, "", arm) }
/out_of_order_discarded=/ {
    line = $0
    if (match(line, /delivered=[0-9]+/))              { d = substr(line, RSTART+10, RLENGTH-10) }
    if (match(line, /out_of_order_discarded=[0-9]+/)) { o = substr(line, RSTART+23, RLENGTH-23) }
    if (d + 0 > 1000) { printf "%-4s delivered=%-10s out_of_order_discarded=%s\n", arm, d, o }
}
/^perf  *RESULT:/ { printf "%-4s %s\n", arm, $0 }
/libtickle.a sha256:/ { printf "%-4s %s\n", arm, $0 }
' "$OUT"
echo "full log: $OUT"
