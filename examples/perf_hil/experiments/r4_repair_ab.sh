#!/usr/bin/env bash
# r4_repair_ab.sh - the Wi-Fi profile's R4 (1 MiB p50 at ge10b4) against a non-wire change of the reader's retry
# timing, PC netns only (wifi_repair_ab.sh / wifi_burst_loss_netns.sh), 2026-10-25. No rig, no default namespace.
#
# WHY. R4 fails at ge10b4 on both profiles (RESULTS `WR`: p50 102.9-110.9 ms against 74.0-74.4; reverse bytes 0.25%,
# "a timer or pacing value"). Reproduced on main b434f649 (3 reps, 450 samples): p50 138.4 ms against 74.2. Traced
# (r4_trace_read.py on a LARGE_TRACE run, 300 samples): of a ge10b4 sample near the p50 (108 ms), 42 ms is the drain
# the control has too, 10 ms to the critical unit's first resend, 49 ms the reader's wait before it asks again for a
# repair that was lost, 4 ms the writer's resend order holding a request, 3 ms the last repair's transit. The re-asks
# came from the reader's retry timer (187 of 326) and the answers to the writer's end-of-sample HEARTBEAT chase (134),
# and the timer's interval was 37 ms at the p50 (p90 78 ms) on a path whose idle repair takes 3.2 ms one way (6.9 ms
# round trip). Repairs queued behind fresh units in netem took 17.8 ms (p50) against 3.2 ms with nothing ahead, but on
# the critical path that queue is only the 10 ms term: the dominant term is the timer.
#
# THE CHANGE (K): on a repair_timing link (WIFI), a writer of large samples gets Karn's rule past the seed, as every
# other writer already does: only a recovery its first request answered is timed. Today such a proxy keeps the default
# rule (a recovery up to tt_RELIABLE_RETRY retries timed), so a recovery that spans a lost repair's retry wait goes into
# the estimate and lengthens the interval it was waiting on - the loop the WIFI repair timing (DESIGN "Wi-Fi repair
# timing") removed for small samples. It was kept for large writers on 2026-10-23 because under Karn's rule the
# estimate stayed at the seed and the reader's frequent requests made the writer resend (one 1 MiB rep 208,479 units).
# That was before the writer's resend order (a664dae9, 2026-10-23 23:09; RACK b1088e82), which answers a request for a
# unit whose resend is not yet shown lost by holding it: a frequent request now costs an ACKNACK, not a resend. No wire
# change; the writer's code is untouched; ETHERNET's recoveries are timed as before (repair_timing off).
#
# PRE-REGISTERED (written 2026-10-25 before the prototype; the python at the end applies P1-P4, P5 is read from the
# traced runs by r4_trace_read.py and stated in RESULTS):
#   Trees, at once (wifi_repair_ab.sh: they share the PC's load rep by rep): M = main with this harness, A = the same
#   commit in a second checkout (the A/A control), K = M + the change. PROFILE=wifi, TT_UDP_OFFLOAD=0, LARGE_RSTATS=1,
#   ARMS "ctl ge5b2 ge5b4 ge10b4 bc tv", all three cells, REPS=3.
#   P1 (the target) K's own R4 verdict at ge10b4 (wifi_burst_read.py: median of per-rep p50 <= its ctl rel_large p50 +
#      4 x base_rtt) is PASS.
#   P2 (no WORSE) K vs M on every arm and cell by wifi_repair_ab.sh's rule (small cells' median per-rep p99 within 10%,
#      delivered within 0.1 point, no more STORM reps), and in rel_large at every arm K's median per-rep p50 at most
#      1.10 x M's; and no R1-R6 rule FAIL for K that is PASS for M. A WORSE that A vs M shows too on the same cell is
#      UNDECIDED (noise), listed, never a pass.
#   P3 (the control) A vs M: the same reading, expected NOT WORSE everywhere; |A - M| / M of the ge10b4 R4 p50 is the
#      A/A spread, and K's fall of that p50 counts only when it is above twice that spread. The small cells (be_small,
#      rel_small) are a second control: wifi_link_node is built without tt_LARGE_SAMPLES, so the change is compiled out
#      there; their K - M differences are the floor for P2.
#   P4 (sample count) rel_large 150 samples a rep, 450 a cell a tree; a rep scoring under 100 is VOID (harness); a cell
#      with fewer than 2 unloaded reps is VOID for latency. Printed per tree and cell.
#   P5 (treatment and falsifier; traced runs, LARGE_TRACE=1, ge10b4 rel_large, 2 reps, K and M): the treatment applied
#      when K's reader retry-timer interval p50 is at most half of M's. FALSIFIED - the timer is not why R4 fails - if it
#      applied and K's R4 p50 (P1's figure) falls by less than half of M's re-ask term (49 ms: under 25 ms). VOID if it
#      did not apply.
#   R7 (copies after delivery under tv) is printed, not judged: it is FEC's (RESULTS `WR`).
#
# Usage: M=/tree A=/tree K=/tree [REPS=3] [TAG=r4_ab] [BASE_OUT=/home/semih/r4_results] r4_repair_ab.sh
#   Ends with "=== r4_repair_ab ended" in $BASE_OUT/$TAG.r4.txt, whatever happened.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
M=${M:?M=main tree} A=${A:?A=A/A tree} K=${K:?K=change tree}
TAG=${TAG:-r4_ab}
BASE_OUT=${BASE_OUT:-$HOME/r4_results}
OUTF="$BASE_OUT/$TAG.r4.txt"
mkdir -p "$BASE_OUT" || exit 1
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== r4_repair_ab ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ===" >>"$OUTF"
}
trap on_exit EXIT
echo "=== r4_repair_ab $(date -Is) M=$M A=$A K=$K" >"$OUTF"
TREES="M=$M A=$A K=$K" TAG=$TAG BASE_OUT=$BASE_OUT PROFILE=wifi REPS=${REPS:-3} \
    ARMS=${ARMS:-"ctl ge5b2 ge5b4 ge10b4 bc tv"} CELLS="be_small rel_small rel_large" \
    NODE_ENV=TT_UDP_OFFLOAD=0 LARGE_RSTATS=1 "$HERE/wifi_repair_ab.sh" >>"$OUTF" 2>&1
