#!/usr/bin/env python3
"""qos_be_ab.py - the reading of qos_be_ab.sh: did the QoS work lower TickLE's same-host BEST_EFFORT goodput?
Written 2026-10-19, before any run of it. Every rule below is implemented in judge() / answer(), not only stated here.

QUESTION. docs/RESULTS.md S1-S3 (BEST_EFFORT KEEP_LAST 1, max rate, P2/P3/P4, one Pi, shared memory) re-measured at
60f49f6a (SB) read 20,303 / 21,284 / 24,773 Mbps against TickLE's 20,867 / 21,982 / 25,066 at e521bbba (SF): -2.7% /
-3.2% / -1.2%, while CycloneDDS moved -2.0% / -0.4% / +1.2% in the same sessions. Hundreds of commits lie between the
two, and SF ran the frameworks one after another, so that is not an A/B. This one is.

DESIGN (qos_be_ab.sh runs it through ab_samehost.sh, gate v2, unchanged reading machinery):
  arms   A = main just before the QoS work (93ec2b3b, the parent of e4fc0683, the first QoS commit), layout-stable
         build (gate_v2_lib.sh TICKLE_LAYOUT_CFLAGS, as SB and SF were built);
         P = A's commit rebuilt with TICKLE_PLACEMENT_CFLAGS (every function moved by a multiple of 64 bytes: the same
         code, another placement);  B = origin/main at launch, A's flags.
  blocks A P B B P A, REPS per block and cell (2 x REPS repetitions per arm), DUR 5 s with the 2 s warm-up and cool-down
         windows - the SB cells' own s6_transport_cells.sh arguments: FRAMEWORKS=tickle, free (unpinned), BE_HISTORY=1
         (-K 1 on client and server), one Pi, its own shared memory.
  cells  best_effort_throughput p2, p3, p4 (the question), reliable_throughput p3 (a second view: RELIABLE moved +1.3%
         to +2.2% in SB, so a code effect that lowers BEST_EFFORT and not RELIABLE says where to look).
  plus   ab_samehost's sentinel (A's build of best_effort_throughput p3 in every block): the Pi's drift, printed here,
         and ab_samehost's own gate v2 verdict (instruction counts) in its summary. Neither decides THIS reading.

METRICS (the ON arm - shared memory - of each repetition with window=ok and insn=ok on both lines, as ab_samehost.load()
reads them; a file counts only if its header names the arm's commit and its builds show the arm's flags):
  goodput_mbps          server.win_recv_mbps (fair_samehost_summary.py's headline for S1-S6); higher is better
  cpu_us_per_delivered  1e6 x (client.sched_cpu_s + server.sched_cpu_s) / server.recv (S17's client + server per
                        delivered sample, whole process lives over every delivered sample); lower is better

THE RULE, per cell and metric (judge()):
  NO VERDICT  fewer than MIN_N (3) counted repetitions in A, P or B.
  VOID        the placement floor |mean(P) - mean(A)| / mean(A) exceeds the effect being asked about in that cell
              (ASKED: the SB-vs-SF TickLE move, 2.7 / 3.2 / 1.2%; reliable p3 is held to the smallest, 1.2%; the CPU
              metric to the same fraction as the cell's goodput): placement alone moves it as much as the question.
  WORSE       EVERY B repetition is on the bad side of EVERY A and EVERY P repetition (no overlap at all), AND
              |mean(B) - mean(A)| exceeds the placement floor, in the bad direction.
  BETTER      the same in the good direction.
  NOT WORSE   otherwise; qualified by the interval of (mean(B) - mean(A)) / mean(A) - Welch, at ab_compare's 2 x SE
              level (two-sided 0.0455, Student t at Welch's df): "asked effect excluded" when its bad-side end is less
              bad than -ASKED (goodput) / +ASKED (CPU), else "asked effect NOT excluded" (the run cannot say).
  ANSWER (goodput of the three BEST_EFFORT cells; CPU and the reliable view are printed the same way, never decide it):
    YES          some BEST_EFFORT cell's goodput is WORSE;
    UNDECIDED    else, some is VOID or NO VERDICT;
    NO           else, all three NOT WORSE (or BETTER) with the asked effect excluded;
    NOT SHOWN    else: not worse by the rule, but the run cannot exclude the asked effect in some cell.

Before any value it prints the sample counts (per arm and cell, and every repetition dropped), then each file's
realized commit (its S6 header) and build flags (the ON and OFF arm's verified line), then the verdicts.

Usage:
  qos_be_ab.py --prereg AB_PREREG.json OUT.json   check ab_samehost's pre-registration is this design, write this
                                                  reading's (its constants and the sha256 of AB_PREREG.json) and print it
  qos_be_ab.py --samplesize SB_PREFIX [REPS,...]  the rep-to-rep spread of TickLE in the SB files -> each REPS's power
                                                  under this rule (simulated), and the REPS it recommends
  qos_be_ab.py --selftest                         the rule on fixture files, through ab_samehost.load()
  qos_be_ab.py summary PREREG.json PREFIX         read PREFIX.b<n>_<arm>_<sha8>_<slug>.txt and print the verdicts
Exit status of summary: 0 NO, 1 YES, 3 UNDECIDED or NOT SHOWN, 2 refused.
"""
import hashlib
import json
import math
import os
import random
import re
import statistics
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ab_samehost  # noqa: E402
from ab_compare import bonferroni_t  # noqa: E402
from gate_v2 import welch_df  # noqa: E402

