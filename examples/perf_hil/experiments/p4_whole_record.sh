#!/usr/bin/env bash
# p4_whole_record.sh - does not fragmenting an all-local sample recover p4's 92.6%?
#
# THE CELL. COMPARISON 2.2c: on the same-host shared-memory path at p4 (2800 B), TickLE carries 2,745 Mbps
# against CycloneDDS's 5,287 - a 92.6% gap, and the only separable loss in that table. SHM_PLAN 6e traced it
# to a mechanical cause: tt_SEGMENT_SLOT_BYTES was the Ethernet UDP payload, so a 2800-byte sample took two
# slots on a path that has no MTU. 6e(a) raises the limit to the destination's slot when every destination is
# a same-host peer whose segment is already attached. This measures whether that did anything.
#
# WHY TWO SHAs AND NOT "RE-RUN AND COMPARE TO 2.2c". The 2,745 was measured at the 512 KiB segment default,
# 256 slots. 5ee4b7e6 moved the default to 768 KiB and 512 slots hours before this change landed, so a figure
# taken now against that record would differ in TWO ways and attribute both to 6e(a). Both arms are therefore
# built here, at the same default, differing only in whether 6e(a) is present.
#
# HOW TO READ IT, written before the run (6e's own criteria, restated against the arms that exist):
#   after separably above before, near the naive ~2x  -> the fragmentation bound was the whole cost. Record
#        it, with the all-local caveat: a publisher with any remote subscriber on the topic gets nothing,
#        because the record has to suit every destination.
#   after separably above before, well short of 2x    -> real but partial. 6e names the candidates and the
#        one still open is lost tx_buffer batching; ring depth was measured not-separable on 2026-10-02
#        (slot_depth_cost.sh) though that harness could not see a cost below ~20%.
#   ranges OVERLAP                                    -> 6e(a) did not deliver. The mechanism is in and its
#        predicate is tested, so the question becomes whether it ever GRANTS in this cell - check tx_shm
#        against datagrams per sample rather than assuming the path was taken.
#   after separably BELOW before                      -> report that first. A whole record is one larger ring
#        write where there were two smaller ones, and a slot sized for it halves the ring at a fixed segment
#        size; neither was expected to cost, and both were measured not to, but this is the arm that would
#        say otherwise.
#
# Judged on send_mbps, the cell's own metric, with the per-arm range printed - this harness's spread between
# repetitions of one arm has been 13-32% on other cells, so a median difference inside the ranges is a draw.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BEFORE=${BEFORE:?set BEFORE=<sha without 6e(a)>}
AFTER=${AFTER:?set AFTER=<sha with 6e(a)>}
REPS=${REPS:-3}
DUR=${DUR:-5}
SIZE=${SIZE:-p4}
OUT=${OUT:-$HOME/rig_results_safe/p4_whole_record.txt}
# 6e(a) is INERT at the default slot, measured 2026-10-03: a p4 sample is 2800 bytes and the default slot is
# one datagram, so the predicate refuses - correctly - and both arms sent 2.01 datagrams per sample. A slot
# that can hold the sample is therefore part of the thing being tested, not a tuning knob, and it is applied
# to BOTH arms so the only difference between them stays the commit.
# 4096, not 2816. The first attempt used "p4 rounded up" and still fragmented: the record is the CDR PLUS
# tt_Header, the submessage header and the data header, about 2,840 bytes for a 2,800-byte sample, so a 2,816
# slot is short by a few dozen. Sized generously rather than exactly, because the point of the run is whether
# a whole record helps, not how tightly a slot can be cut.
BUILD_FLAGS=${BUILD_FLAGS:--Dtt_SEGMENT_SLOT_BYTES=4096}
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== p4 whole record $(date -Is) before=$BEFORE after=$AFTER size=$SIZE reps=$REPS dur=$DUR ==="

