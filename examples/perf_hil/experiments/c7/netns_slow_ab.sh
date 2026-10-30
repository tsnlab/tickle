#!/usr/bin/env bash
# C7: the slow reader's repair window (DESIGN.md section 4) against its parent on the PC, in private namespaces.
# large_repair_followups_netns.sh PHASE=slow does the running: the amp phase's ETHERNET path (one band, netem
# "limit 100000 delay 100us rate 1gbit", nothing lost on the path), 1 MiB RELIABLE KEEP_LAST at 30 Hz, the reader
# spinning W us in each sample's callback: W = 0 (the control: nothing dropped, nothing resent) and W = 25000 (75% of
# the period, so the next sample's burst arrives while it spins and its socket drops the end). P = BASE (default
# 73e7a38c), F = HEAD_REF (default HEAD), rep by rep in alternation, each from a git archive of its commit.
#
# PRE-REGISTERED (written before the first run, 2026-10-29):
#   (Round 3, 2026-10-30: BASE defaults to main 75495519; ROUND=3 reads with netns_read_r3.py.)
#   VOID per rep as the harness says (no RESULT line; W = 0 with a socket drop or a resend; W = 25000 with no socket
#     drop; tc drops; load above nproc/2 at the rep's start).
#   CONTROL the change cannot touch: W = 0. Both trees resend 0 there (else the reading is VOID: the window must not
#     act where nothing is lost).
#   SHOWN when, pooled over usable W = 25000 reps, F's resends per socket drop are below P's by at least a quarter AND
#     F delivers no fewer samples per published than P less 1 point. NOT SHOWN otherwise.
#   WORSE when F delivers fewer than P less 1 point, or F's resends per socket drop exceed P's.
#   AMENDED 2026-10-29 22:15, before the run it applies to: two runs on an idle PC dropped nothing at W = 25000 (the
#     spin ends ~1 ms into the next sample's 8.7 ms burst; the receive drain absorbs the rest). The treatment arm is
#     SLOW_W (31000: the spin ends ~6 ms into the next burst) with SLOW_SUB_EXTRA="-S 262144" (512 KB granted, the
#     drain capped as much); the reading is unchanged, read on that arm.
#   With 4 reps a tree the reading can show a large effect only (LRF's slow reps ranged 8,179-532,481 resent): it is
#   a check that the sim's mechanism exists on a real socket, not a rate estimate.
#
# Usage: [BASE=ref] [HEAD_REF=ref] [REPS=4] [DUR=30] [SLOW_W=us] [SLOW_SUB_EXTRA="-S bytes"] netns_slow_ab.sh. Prints "C7_NETNS_DONE rc=N OUT=dir" last,
# whatever happened; the harness's own summary is OUT/summary.txt.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
stamp=$(date +%Y%m%d-%H%M%S)
base=${BASE:-75495519}
head=${HEAD_REF:-HEAD}
trees=${TREES_DIR:-/home/semih/tmp_build/c7/trees_$stamp}
out=${OUT:-$HOME/rig_results_safe/large_repair/c7_slow_$stamp}
rc=1
finish() {
    echo "C7_NETNS_DONE rc=$rc OUT=$out"
}
trap finish EXIT
mkdir -p "$trees/P" "$trees/F" "$out" || exit 1
git -C "$root" archive "$base" | tar -x -C "$trees/P" || exit 1
git -C "$root" archive "$head" | tar -x -C "$trees/F" || exit 1
# The bench is the harness, not the change: P runs F's large_sample_bench.c, which has -S (SLOW_SUB_EXTRA="-S BYTES":
# a socket that holds less than a sample, without which an idle PC's reader dropped nothing on 2026-10-29).
cp "$trees/F/examples/perf_hil/experiments/large_sample_bench.c" "$trees/P/examples/perf_hil/experiments/" || exit 1
echo "P=$base ($(git -C "$root" rev-parse --short "$base")) F=$head ($(git -C "$root" rev-parse --short "$head"))" \
    "SLOW_SUB_EXTRA='${SLOW_SUB_EXTRA:-}' (P with F's large_sample_bench.c)" >"$out/trees.txt"
TMPDIR=${TMPDIR:-/home/semih/tmp_build} TREES="P=$trees/P F=$trees/F" PHASE="${PHASE:-slow}" REPS="${REPS:-4}" DUR="${DUR:-30}" \
    OUT="$out" "$trees/F/examples/perf_hil/experiments/large_repair_followups_netns.sh"
rc=$?
# The reading above, from the harness's pooled lines ("P    W=25000 reps N resent R missing M socket_drops S ...
# delivered D/U").
python3 - "$out/summary.txt" <<'EOF' | tee -a "$out/summary.txt"
import os, re, sys
text = open(sys.argv[1]).read()
pat = re.compile(r"^(\S+)\s+W=\s*(\d+)\s+reps (\d+) resent (\d+) missing \d+ socket_drops (\d+) .* delivered (\d+)/(\d+)", re.M)
rows = {(m.group(1), m.group(2)): [int(x) for x in m.groups()[2:]] for m in pat.finditer(text)}
print("=== C7 reading")
ctl = [rows.get((t, "0")) for t in ("P", "F")]
if any(r is None for r in ctl) or any(r[1] != 0 for r in ctl):
    print("VOID: the W = 0 control is missing or resent:", ctl)
    sys.exit(0)
w = os.environ.get("SLOW_W", "25000")
p, f = rows.get(("P", w)), rows.get(("F", w))
if p is None or f is None or not p[2] or not f[2]:
    print("VOID: no usable W = 25000 reps with socket drops", p, f)
    sys.exit(0)
per = {k: r[1] / r[2] for k, r in (("P", p), ("F", f))}
dlv = {k: r[3] / r[4] if r[4] else float("nan") for k, r in (("P", p), ("F", f))}
print(f"resends per socket drop P {per['P']:.3f} F {per['F']:.3f}; delivered P {100 * dlv['P']:.1f}% "
      f"F {100 * dlv['F']:.1f}%; reps P {p[0]} F {f[0]}")
if dlv["F"] < dlv["P"] - 0.01 or per["F"] > per["P"]:
    print("WORSE")
elif per["F"] <= 0.75 * per["P"]:
    print("SHOWN")
else:
    print("NOT SHOWN")
EOF
# Round 2's reading (C7 design section 7; PHASE=order runs the slow cell and the amp path-loss cells), in code; round 3
# (ROUND=3, the wire experiment) reads with netns_read_r3.py instead.
if [ "${PHASE:-slow}" = order ]; then
    if [ "${ROUND:-2}" = 3 ]; then
        SLOW_W="${SLOW_W:-25000}" python3 "$here/netns_read_r3.py" "$out" | tee -a "$out/summary.txt"
    else
        SLOW_W="${SLOW_W:-25000}" python3 "$here/netns_read.py" "$out" | tee -a "$out/summary.txt"
    fi
fi