BE_CELLS = ("best_effort_throughput:p2", "best_effort_throughput:p3", "best_effort_throughput:p4")
SECOND_VIEW = ("reliable_throughput:p3",)
# The effect being asked about, as a fraction of A: TickLE's SB-vs-SF move per cell (RESULTS.md same-host notes).
ASKED = {"best_effort_throughput:p2": 0.027, "best_effort_throughput:p3": 0.032, "best_effort_throughput:p4": 0.012,
         "reliable_throughput:p3": 0.012}
MIN_N = 3
BE_HISTORY = 1
METRICS = (("goodput_mbps", True), ("cpu_us_per_delivered", False))
QOS_FIRST = "e4fc0683"   # tickle.h: struct tt_Topic becomes struct tt_TypeSupport - the first commit of the QoS work


class Refused(Exception):
    pass


def metric(rep, name):
    """One repetition's metric, or None when a field it needs is absent or zero."""
    try:
        if name == "goodput_mbps":
            return float(rep["server"]["win_recv_mbps"])
        recv = float(rep["server"]["recv"])
        cpu = float(rep["client"]["sched_cpu_s"]) + float(rep["server"]["sched_cpu_s"])
        return 1e6 * cpu / recv if recv > 0 else None
    except (KeyError, ValueError):
        return None


def mean_se(v):
    m = statistics.fmean(v)
    return m, (statistics.stdev(v) / math.sqrt(len(v)) if len(v) > 1 else float("nan")), len(v)


_T = {}


def t_2se(df):
    """ab_compare's 2 x SE level (two-sided 0.0455) as a Student t at df (cached; df rounded to 0.1)."""
    key = math.inf if math.isinf(df) else round(df, 1)
    if key not in _T:
        _T[key] = bonferroni_t(1, key)
    return _T[key]


def judge(a, p, b, hi, asked):
    """-> (verdict, detail dict). a, p, b: the arms' values; hi: higher is better; asked: the effect, a fraction."""
    d = {"n": (len(a), len(p), len(b))}
    if min(d["n"]) < MIN_N:
        return "NO VERDICT", d
    sa, sp, sb = mean_se(a), mean_se(p), mean_se(b)
    ma, mp, mb = sa[0], sp[0], sb[0]
    floor = abs(mp - ma)
    d.update(mean_a=ma, mean_p=mp, mean_b=mb, floor=floor, floor_rel=floor / ma, delta_rel=(mb - ma) / ma)
    se = math.sqrt(sa[1] ** 2 + sb[1] ** 2)
    t = t_2se(welch_df(sa, sb))
    d.update(ci_rel=((mb - ma - t * se) / ma, (mb - ma + t * se) / ma))
    if d["floor_rel"] > asked:
        return "VOID", d
    ref = list(a) + list(p)

    def bad(x, y):
        return x < y if hi else x > y

    if all(bad(x, r) for x in b for r in ref) and bad(mb, ma) and abs(mb - ma) > floor:
        return "WORSE", d
    if all(bad(r, x) for x in b for r in ref) and bad(ma, mb) and abs(mb - ma) > floor:
        return "BETTER", d
    lo, up = d["ci_rel"]
    d["excluded"] = lo > -asked if hi else up < asked
    return "NOT WORSE", d