for arm in before after; do
    sha=$([ "$arm" = before ] && echo "$BEFORE" || echo "$AFTER")
    say "--- arm $arm ($sha) ---"
    # Only the tickle cell: the vendors are unchanged between these two SHAs, so running them would spend rig
    # time to re-measure a constant. COMPARISON's vendor figures stand.
    FRAMEWORKS=tickle REPS="$REPS" DUR="$DUR" SCEN=reliable_throughput SIZE="$SIZE" SHA="$sha" \
        BUILD_FLAGS="$BUILD_FLAGS" \
        OUT="$OUT.$arm" "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" >>"$OUT.driver" 2>&1
    grep -hE 'arm=ON .*framework=tickle' "$OUT.$arm" 2>/dev/null | tee -a "$OUT" >/dev/null
done
say "=== done $(date -Is) ==="

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
rows = {"before": [], "after": []}
cur = None
for line in open(sys.argv[1]):
    m = re.match(r"--- arm (\w+) ", line)
    if m:
        cur = m.group(1); continue
    if cur and "framework=tickle" in line and "arm=ON" in line:
        f = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
        try:
            rows[cur].append((float(f["send_mbps"]), int(f.get("tx_shm", 0)), int(f.get("sent", 0)),
                              f.get("sample_path", "?")))
        except (KeyError, ValueError):
            pass

print()
print("  arm      n   send_mbps median   range                 datagrams/sample  sample_path")
paths = {}
for arm in ("before", "after"):
    rs = rows[arm]
    if not rs:
        print(f"  {arm:<8} 0   (no usable reps)"); continue
    mb = [r[0] for r in rs]
    dps = [r[1] / r[2] for r in rs if r[2]]
    paths[arm] = sorted({r[3] for r in rs})
    print(f"  {arm:<8} {len(rs)}   {st.median(mb):>14.1f}   {min(mb):.1f}..{max(mb):.1f}"
          f"{'':>8} {st.median(dps) if dps else float('nan'):.2f}{'':>12} {','.join(paths[arm])}")
print()
# Did the thing under test engage at all? Asked before any throughput verdict, because "no separable change"
# and "the mechanism never ran" read identically in the numbers and mean completely different things - the
# first run of this harness spent a rig campaign learning that at the default slot.
if paths.get("after") and all(p == "frag" for p in paths["after"]):
    print("INERT: the after arm still took the fragment path on every repetition, so 6e(a) never granted and")
    print("  the throughput figures below compare two identical behaviours. Check the slot against the record")
    print("  size - the record is the CDR plus tt_Header, the submessage header and the data header - before")
    print("  reading anything else here.")
    print()
b, a = [r[0] for r in rows["before"]], [r[0] for r in rows["after"]]
if len(b) < 2 or len(a) < 2:
    print("VOID: fewer than two usable repetitions on an arm.")
elif not (min(a) > max(b) or min(b) > max(a)):
    print(f"NO SEPARABLE CHANGE: before {min(b):.0f}..{max(b):.0f}, after {min(a):.0f}..{max(a):.0f} overlap.")
    print("  6e(a) did not deliver in this cell. Before concluding it cannot, check whether it ever GRANTED:")
    print("  datagrams per sample above should fall toward 1.0 if a whole record was being sent.")
elif st.median(a) > st.median(b):
    gain = (st.median(a) / st.median(b) - 1) * 100
    print(f"RECOVERED {gain:.1f}%: {st.median(b):.0f} -> {st.median(a):.0f} Mbps, ranges separable.")
    print("  Record with the all-local caveat: a publisher with any remote subscriber on the topic gets")
    print("  nothing from this, because one record has to suit every destination.")
else:
    loss = (1 - st.median(a) / st.median(b)) * 100
    print(f"WORSE BY {loss:.1f}%: {st.median(b):.0f} -> {st.median(a):.0f} Mbps, ranges separable. Report this")
    print("  first. A whole record is one larger ring write where there were two smaller ones.")
PYEOF