python3 - "$BASE_OUT" "$TAG" <<'EOF' | tee -a "$OUTF"
import os, re, statistics, sys
base, tag = sys.argv[1], sys.argv[2]
KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")
nproc = os.cpu_count() or 1
def read(p):
    try:
        return open(p, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return []
def rules(lab):
    v = {}
    for line in read(os.path.join(base, f"{tag}_{lab}", "summary.txt")):
        m = re.match(r"^(PASS|FAIL|VOID)\s+(R\d[^:]*):\s+(\S+)?", line)
        if m:
            v.setdefault(m.group(2), []).append((m.group(1), line.strip()))
    return v
def large(lab):
    out = {}
    runs = os.path.join(base, f"{tag}_{lab}", "runs")
    for name in sorted(os.listdir(runs)) if os.path.isdir(runs) else []:
        arm, cell, rep = name.rsplit("-", 2)
        if cell != "rel_large":
            continue
        d = os.path.join(runs, name)
        load = float(dict(KV.findall(" ".join(read(os.path.join(d, "meta.txt"))))).get("load", 0))
        sub = [dict(KV.findall(x)) for x in read(os.path.join(d, "echo.log")) if x.startswith("RESULT: role=sub")]
        r = out.setdefault(arm, {"p50": [], "n": 0, "void": []})
        if not sub:
            r["void"].append(f"rep {rep}: no RESULT")
            continue
        n = int(sub[0].get("delivered", 0))
        r["n"] += n
        if n < 100 or load > nproc / 2:
            r["void"].append(f"rep {rep}: delivered {n} load {load}")
            continue
        r["p50"].append(float(sub[0]["latency_median_us"]) / 1000)
    return out
ab = "\n".join(read(os.path.join(base, f"{tag}.ab.txt")))
verdicts = {}
for m in re.finditer(r"^VERDICT (\w+) vs M (\w+)/(\w+): (.*)$", ab, re.M):
    verdicts[(m.group(1), m.group(2), m.group(3))] = m.group(4)
R = {lab: rules(lab) for lab in "MAK"}
L = {lab: large(lab) for lab in "MAK"}
print("--- r4_repair_ab reading (pre-registered in r4_repair_ab.sh)")
for lab in "MAK":
    for arm, r in sorted(L[lab].items()):
        med = statistics.median(r["p50"]) if r["p50"] else float("nan")
        r["med"] = med
        print(f"P4 {lab} rel_large {arm:7} samples {r['n']} unloaded reps {len(r['p50'])} per-rep p50 ms "
              f"{' '.join(f'{x:.1f}' for x in r['p50'])} median {med:.1f}" + (f" VOID {r['void']}" if r["void"] else ""))
def verdict_of(lab, rule, arm=None):
    for v, line in R[lab].get(rule, []):
        if arm is None or f" {arm} " in line:
            return v, line
    return None, ""
v, line = verdict_of("K", "R4", "ge10b4")
print(f"P1 K R4 ge10b4: {v or 'missing'}  ({line})")
p1 = v == "PASS"
worse, undecided = [], []
for (lab, cell, arm), text in sorted(verdicts.items()):
    if lab != "K" or text.startswith("NOT WORSE"):
        continue
    (undecided if not verdicts.get(("A", cell, arm), "NOT WORSE").startswith("NOT WORSE") else worse).append(
        f"{cell}/{arm}: {text}")
for arm, k in sorted(L["K"].items()):
    m, a = L["M"].get(arm), L["A"].get(arm)
    if m and m["med"] == m["med"] and k["med"] == k["med"] and k["med"] > 1.10 * m["med"]:
        text = f"rel_large/{arm}: median per-rep p50 {k['med']:.1f} > 1.10 x M's {m['med']:.1f}"
        noisy = a and a["med"] == a["med"] and a["med"] > 1.10 * m["med"]
        (undecided if noisy else worse).append(text)
for rule in sorted(set(R["K"]) | set(R["M"])):
    if rule.startswith("R7") or "as-registered" in rule or "falsifier" in rule or rule.endswith("-all"):
        continue
    for v, line in R["K"].get(rule, []):
        if v != "FAIL":
            continue
        mv = [x for x, _ in R["M"].get(rule, [])]
        if "PASS" in mv and "FAIL" not in mv:
            worse.append(f"{rule}: K FAIL where M PASS ({line})")
print("P2 K vs M: " + ("NOT WORSE" if not worse else "WORSE: " + "; ".join(worse)))
for u in undecided:
    print(f"P2 UNDECIDED (A vs M shows it too): {u}")
aa = [f"{c}/{a}: {t}" for (lab, c, a), t in sorted(verdicts.items()) if lab == "A" and not t.startswith("NOT WORSE")]
print("P3 A vs M: " + ("NOT WORSE everywhere" if not aa else "WORSE: " + "; ".join(aa)))
mm, am, km = (L[x].get("ge10b4", {}).get("med", float("nan")) for x in "MAK")
spread = abs(am - mm) / mm if mm == mm and am == am else float("nan")
fall = (mm - km) / mm if mm == mm and km == km else float("nan")
print(f"P3 ge10b4 R4 p50: M {mm:.1f} A {am:.1f} K {km:.1f} ms; A/A spread {100 * spread:.1f}%, K's fall "
      f"{100 * fall:.1f}% ({'counts' if fall > 2 * spread else 'within twice the A/A spread: does not count'})")
for lab in "MAK":
    for v, line in R[lab].get("R7", []):
        print(f"R7 {lab} (reported, not judged): {line}")
ok = p1 and not worse and fall > 2 * spread
print(f"VERDICT r4_repair_ab: {'K PASSES P1-P3' if ok else 'K does NOT pass P1-P3'} (P5 from the traced runs)")
EOF