def answer(verdicts):
    """verdicts: {cell: (verdict, detail)} for the BEST_EFFORT cells' goodput -> YES / UNDECIDED / NO / NOT SHOWN."""
    vs = [verdicts[c][0] for c in BE_CELLS]
    if "WORSE" in vs:
        return "YES"
    if "VOID" in vs or "NO VERDICT" in vs:
        return "UNDECIDED"
    if all(v == "BETTER" or verdicts[c][1].get("excluded") for c, v in zip(BE_CELLS, vs)):
        return "NO"
    return "NOT SHOWN"


EXIT = {"NO": 0, "YES": 1, "UNDECIDED": 3, "NOT SHOWN": 3}


# ------------------------------------------------------------------------------------------- pre-registration
def build_prereg(ab_path):
    with open(ab_path, "rb") as fh:
        raw = fh.read()
    ab = json.loads(raw)
    if ab.get("gate") != "v2":
        raise Refused("the ab_samehost pre-registration is not gate v2 (A P B B P A)")
    arms = ab["arms"]
    if sorted(arms) != ["A", "B", "P"]:
        raise Refused(f"arms {sorted(arms)}: want exactly A, B and P")
    if arms["A"] == arms["B"]:
        raise Refused("A and B are the same commit")
    fl = ab["flags"]
    if fl["B"] != fl["A"]:
        raise Refused(f"B's flags '{fl['B']}' differ from A's '{fl['A']}': B must differ from A by its code only")
    if fl["P"] == fl["A"] or arms["P"] != arms["A"]:
        raise Refused("P must be A's commit under other flags")
    keys = [c["key"] for c in ab["cells"]]
    for k in BE_CELLS + SECOND_VIEW:
        if k not in keys:
            raise Refused(f"cell {k} is not in the ab_samehost pre-registration ({', '.join(keys)})")
    if ab.get("be_history") != BE_HISTORY:
        raise Refused(f"be_history {ab.get('be_history')!r}: the SB cells ran -K {BE_HISTORY} on client and server")
    if ab["reps"] * 2 < MIN_N:
        raise Refused(f"REPS={ab['reps']} gives {2 * ab['reps']} repetitions per arm, fewer than MIN_N={MIN_N}")
    return {"reading": "qos_be_ab.py", "ab_prereg_sha256": hashlib.sha256(raw).hexdigest(), "arms": arms,
            "flags": fl, "reps_per_arm": 2 * ab["reps"], "dur": ab["dur"], "asked": ASKED, "min_n": MIN_N,
            "be_cells": list(BE_CELLS), "second_view": list(SECOND_VIEW),
            "metrics": [{"name": n, "higher_is_better": h} for n, h in METRICS]}


