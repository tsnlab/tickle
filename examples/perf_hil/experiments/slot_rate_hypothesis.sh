#!/usr/bin/env bash
# Is the segment path's bottleneck the number of SLOT WRITES per sample, and is it a constant?
#
# THE HYPOTHESIS, derived from COMPARISON 2.2c's own published figures rather than from a new idea:
# converting that table from Mbps to samples/s gives TickLE 243,905 (p2, 1 slot), 244,558 (p3, 1 slot) and
# 122,545 (p4, 2 slots). p4/p2 = 0.502 - exactly half, for exactly twice the slots. CycloneDDS over iceoryx
# is flat at 0.922 because it carries one chunk per sample whatever the size. So the segment path looks like
# a constant ~244k slot-writes a second, and p4 pays two of them.
#
# If that is right, the fix for p4 is one slot write per sample and the ceiling is 243,905 x 2800 x 8 =
# 5,463 Mbps, against CycloneDDS's measured 5,287. If it is wrong, nobody should build the multi-slot record.
#
# THE ARMS. TICKLE_DATAGRAM_BYTES changes how many datagrams - and so how many slots - a 2800-byte sample
# takes, without changing the sample:
#     1472 (default) -> 2 slots      1024 -> 3 slots      800 -> 4 slots
#
# HOW TO READ IT, written before the run:
#   samples/s falls as C/slots with C within a few percent across all three arms
#        -> the bottleneck is per-slot and constant. 5,463 Mbps stops being a prediction and becomes an
#           extrapolation from four points. Build the one-slot record.
#   samples/s falls FASTER than C/slots
#        -> something else scales with fragment count too (reassembly, cache footprint, ack volume). The
#           one-slot record still helps but 5,463 is an overestimate; re-derive before promising it.
#   samples/s roughly FLAT across arms
#        -> the bottleneck is per-BYTE, not per-slot. The whole account above is wrong, p4's gap is not
#           about slot count, and the multi-slot record should NOT be built. This is the arm that refutes.
#   CONTROL: datagrams/sample (tx_shm / sent) must read 2, 3, 4 on the three arms. If it does not, the
#        build did not take the flag and the throughput numbers say nothing - VOID, not a weak result.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SHA=${SHA:-$(cd "$REPO" && git rev-parse --short origin/main)}
OUT=${OUT:-$HOME/rig_results_safe/slot_rate_hypothesis.txt}
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== slot-rate hypothesis $(date -Is) sha=$SHA ==="
# BUILD_FLAGS, not TICKLE_DATAGRAM_BYTES. The first run of this used the latter and every arm came back
# identical: s6_witness_check.sh forwards ONLY TICKLE_EXTRA_CFLAGS across the ssh to the rig's build.sh, so
# the variable was set in a local shell and never reached the compiler. BUILD_FLAGS maps to
# TICKLE_EXTRA_CFLAGS and does cross, and build.sh gives each flag set its own install prefix.
for D in 1472 1024 800; do
    say "--- TICKLE_DATAGRAM_BYTES=$D ---"
    TICKLE_DATAGRAM_BYTES=$D FRAMEWORKS=tickle REPS=3 DUR=5 SCEN=reliable_throughput SIZE=p4 SHA="$SHA" \
        OUT=/tmp/slotrate_$D.txt timeout 900 \
        "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" >>"$OUT.driver" 2>&1
    grep -hE 'arm=ON .*framework=tickle' "/tmp/slotrate_$D.txt.tickle" 2>/dev/null | sed "s/^/D=$D /" >>"$OUT"
done
say "=== done $(date -Is) ==="
python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
rows = {}
for line in open(sys.argv[1]):
    m = re.match(r'D=(\d+) ', line)
    if not m or 'framework=tickle' not in line:
        continue
    f = dict(kv.split('=', 1) for kv in line.split() if '=' in kv)
    try:
        sent = int(f['sent'])
        if sent < 1000:
            continue
        rows.setdefault(int(m.group(1)), []).append(
            (float(f['send_mbps']), int(f.get('tx_shm', 0)), sent))
    except (KeyError, ValueError):
        pass
print()
print("  datagram_B   samples/s    datagrams/sample   C = samples/s x slots")
consts = []
for d in sorted(rows, reverse=True):
    rs = rows[d]
    if len(rs) < 2:
        print(f"  {d:<10}   (fewer than two usable reps)"); continue
    rate = st.median(r[0] for r in rs) * 1e6 / (2800 * 8)
    dps = st.median(r[1] / r[2] for r in rs if r[2])
    consts.append(rate * dps)
    print(f"  {d:<10} {rate:>10,.0f}   {dps:>14.2f}   {rate*dps:>14,.0f}")
print()
# The control, IN THE CODE this time. The first run put it in the header comment only - "datagrams/sample
# must read 2, 3, 4" - and the verdict logic never looked at it. All three arms came back at 2.01 because
# the flag never reached the compiler, C was trivially constant across three repetitions of ONE condition,
# and the script printed "PER-SLOT AND CONSTANT" with full confidence. A reading rule no code enforces is
# a comment.
dps_seen = {d: st.median(r[1] / r[2] for r in rows[d] if r[2]) for d in rows if rows[d]}
if len(set(round(v) for v in dps_seen.values())) < len(dps_seen):
    print("  VOID: the arms did not differ in datagrams per sample -", dps_seen)
    print("    The build did not take the flag, so every arm is the same condition and the constancy below")
    print("    is three repetitions of one point. Nothing here says anything about slot count.")
    raise SystemExit
if len(consts) < 3:
    print("  VOID: fewer than three usable arms.")
else:
    spread = (max(consts) - min(consts)) / st.median(consts) * 100
    print(f"  C spread across arms: {spread:.1f}%")
    if spread < 10:
        print("  PER-SLOT AND CONSTANT: samples/s = C / slots holds. 5,463 Mbps becomes an extrapolation.")
    elif min(consts) == consts[0]:
        print("  FALLS FASTER THAN C/slots: something else scales with fragment count. The one-slot record")
        print("    still helps; 5,463 is an overestimate and must be re-derived before it is promised.")
    else:
        print("  NOT CONSTANT: re-read the arms before concluding anything; C should not rise with slots.")
PYEOF
