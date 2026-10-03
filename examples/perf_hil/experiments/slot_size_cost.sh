#!/usr/bin/env bash
# Does C (the ~244k slot-writes a second) survive a BIGGER slot?
#
# WHY. slot_rate_hypothesis.sh established samples/s = C / slots with C constant to 1.4% - but every one of
# its five points had slot_bytes of 800, 1024 or 1472, because tt_CONTROL_MAX_LENGTH follows
# tt_MAX_BUFFER_LENGTH downward and tt_SEGMENT_SLOT_BYTES follows that. Putting a 2800-byte p4 sample in ONE
# slot needs slot_bytes >= 2824, which is ~2.8x outside everything measured, and the only observation ever
# taken in that region is the 2026-10-03 whole-record run that stalled at 1.4 Mbps. So "5,468 Mbps" rests on
# an untested assumption, and calling it an extrapolation was looking at one axis of two.
#
# THE ISOLATION. p2 (1292 B) fits ONE slot at either setting, so slot COUNT is pinned at 1 and only slot SIZE
# changes. No whole-record path, no seq_no change, no fragmentation - just a bigger hole for the same record.
#
# HOW TO READ IT, written before the run:
#   C within a few percent at 4096 as at 1472  -> slot size is not a cost. The 5,468 figure's assumption
#        holds and the 1.4 Mbps stall was plumbing, as suspected but not until now shown.
#   C separably LOWER at 4096                  -> the one-slot record's premise is broken independently of
#        how seq_no is carried, and the whole 6e direction needs rethinking before anything is built.
#   the 4096 arm stalls or drops               -> report that first. It is the same symptom as the 2026-10-03
#        run and would say the stall is about slot size rather than about the whole-record plumbing.
#   CONTROL: datagrams/sample must read ~1.0 on BOTH arms. If the 4096 arm differs, slot count moved too and
#        the comparison is not isolating anything - VOID.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SHA=${SHA:-$(cd "$REPO" && git rev-parse --short origin/main)}
OUT=${OUT:-$HOME/rig_results_safe/slot_size_cost.txt}
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== slot size cost $(date -Is) sha=$SHA (p2, one slot either way) ==="
for S in 1472 4096; do
    say "--- tt_SEGMENT_SLOT_BYTES=$S ---"
    FRAMEWORKS=tickle REPS=3 DUR=5 SCEN=reliable_throughput SIZE=p2 SHA="$SHA" \
        BUILD_FLAGS="-Dtt_SEGMENT_SLOT_BYTES=$S" OUT=/tmp/slotsize_$S.txt timeout 900 \
        "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" >>"$OUT.driver" 2>&1
    grep -hE 'arm=ON .*framework=tickle' "/tmp/slotsize_$S.txt.tickle" 2>/dev/null | sed "s/^/S=$S /" >>"$OUT"
done
say "=== done $(date -Is) ==="
python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
rows = {}
for line in open(sys.argv[1]):
    m = re.match(r'S=(\d+) ', line)
    if not m or 'framework=tickle' not in line:
        continue
    f = dict(kv.split('=', 1) for kv in line.split() if '=' in kv)
    try:
        sent = int(f['sent'])
        if sent < 1000:
            continue
        rows.setdefault(int(m.group(1)), []).append(
            (float(f['send_mbps']), int(f.get('tx_shm', 0)), sent, int(f.get('shm_full_dropped', 0))))
    except (KeyError, ValueError):
        pass
print()
print("  slot_B    samples/s   datagrams/sample   ring drops")
res = {}
for s in sorted(rows):
    rs = rows[s]
    if len(rs) < 2:
        print(f"  {s:<9} (fewer than two usable reps)"); continue
    rate = st.median(r[0] for r in rs) * 1e6 / (1292 * 8)
    dps = st.median(r[1] / r[2] for r in rs if r[2])
    drops = st.median(r[3] for r in rs)
    res[s] = (rate, dps, drops)
    print(f"  {s:<9} {rate:>10,.0f} {dps:>15.2f} {drops:>12,.0f}")
print()
# The control, in the code. Both arms must still be ONE slot per sample or nothing is isolated.
if len(res) < 2:
    print("  VOID: fewer than two usable arms.")
elif any(abs(v[1] - 1.0) > 0.15 for v in res.values()):
    print("  VOID: an arm is not at one datagram per sample, so slot COUNT moved and this comparison")
    print("    isolates nothing.", {k: round(v[1], 2) for k, v in res.items()})
else:
    a, b = res[1472][0], res[4096][0]
    delta = (b - a) / a * 100
    print(f"  C at 1472 = {a:,.0f}   C at 4096 = {b:,.0f}   change {delta:+.1f}%")
    if res[4096][2] > 0:
        print(f"  WARNING: the 4096 arm dropped {res[4096][2]:,.0f} ring writes - report that before the rate.")
    if abs(delta) < 5:
        print("  SLOT SIZE IS NOT A COST: C survives a 2.8x larger slot. The 5,468 Mbps assumption holds and")
        print("    the 2026-10-03 stall was plumbing rather than slot size.")
    elif delta < 0:
        print("  SLOT SIZE COSTS: the one-slot record's premise is broken independently of seq_no design.")
        print("    Nothing should be built on 5,468 until this is understood.")
    else:
        print("  HIGHER AT 4096: report it and suspect the arms before believing it.")
PYEOF