def describe(pre):
    out = ["=== qos_be_ab PRE-REGISTRATION (written before any run; summary reads only this and the ab_samehost one) ==="]
    out.append(f"  ab_samehost pre-registration sha256 {pre['ab_prereg_sha256']}")
    for a in ("A", "P", "B"):
        out.append(f"  arm {a} {pre['arms'][a]}  flags '{pre['flags'][a]}'")
    out.append(f"  {pre['reps_per_arm']} repetitions per arm (two blocks), DUR {pre['dur']} s; MIN_N {pre['min_n']}")
    for c in pre["be_cells"] + pre["second_view"]:
        role = "decides the ANSWER (goodput)" if c in pre["be_cells"] else "second view, never decides"
        out.append(f"  cell {c:28s} asked effect {100 * pre['asked'][c]:.1f}%  - {role}")
    out.append("  metrics: goodput_mbps = server.win_recv_mbps (higher better); cpu_us_per_delivered = 1e6 x (client + "
               "server sched_cpu_s) / server.recv (lower better)")
    out.append("  VOID if |P - A| / A > asked; WORSE only if every B rep is beyond every A and P rep on the bad side AND "
               "|B - A| > |P - A|; NOT WORSE qualified by the 2 x SE Welch interval against the asked effect")
    out.append("  ANSWER: YES (a BE goodput WORSE) > UNDECIDED (a VOID / NO VERDICT) > NO (all excluded) > NOT SHOWN")
    return "\n".join(out)


# ------------------------------------------------------------------------------------------------- summary
def summary(pre, prefix, out=print):
    ab_path = f"{prefix}.prereg.json"
    with open(ab_path, "rb") as fh:
        raw = fh.read()
    if hashlib.sha256(raw).hexdigest() != pre["ab_prereg_sha256"]:
        raise Refused(f"{ab_path} is not the ab_samehost pre-registration this reading was registered against")
    ab = json.loads(raw)
    data, _bins, notes, extra = ab_samehost.load(ab, prefix)
    out("=== SAMPLE COUNTS (ON repetitions counted: window=ok, insn=ok on client and server, identity checked) ===")
    for c in pre["be_cells"] + pre["second_view"]:
        out(f"  {c:28s} " + "  ".join(f"{a} n={len(data.get((a, c), {}).get('ON', []))}" for a in ("A", "P", "B"))
            + f"   (registered {pre['reps_per_arm']} per arm)")
    for n in notes:
        out(f"  note: {n}")
    out("=== REALIZED COMMIT AND FLAGS, per file ===")
    folder, base = os.path.split(prefix)
    pat = re.compile(re.escape(base) + r"\.b(\d+)_([A-Z])_([0-9a-f]{8})_(.+)\.txt$")
    for name in sorted(os.listdir(folder or ".")):
        m = pat.match(name)
        if not m:
            continue
        info, hsha = {}, "?"
        with open(os.path.join(folder, name), encoding="utf-8", errors="replace") as fh:
            for line in fh:
                h = ab_samehost.HEADER.search(line)
                if h:
                    hsha = h[1]
                    break
        ab_samehost.read_file(os.path.join(folder, name), hsha, {"scen": "", "size": ""}, info)
        fl = info.get("flags", {})
        out(f"  b{m[1]} arm {m[2]} file {m[3]} header sha={hsha[:12]} flags ON '{fl.get('ON', '-')}' OFF "
            f"'{fl.get('OFF', '-')}' client ON {info.get('ON', '-')} OFF {info.get('OFF', '-')}")
    sent = extra.get("sentinel", {})
    if sent:
        out("=== SENTINEL (A's build of the same cell in every block; the Pi's drift - printed, never decides) ===")
        for c, per_block in sent.items():
            means = {}
            for blk, (arm, w) in sorted(per_block.items()):
                v = [x for x in (metric(r, "goodput_mbps") for r in w["ON"]) if x is not None]
                if v:
                    means[blk] = statistics.fmean(v)
                out(f"  {c} block {blk} (arm {arm}): goodput "
                    + (f"{means[blk]:.1f} Mbps, n={len(v)}" if v else "no counted repetition"))
            if len(means) > 1:
                lo, hi = min(means.values()), max(means.values())
                out(f"  {c}: block means span {100 * (hi - lo) / lo:.2f}%")
    out("=== VERDICTS (judge(); the rule is this file's header) ===")
    results = {}
    for c in pre["be_cells"] + pre["second_view"]:
        for m in pre["metrics"]:
            vals = {a: [x for x in (metric(r, m["name"]) for r in data.get((a, c), {}).get("ON", []))
                        if x is not None] for a in ("A", "P", "B")}
            v, d = judge(vals["A"], vals["P"], vals["B"], m["higher_is_better"], pre["asked"][c])
            results[(c, m["name"])] = (v, d)
            if "mean_a" in d:
                ex = ""
                if v == "NOT WORSE":
                    ex = " (asked effect excluded)" if d["excluded"] else " (asked effect NOT excluded)"
                out(f"{v + ex:38s} {c:28s} {m['name']:21s} A {d['mean_a']:.5g}  P {d['mean_p']:.5g}  B "
                    f"{d['mean_b']:.5g}  B-A {100 * d['delta_rel']:+.2f}% [{100 * d['ci_rel'][0]:+.2f}, "
                    f"{100 * d['ci_rel'][1]:+.2f}]  floor |P-A| {100 * d['floor_rel']:.2f}%  asked "
                    f"{100 * pre['asked'][c]:.1f}%  n {d['n']}")
            else:
                out(f"{v:38s} {c:28s} {m['name']:21s} n (A, P, B) = {d['n']}, need {pre['min_n']} each")
            for a in ("A", "P", "B"):
                out(f"      {a}: " + " ".join(f"{x:.5g}" for x in vals[a]))
    ans = answer({c: results[(c, "goodput_mbps")] for c in pre["be_cells"]})
    cpu = answer({c: results[(c, "cpu_us_per_delivered")] for c in pre["be_cells"]})
    out(f"CPU (not deciding): did B raise client + server CPU per delivered BEST_EFFORT sample? {cpu}")
    out("SECOND VIEW (not deciding): " + "; ".join(f"{c} {m['name']} {results[(c, m['name'])][0]}"
                                                  for c in pre["second_view"] for m in pre["metrics"]))
    out(f"ANSWER: did the QoS work lower TickLE's same-host BEST_EFFORT KEEP_LAST 1 goodput? {ans}")
    return EXIT[ans]


