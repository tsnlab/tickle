#!/usr/bin/env bash
# Did record_size_limit() change anything at the DEFAULT slot, where 6e(a) cannot grant?
#
# The argument says no: with every peer's slot at one datagram, whole_record_limit_for() returns that, and
# record_size_limit(floor=tt_MAX_BUFFER_LENGTH) and record_size_limit(floor=FRAG_WHOLE_DATA_LIMIT) both
# resolve to exactly the constants they replaced. But "should be inert" is an argument, and two arguments of
# mine were wrong tonight - the hoist comment claimed the bound could not come apart while sitting on the code
# that parted it. This is the measurement.
#
# Against OUR OWN previous value, not against a vendor: the vendors control the rig, not our regressions. The
# figure to beat is the default-slot ON arm, 2745 Mbps, measured before record_size_limit() existed.
#
# HOW TO READ IT, written before the run:
#   ON arm within the run-to-run spread of 2745          -> inert at the default, as argued. Safe to keep
#        quoting 2745, and the 6e(a) hold is the only behavioural change in 73073336.
#   ON arm separably BELOW 2745                          -> record_size_limit() costs something on the path
#        every build takes. That is a regression in the hot path and outranks all of the 6e work.
#   ON arm separably ABOVE                               -> report it, do not celebrate it; the likeliest
#        cause is a difference in the arms rather than a gain, and it needs the same scrutiny as a loss.
#   datagrams per sample must stay ~2.01 on the ON arm. At the default slot the record cannot go whole, so a
#        ratio near 1.0 would mean the ceiling is not holding and the reading above is void.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SHA=${SHA:-73073336}
OUT=${OUT:-$HOME/rig_results_safe/rig_default_regression.txt}
: >"$OUT"
echo "=== default-slot regression $(date -Is) sha=$SHA (no BUILD_FLAGS: the shipped geometry) ===" | tee -a "$OUT"
timeout 1500 env FRAMEWORKS=tickle REPS=3 DUR=3 SCEN=reliable_throughput SIZE=p4 SHA="$SHA" \
    OUT=/tmp/rigdef_cells.txt "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" >>"$OUT.driver" 2>&1
grep -hE 'arm=|built:|FATAL' /tmp/rigdef_cells.txt.tickle 2>/dev/null | tee -a "$OUT" >/dev/null
python3 - "$OUT" <<'PYEOF'
import sys, statistics as st
# Read BOTH arms. The OFF arm is the control and the first version of this script ignored it, sitting in
# the same file: with segments compiled out whole_record_limit_for() returns 0 and record_size_limit()
# returns its floor, so every bound is byte-identical to before the change. Anything that moves OFF is the
# machine, not the code.
arms = {"ON": [], "OFF": []}
for line in open(sys.argv[1]):
    for a in ("ON", "OFF"):
        if f"arm={a} " in line and "framework=tickle" in line:
            f = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
            try:
                arms[a].append((float(f["send_mbps"]), int(f.get("tx_shm", 0)), int(f.get("sent", 0))))
            except (KeyError, ValueError):
                pass
            break
ON_BEFORE = 2745.0    # ON arm, before record_size_limit() existed
OFF_BEFORE = 1950.3   # OFF arm, same harness, 2026-10-03 02:36
print()
if len(arms["ON"]) < 2 or len(arms["OFF"]) < 2:
    print("VOID: fewer than two usable reps on an arm."); raise SystemExit
on = [r[0] for r in arms["ON"]]; off = [r[0] for r in arms["OFF"]]
dps = [r[1] / r[2] for r in arms["ON"] if r[2]]
on_m, off_m = st.median(on), st.median(off)
d_on = 100 * (on_m - ON_BEFORE) / ON_BEFORE
d_off = 100 * (off_m - OFF_BEFORE) / OFF_BEFORE
print(f"  ON  {on_m:8.1f} Mbps  range {min(on):.1f}..{max(on):.1f}   vs own previous {ON_BEFORE}: {d_on:+.2f}%")
print(f"  OFF {off_m:8.1f} Mbps  range {min(off):.1f}..{max(off):.1f}   vs own previous {OFF_BEFORE}: {d_off:+.2f}%   (CONTROL)")
print(f"  datagrams/sample {st.median(dps):.2f}" if dps else "  datagrams/sample: n/a")
print()
if dps and st.median(dps) < 1.5:
    print("  VOID: the record went whole at the default slot, so the ceiling is not holding and the")
    print("    throughput reading says nothing about the default path.")
elif abs(d_on - d_off) < 0.25:
    print(f"  INERT: ON moved {d_on:+.2f}% and the control moved {d_off:+.2f}%. A change that cannot touch")
    print("    the OFF arm moved it by the same amount, so the machine accounts for both. No cost on the")
    print("    default path that this run can see.")
elif d_on < d_off - 0.25:
    print(f"  REGRESSION: ON fell {d_on:+.2f}% while the control moved {d_off:+.2f}%, so the difference is")
    print("    NOT the machine. The hot path got slower; this outranks the 6e work.")
else:
    print(f"  ABOVE THE CONTROL: ON {d_on:+.2f}% against control {d_off:+.2f}%. Treat as a difference in")
    print("    the arms until shown otherwise rather than as a gain.")
print()
print("  Why the control and not a bare comparison to a number: the first version of this script asked")
print("  whether the new RANGE straddled the single previous POINT, with no knowledge of that point's own")
print("  spread, and declared REGRESSION on a 0.54% difference that the control showed was the box.")
PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
on = []
for line in open(sys.argv[1]):
    if "arm=ON" in line and "framework=tickle" in line:
        f = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
        try:
            on.append((float(f["send_mbps"]), int(f.get("tx_shm", 0)), int(f.get("sent", 0))))
        except (KeyError, ValueError):
            pass
print()
if len(on) < 2:
    print("VOID: fewer than two usable ON reps."); raise SystemExit
mb = [r[0] for r in on]
dps = [r[1] / r[2] for r in on if r[2]]
print(f"  ON arm n={len(on)}  median {st.median(mb):.1f} Mbps  range {min(mb):.1f}..{max(mb):.1f}")
print(f"  datagrams/sample median {st.median(dps):.2f}" if dps else "  datagrams/sample: n/a")
print(f"  previous value, before record_size_limit(): 2745 Mbps")
if dps and st.median(dps) < 1.5:
    print("  VOID: the record went whole at the default slot - the ceiling is not holding, so the")
    print("    throughput reading says nothing about the default path.")
elif min(mb) <= 2745 <= max(mb):
    print("  INERT: 2745 falls inside this run's range. No measurable cost on the default path.")
elif max(mb) < 2745:
    print(f"  REGRESSION: whole range below 2745 ({max(mb):.1f} at best). The hot path got slower; this")
    print("    outranks the 6e work and should be looked at before anything else.")
else:
    print(f"  ABOVE: whole range above 2745 ({min(mb):.1f} at worst). Treat as a difference in the arms")
    print("    until shown otherwise rather than as a gain.")
PYEOF
echo "=== done $(date -Is) ===" | tee -a "$OUT"
