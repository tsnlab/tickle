import sys, statistics as st
# Read BOTH arms. The OFF arm is the control and the first version of this script ignored it, sitting in
# the same file: with segments compiled out whole_record_limit_for() returns 0 and record_size_limit()
# returns its floor, so every bound is byte-identical to before the change. Anything that moves OFF is
# the machine, not the code.
arms = {"ON": [], "OFF": []}
for line in open(sys.argv[1]):
    for a in ("ON", "OFF"):
        if "arm=%s " % a in line and "framework=tickle" in line:
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
    print("  VOID: fewer than two usable reps on an arm.")
    raise SystemExit
on = [r[0] for r in arms["ON"]]
off = [r[0] for r in arms["OFF"]]
dps = [r[1] / r[2] for r in arms["ON"] if r[2]]
on_m, off_m = st.median(on), st.median(off)
d_on = 100 * (on_m - ON_BEFORE) / ON_BEFORE
d_off = 100 * (off_m - OFF_BEFORE) / OFF_BEFORE
print("  ON  %8.1f Mbps  range %.1f..%.1f   vs own previous %.1f: %+.2f%%" % (on_m, min(on), max(on), ON_BEFORE, d_on))
print("  OFF %8.1f Mbps  range %.1f..%.1f   vs own previous %.1f: %+.2f%%   (CONTROL)" % (off_m, min(off), max(off), OFF_BEFORE, d_off))
print("  datagrams/sample %.2f" % st.median(dps) if dps else "  datagrams/sample: n/a")
print()
if dps and st.median(dps) < 1.5:
    print("  VOID: the record went whole at the default slot, so the ceiling is not holding and the")
    print("    throughput reading says nothing about the default path.")
elif abs(d_on - d_off) < 0.25:
    print("  INERT: ON moved %+.2f%% and the control moved %+.2f%%, a difference of %.2f pp." % (d_on, d_off, abs(d_on - d_off)))
    print("    A change that cannot touch the OFF arm moved it by the same amount, so the machine")
    print("    accounts for both. No cost on the default path that this run can see.")
elif d_on < d_off - 0.25:
    print("  REGRESSION: ON fell %+.2f%% while the control moved %+.2f%%, so the difference is NOT the" % (d_on, d_off))
    print("    machine. The hot path got slower; this outranks the 6e work.")
else:
    print("  ABOVE THE CONTROL: ON %+.2f%% against control %+.2f%%. Treat as a difference in the arms" % (d_on, d_off))
    print("    until shown otherwise rather than as a gain.")
print()
print("  Why a control and not a bare comparison: the first version asked whether the new RANGE straddled")
print("  the single previous POINT, with no knowledge of that point's spread, and called REGRESSION on a")
print("  0.54% difference the control showed was the box.")