# ------------------------------------------------------------------------------------------------ sample size
RES = re.compile(r"^\s*arm=ON RESULT: framework=tickle (.*)$")


def sb_reps(path):
    """TickLE's ON repetitions in one s6 file (client line, then its server line) -> [{client:..., server:...}]."""
    reps = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = RES.match(line)
            if not m:
                continue
            f = dict(re.findall(r"([A-Za-z_]\w*)=(\S+)", m[1]))
            if f.get("role") == "server":
                if reps and "server" not in reps[-1]:
                    reps[-1]["server"] = f
            elif f.get("window") == "ok":
                reps.append({"client": f})
    return [r for r in reps if "server" in r]


def simulate(cv, asked, hi, n, sims, seed):
    """P(WORSE) with B moved by the asked effect, and P(WORSE), P(VOID), P(excluded) with no effect."""
    rng = random.Random(seed)
    out = {}
    for name, shift in (("h1", -asked if hi else asked), ("h0", 0.0)):
        cnt = {"WORSE": 0, "VOID": 0, "excluded": 0}
        for _ in range(sims):
            a = [rng.gauss(1, cv) for _ in range(n)]
            p = [rng.gauss(1, cv) for _ in range(n)]
            b = [rng.gauss(1 + shift, cv) for _ in range(n)]
            v, d = judge(a, p, b, hi, asked)
            cnt[v] = cnt.get(v, 0) + 1
            cnt["excluded"] += bool(d.get("excluded")) or v == "BETTER"
        out[name] = {k: c / sims for k, c in cnt.items()}
    return out


def samplesize(prefix, reps_list, sims=3000):
    print("=== SAMPLE SIZE from the SB spread (within-file SD of TickLE's ON reps; normal model; this rule) ===")
    rec = {}
    for c in BE_CELLS + SECOND_VIEW:
        scen, size = c.split(":")
        path = f"{prefix}.{scen}_{size}_free.txt"
        reps = sb_reps(path)
        for name, hi in METRICS:
            v = [x for x in (metric(r, name) for r in reps) if x is not None]
            if len(v) < 2:
                print(f"  {c} {name}: {len(v)} reps in {path} - cannot estimate")
                continue
            cv = statistics.stdev(v) / statistics.fmean(v)
            print(f"  {c} {name}: n={len(v)} mean {statistics.fmean(v):.5g} CV {100 * cv:.2f}%  asked "
                  f"{100 * ASKED[c]:.1f}% = {ASKED[c] / cv:.1f} SD")
            for r in reps_list:
                s = simulate(cv, ASKED[c], hi, 2 * r, sims, f"{c}|{name}|{r}")
                print(f"    REPS {r} ({2 * r}/arm): P(WORSE | asked effect) {s['h1']['WORSE']:.2f}   no effect: "
                      f"P(WORSE) {s['h0']['WORSE']:.3f} P(VOID) {s['h0']['VOID']:.3f} P(asked excluded) "
                      f"{s['h0']['excluded']:.2f}")
                rec.setdefault(r, []).append((c, name, cv, s))
    print("RECOMMENDATION (goodput of the BEST_EFFORT cells: P(WORSE | asked effect) >= 0.8 in every cell whose asked "
          "effect is >= 3 SD, P(VOID | none) <= 0.10 and P(WORSE | none) <= 0.01 in every cell):")
    chosen = None
    for r in reps_list:
        why = []
        for c, name, cv, s in rec.get(r, []):
            if name != "goodput_mbps" or c not in BE_CELLS:
                continue
            if s["h0"]["VOID"] > 0.10:
                why.append(f"{c} false VOID {s['h0']['VOID']:.2f}")
            if s["h0"]["WORSE"] > 0.01:
                why.append(f"{c} false WORSE {s['h0']['WORSE']:.3f}")
            if ASKED[c] >= 3 * cv and s["h1"]["WORSE"] < 0.8:
                why.append(f"{c} power {s['h1']['WORSE']:.2f}")
        print(f"  REPS {r}: " + ("meets it" if not why else "does not: " + ", ".join(why)))
        if not why and chosen is None:
            chosen = r
    for c, name, cv, _s in rec.get(reps_list[0], []):
        if name == "goodput_mbps" and c in BE_CELLS and ASKED[c] < 3 * cv:
            print(f"  NOTE {c}: the asked effect is {ASKED[c] / cv:.1f} SD - inside the noise; the rule's WORSE is "
                  "unlikely there at any REPS, so that cell is read by its interval (excluded or not)")
    print(f"RECOMMENDED REPS per block: {chosen}")
    return 0


# ---------------------------------------------------------------------------------------------------- selftest
SHAS = {"A": "a" * 40, "B": "b" * 40}
FLAGS = {"A": "-falign-functions=64 -falign-loops=1", "P": "-falign-functions=128 -falign-loops=1"}
BASE = {"best_effort_throughput:p2": 20000.0, "best_effort_throughput:p3": 21000.0,
        "best_effort_throughput:p4": 24800.0, "reliable_throughput:p3": 15000.0}
DEV = (-0.002, 0.0, 0.002)   # per repetition, fixed: the outcome of each case cannot depend on chance


def fixture_env():
    cells = ";".join(BE_CELLS + SECOND_VIEW)
    return {"GATE": "v2", "A": SHAS["A"], "B": SHAS["B"], "CELLS": cells, "REPS": "3", "DUR": "5",
            "TICKLE_EXTRA_CFLAGS": FLAGS["A"], "P_EXTRA_CFLAGS": FLAGS["P"], "B_EXTRA_CFLAGS": FLAGS["A"],
            "BE_HISTORY": "1", "WITNESS": "*:server.win_recv_mbps", "CONTROL": "best_effort_throughput:p3:server.recv",
            "TREATMENT": "*:*:client.tx_shm>0", "TAG": "qosbe_fixture"}


def write_fixture(folder, ab, case):
    """s6-shaped files for every block of `ab`; case: {arm: {cell or '*': goodput factor}}, 'cpu' factors, 'reps',
    'flags' (arm -> flags its files show), 'overlap' (one B rep at A's level)."""
    os.makedirs(folder, exist_ok=True)
    prefix = os.path.join(folder, "fx")
    for n, arm in enumerate(ab_samehost.blocks(ab), 1):
        sha = ab["arms"][arm]
        shown = case.get("flags", {}).get(arm, ab["flags"][arm])
        for cell in ab["cells"]:
            key = cell["key"]
            g = case.get(arm, {}).get(key, case.get(arm, {}).get("*", 1.0))
            cf = case.get("cpu", {}).get(arm, 1.0)
            lines = [f"=== S6 transport cells 2026-10-19T00:00:00+09:00 sha={sha} scen={cell['scen']} "
                     f"size={cell['size']} dur=5s reps=3 host=10.1.1.214 iface=lo ===\n"]
            for tag in ("ON", "OFF"):
                lines.append(f"    arm {tag} (extra='{shown}') built, client sha256="
                             f"{hashlib.sha256(f'{sha}{shown}{tag}{key}'.encode()).hexdigest()[:16]}\n")
                lines.append(f"    arm {tag} TICKLE_EXTRA_CFLAGS='{shown}' reached core and bench (fixture)\n")
            for r in range(case.get("reps", 3)):
                dev = DEV[r % 3]
                if case.get("overlap") and arm == "B" and r == 0 and n == 3:
                    gr = BASE[key] * 1.004
                else:
                    gr = BASE[key] * g * (1 + dev)
                recv = int(gr * 1e6 * 9 / 8 / 1424)
                cpu = 9.0 * cf * (1 - dev)
                lines.append(f"  arm=ON RESULT: framework=tickle scenario={cell['scen']} role=client sent={recv} "
                             f"window=ok history_arg=1 tx_shm={recv} sched_cpu_s={cpu:.6f} insn_u=1000 insn_k=10 "
                             "insn=ok\n")
                lines.append(f"  arm=ON RESULT: framework=tickle scenario={cell['scen']} role=server recv={recv} "
                             f"win_recv_mbps={gr:.6f} window=ok history_arg=1 rx_shm={recv} sched_cpu_s={cpu:.6f} "
                             "insn_u=1000 insn_k=10 insn=ok\n")
            with open(f"{prefix}.b{n}_{ab_samehost.token(ab, arm)}_{cell['slug']}.txt", "w", encoding="utf-8") as fh:
                fh.writelines(lines)
    return prefix


CASES = [
    ("no change", {}, "NO", "B as A: every cell not worse, the asked effect excluded"),
    ("B -3% everywhere", {"B": {"*": 0.97}}, "YES", "every B rep below every A and P rep, beyond the floor"),
    ("B -3% on p3 only", {"B": {"best_effort_throughput:p3": 0.97}}, "YES", "one cell is enough"),
    ("placement 4% on p2", {"P": {"best_effort_throughput:p2": 1.04}, "B": {"*": 0.97}}, "YES",
     "p2 VOID (floor 4% > 2.7%), but p3 and p4 WORSE still answer YES"),
    ("placement 4% alone", {"P": {"best_effort_throughput:p2": 1.04}}, "UNDECIDED", "p2 VOID, nothing worse"),
    ("one B rep overlaps", {"B": {"best_effort_throughput:p3": 0.97}, "overlap": True}, "NOT SHOWN",
     "a B rep at A's level: not every B rep is beyond, and p3's interval cannot exclude 3.2%"),
    ("B -0.3% on p4", {"B": {"best_effort_throughput:p4": 0.997}}, "NO",
     "a move inside the noise (the reps overlap): not worse, and -1.2% excluded"),
    ("B -0.6% on p4", {"B": {"best_effort_throughput:p4": 0.994}}, "YES",
     "smaller than asked, but every B rep below every A and P rep with a zero floor: a reduction is a reduction"),
    ("two reps per arm", {"reps": 1}, "UNDECIDED", "n = 2 < MIN_N: NO VERDICT, never read as held"),
    ("B built with P's flags", {"flags": {"B": FLAGS["P"]}}, "UNDECIDED",
     "B's files do not show B's registered flags: not read, so B n = 0"),
    ("B CPU +3%", {"cpu": {"B": 1.03}}, "NO", "CPU worse does not decide the goodput ANSWER (printed beside it)"),
]


def selftest():
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, case, want, why) in enumerate(CASES):
            folder = os.path.join(tmp, f"c{i}")
            os.makedirs(folder)
            ab = ab_samehost.build_prereg(fixture_env())
            prefix = os.path.join(folder, "fx")
            with open(f"{prefix}.prereg.json", "w", encoding="utf-8") as fh:
                json.dump(ab, fh, indent=1, sort_keys=True)
            pre = build_prereg(f"{prefix}.prereg.json")
            write_fixture(folder, ab, case)
            lines = []
            summary(pre, prefix, out=lines.append)
            got = next(ln for ln in lines if ln.startswith("ANSWER:")).rsplit("? ", 1)[1]
            ok = got == want
            fails += not ok
            print(f"{'ok  ' if ok else 'FAIL'} {name:24s} want {want:10s} got {got:10s} - {why}")
            if name == "B CPU +3%":
                cpu = next(ln for ln in lines if ln.startswith("CPU"))
                c_ok = cpu.endswith("YES")
                fails += not c_ok
                print(f"{'ok  ' if c_ok else 'FAIL'}   ... and its CPU line says YES: {cpu}")
            if not ok:
                print("\n".join(lines))
        # Refusals of a pre-registration that is not this design.
        for name, change in (("no BE_HISTORY", {"BE_HISTORY": ""}),
                             ("B under other flags", {"B_EXTRA_CFLAGS": FLAGS["P"]}),
                             ("a cell missing", {"CELLS": ";".join(BE_CELLS)})):
            env = dict(fixture_env(), **change)
            try:
                ab = ab_samehost.build_prereg(env)
                path = os.path.join(tmp, "r.json")
                with open(path, "w", encoding="utf-8") as fh:
                    json.dump(ab, fh)
                build_prereg(path)
                print(f"FAIL refusal not raised: {name}")
                fails += 1
            except (Refused, ab_samehost.Refused) as err:
                print(f"ok   refused ({name}): {err}")
        # The pre-registration is bound to its ab_samehost file: an edited one is refused.
        ab = ab_samehost.build_prereg(fixture_env())
        folder = os.path.join(tmp, "edited")
        os.makedirs(folder)
        prefix = os.path.join(folder, "fx")
        with open(f"{prefix}.prereg.json", "w", encoding="utf-8") as fh:
            json.dump(ab, fh)
        pre = build_prereg(f"{prefix}.prereg.json")
        with open(f"{prefix}.prereg.json", "a", encoding="utf-8") as fh:
            fh.write(" ")
        try:
            summary(pre, prefix, out=lambda _l: None)
            print("FAIL an edited ab_samehost pre-registration was read")
            fails += 1
        except Refused as err:
            print(f"ok   refused (edited ab pre-registration): {err}")
    print(f"selftest: {'PASS' if fails == 0 else f'FAIL ({fails})'}")
    return 1 if fails else 0


def main(argv):
    try:
        if len(argv) == 4 and argv[1] == "--prereg":
            pre = build_prereg(argv[2])
            print(describe(pre))
            with open(argv[3], "w", encoding="utf-8") as fh:
                json.dump(pre, fh, indent=1, sort_keys=True)
            return 0
        if len(argv) == 2 and argv[1] == "--selftest":
            return selftest()
        if len(argv) in (3, 4) and argv[1] == "--samplesize":
            reps = [int(x) for x in (argv[3] if len(argv) == 4 else "2,3,4,5").split(",")]
            return samplesize(argv[2], reps)
        if len(argv) == 4 and argv[1] == "summary":
            with open(argv[2], encoding="utf-8") as fh:
                pre = json.load(fh)
            print(describe(pre))
            return summary(pre, argv[3])
    except (Refused, ab_samehost.Refused) as err:
        print(f"REFUSED: {err}", file=sys.stderr)
        return 2
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
