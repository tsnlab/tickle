#!/usr/bin/env python3
"""gate_xval_read.py - the pre-registered reading of gate v2's own re-validation after A1'' (TESTING.md 6.1, "Validating
the gate itself"), one chain at a time, as rig_queue_gate_xval.sh runs them.

Written 2026-10-17 before any run it reads. It computes no statistic: every row's verdict, t, threshold and floor are
gate_v2.py's, as ab_compare.py campaign --placement printed them into the chain's compare file.

THE RUNS. Both are A against A itself - placement-only "changes" - on the core cells, gate v2, A P B B P A, REPS=3:
  X1  B = A's commit with A's flags (A/A). The chain's own P arm (-falign-functions=128 + the no-padding flags) is the
      A-vs-P comparison: it shows as the PLACEMENT and NOTE lines.
  X2  B = A's commit at -falign-functions=256 with the same no-padding flags (and -mno-fix-cortex-a53-843419 where the
      build host is aarch64): a placement P did not draw. A B with P's own flags is not used: the floor is one draw
      of placement, so it reads WORSE about half the time (TESTING.md 6.1).

RULE (the expectation TESTING.md 6.1 states, decided here in code):
  PASS  no PRIMARY row WORSE, AND every latency cell's (shape L) insn_u_per_sample row (client and server) has |t| at
        or below its threshold - in either direction: "better" or held(placement-sized) there is a fail too, because
        user instructions in a latency cell must not move with placement at all.
  FAIL  either broken; every offending row is listed. An instruction move in a latency cell means a layout path A1''
        did not cover; a PRIMARY WORSE means the floor or the threshold is too tight for a build against itself.
  VOID  (exit 3) - core_ab_read.read()'s checks (a missing or empty file, the chain's own VOID/REFUSED, gate v2 NO
        VERDICT or no verdict line, k not equal to the rows parsed, a requested cell with no CPU witness row, a latency
        cell with no rtt row), and here: the chain log's header does not show B = A's commit, or does not show the
        flags this run was given for A and B (an arm must report its treatment); a latency cell with no
        insn_u_per_sample row; a deciding row whose t or threshold is not a number (could not look).
REPORTED, never deciding: gate v2's own verdict line (it also counts WITNESS rows); WITNESS rows WORSE (a throughput
CPU or rate move beyond P's floor - the expectation is that there are none); PRIMARY rows held(placement-sized) in
throughput cells (beyond the threshold, inside the floor: expected, timing); every NOTE of a PRIMARY moving between A and
P beyond its threshold, marked when the cell is a latency cell (the expectation is none there).
Last line: "GATE XVAL <name> VERDICT: PASS|FAIL|VOID (...)". Exit 0 PASS, 1 FAIL, 3 VOID, 2 usage.

  gate_xval_read.py --name X1 --compare F --done F --chain-log F --cells "1 2 ..." --a SHA \
                    --a-flags '<A flags>' --b-flags '<B flags>' [--stand-in '<why>']
  gate_xval_read.py --selftest
"""
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import core_ab_read  # noqa: E402  (the shared VOID checks and the row format)

ROW = re.compile(
    r"^(?P<fam>PRIMARY|WITNESS)\s+(?P<verdict>\S+)\s+(?P<cell>c(?P<num>\d+) (?P<shape>[A-Z]) \S+ \S+ \S+)\s+"
    r"(?P<metric>\S+)\s+A\s+\S+ \(se \S+, n\d+\)\s+B\s+\S+ \(se \S+, n\d+\)\s+[+-]?[\d.na]+%"
    r"\s+t (?P<t>\S+) \(> (?P<thr>\S+)\)(?:\s+floor \S+ \(\S+%\))?\s*$")
NOTE = re.compile(r"^NOTE: (?P<cell>c(?P<num>\d+) (?P<shape>[A-Z]) \S+ \S+ \S+) (?P<metric>\S+) moved between A and P")
HEAD = re.compile(r"^=== campaign_ab_chain .* A=(?P<a>\S+) B=(?P<b>\S+) .*flags A='(?P<fa>[^']*)' P='(?P<fp>[^']*)' "
                  r"B='(?P<fb>[^']*)' harness=")


def num(s):
    try:
        return float(s)
    except ValueError:
        return float("nan")


def read(name, compare, done, chain_log, cells, a_sha, a_flags, b_flags):
    """Returns (verdict, reason, lines)."""
    base, base_reason, lines = core_ab_read.read(compare, done, cells)
    void = [base_reason] if base == "VOID" else []
    # Every line core_ab_read printed except its own landing-rule tallies (a different rule from this one).
    lines = [ln for ln in lines if not ln.startswith(("  BLOCKING", "  OTHER", "  REPORTED user insn"))
             and not re.match(r"^  \d+/\d+ WORSE", ln)]
    # The treatment each arm actually got, from the chain's own header line.
    try:
        with open(chain_log, encoding="utf-8", errors="replace") as fh:
            heads = [m for m in map(HEAD.match, fh) if m]
    except OSError as err:
        heads = []
        void.append(f"chain log not read ({err.strerror})")
    if not heads:
        void.append("the chain log has no campaign_ab_chain header (the arms' treatment is unknown)")
    else:
        h = heads[-1]
        lines.append(f"treatment: A={h['a'][:12]} B={h['b'][:12]} flags A='{h['fa']}' P='{h['fp']}' B='{h['fb']}'")
        if h["a"] != a_sha or h["b"] != a_sha:
            void.append(f"the chain ran A={h['a'][:12]} B={h['b'][:12]}, not A = B = {a_sha[:12]}")
        if h["fa"] != a_flags:
            void.append(f"arm A was built with '{h['fa']}', not '{a_flags}'")
        if h["fb"] != b_flags:
            void.append(f"arm B was built with '{h['fb']}', not '{b_flags}'")
    rows, notes = [], []
    try:
        with open(compare, encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                m = ROW.match(raw)
                if m:
                    rows.append(dict(m.groupdict(), line=raw.rstrip()))
                    continue
                m = NOTE.match(raw)
                if m:
                    notes.append(dict(m.groupdict(), line=raw.rstrip()))
    except OSError:
        pass  # core_ab_read already made it VOID
    for r in rows:
        if math.isnan(num(r["t"])) or math.isnan(num(r["thr"])):
            void.append(f"c{r['num']} {r['metric']}: t {r['t']} against {r['thr']} is not a number")
    fail = []
    for r in rows:
        if r["fam"] == "PRIMARY" and r["verdict"] == "WORSE":
            fail.append(("PRIMARY WORSE", r))
        elif r["fam"] == "PRIMARY" and r["shape"] == "L" and r["metric"].endswith(".insn_u_per_sample") \
                and abs(num(r["t"])) > num(r["thr"]):
            fail.append(("latency insn_u beyond its threshold", r))
    for c in cells:
        mine = [r for r in rows if r["num"] == str(c)]
        if mine and mine[0]["shape"] == "L" and not any(r["metric"].endswith(".insn_u_per_sample") for r in mine):
            void.append(f"latency cell c{c} has no insn_u_per_sample row")
    for why, r in fail:
        lines.append(f"  FAIL ({why}): {r['line']}")
    for r in rows:
        if r["fam"] == "WITNESS" and r["verdict"] == "WORSE":
            lines.append(f"  reported, WITNESS WORSE: {r['line']}")
    sized = [r for r in rows if r["fam"] == "PRIMARY" and r["verdict"].startswith("held(placement")]
    for r in sized:
        lines.append(f"  reported, PRIMARY beyond the threshold inside P's floor: {r['line']}")
    for n in notes:
        tag = "LATENCY CELL - " if n["shape"] == "L" else ""
        lines.append(f"  reported, {tag}A vs P: {n['line']}")
    lines.append(f"  {len(rows)} deciding rows: {len(fail)} failing this rule; {len(sized)} PRIMARY inside P's floor; "
                 f"{sum(r['fam'] == 'WITNESS' and r['verdict'] == 'WORSE' for r in rows)} WITNESS WORSE; "
                 f"{len(notes)} A-vs-P NOTE(s), {sum(n['shape'] == 'L' for n in notes)} in latency cells")
    if void:
        return "VOID", "; ".join(void), lines
    if fail:
        return "FAIL", f"{len(fail)} row(s): " + ", ".join(f"c{r['num']} {r['metric']} ({why})" for why, r in fail), lines
    nlat = sum(1 for r in rows if r["shape"] == "L" and r["metric"].endswith(".insn_u_per_sample"))
    return "PASS", f"no PRIMARY WORSE; all {nlat} latency insn_u rows within their threshold", lines


def main(argv):
    if argv[1:] == ["--selftest"]:
        return selftest()
    opt = {k: None for k in ("--name", "--compare", "--done", "--chain-log", "--cells", "--a", "--a-flags",
                             "--b-flags")}
    opt["--stand-in"] = ""
    args = argv[1:]
    while args:
        if args[0] not in opt or len(args) < 2:
            print(__doc__)
            return 2
        opt[args[0]] = args[1]
        args = args[2:]
    if any(v is None for v in opt.values()):
        print(__doc__)
        return 2
    try:
        cells = [int(c) for c in opt["--cells"].split()]
    except ValueError:
        print("--cells takes campaign cell numbers")
        return 2
    print(f"=== gate xval {opt['--name']}: A {opt['--a'][:12]} against itself, B flags '{opt['--b-flags']}', "
          f"cells [{opt['--cells']}], {opt['--compare']}")
    if opt["--stand-in"]:
        print(f"STAND-IN: {opt['--stand-in']} - not the pre-registered stage; the verdict below says nothing of the rig")
    verdict, reason, lines = read(opt["--name"], opt["--compare"], opt["--done"], opt["--chain-log"], cells,
                                  opt["--a"], opt["--a-flags"], opt["--b-flags"])
    for ln in lines:
        print(ln)
    print(f"GATE XVAL {opt['--name']} VERDICT: {verdict} ({reason})")
    return {"PASS": 0, "FAIL": 1, "VOID": 3}[verdict]


# ---------------------------------------------------------------------------------------------------------- selftest
def selftest():
    """Fixtures printed by gate_v2.report() itself, so the parser is checked against the producer's real format."""
    import io
    import tempfile
    from contextlib import redirect_stdout
    import gate_v2

    sha = "a" * 40
    fa, fb = "-falign-functions=64 -falign-loops=1", "-falign-functions=256 -falign-loops=1"
    same, pp = (100.0, 0.05, 6), (100.01, 0.05, 6)

    def table(over=None, p_over=None):
        """c1 (T) and c10 (L); over / p_over map 'cN:metric' to B's / P's stats."""
        over, p_over = over or {}, p_over or {}
        spec = [("c1 T p1 Q0 none", "server.cpu_s_per_Msample", "witness"),
                ("c1 T p1 Q0 none", "client.insn_u_per_sample", "primary"),
                ("c10 L p1 Q0 none", "client.cpu_s_per_Msample", "witness"),
                ("c10 L p1 Q0 none", "client.rtt_avg_ms", "witness"),
                ("c10 L p1 Q0 none", "client.insn_u_per_sample", "primary"),
                ("c10 L p1 Q0 none", "server.insn_u_per_sample", "primary")]
        rows = []
        for cell, metric, fam in spec:
            key = cell.split()[0] + ":" + metric
            rows.append({"cell": cell, "metric": metric, "family": fam, "a": same, "b": over.get(key, same),
                         "p": p_over.get(key, pp), "hi_better": False})
        buf = io.StringIO()
        with redirect_stdout(buf):
            gate_v2.report(rows, title="A a.txt  B b.txt  P p.txt")
        return buf.getvalue()

    def head(a=sha, b=sha, xa=fa, xb=fb):
        return (f"=== campaign_ab_chain 2026-10-17T03:00:00+09:00 GATE=v2 TAG=t A={a} B={b} REPS=3 blocks: A P B B P A"
                f" flags A='{xa}' P='-falign-functions=128 -falign-loops=1' B='{xb}' harness=abc1234 ===\n")

    done = "=== chain done 2026-10-17T05:00:00+09:00: VERDICT: PASS (no deciding comparison) ===\n"
    up, down, far = (110.0, 0.05, 6), (99.0, 0.05, 6), (85.0, 0.05, 6)
    cases = [
        ("all held -> PASS", table(), head(), [1, 10], "PASS"),
        ("throughput PRIMARY WORSE -> FAIL", table({"c1:client.insn_u_per_sample": up}), head(), [1, 10], "FAIL"),
        ("latency insn_u better beyond threshold, beyond floor -> FAIL",
         table({"c10:server.insn_u_per_sample": down}), head(), [1, 10], "FAIL"),
        ("latency insn_u beyond threshold inside floor (held(placement-sized)) -> FAIL",
         table({"c10:client.insn_u_per_sample": down}, {"c10:client.insn_u_per_sample": far}), head(), [1, 10],
         "FAIL"),
        ("throughput insn_u beyond threshold inside floor -> PASS (reported)",
         table({"c1:client.insn_u_per_sample": down}, {"c1:client.insn_u_per_sample": far}), head(), [1, 10],
         "PASS"),
        ("WITNESS CPU WORSE -> PASS (reported)", table({"c1:server.cpu_s_per_Msample": up}), head(), [1, 10],
         "PASS"),
        ("latency rtt WORSE -> PASS here (reported; core_ab_read decides landings, not this)",
         table({"c10:client.rtt_avg_ms": up}), head(), [1, 10], "PASS"),
        ("B is another commit -> VOID", table(), head(b="b" * 40), [1, 10], "VOID"),
        ("B built with other flags -> VOID", table(), head(xb=fa), [1, 10], "VOID"),
        ("no chain header -> VOID", table(), "", [1, 10], "VOID"),
        ("requested cell absent -> VOID", table(), head(), [1, 10, 4], "VOID"),
        ("empty compare -> VOID", "", head(), [1, 10], "VOID"),
        ("t not a number -> VOID", table().replace("t +0.0 (>", "t nan (>", 1), head(), [1, 10], "VOID"),
    ]
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        cp, dp, lp = (os.path.join(d, n) for n in ("c.txt", "d.txt", "chain.log"))
        for name, compare, chainlog, cells, want in cases:
            for path, text in ((cp, compare), (dp, done), (lp, "--- block 1\n" + chainlog)):
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(text)
            got, reason, lines = read("X", cp, dp, lp, cells, sha, fa, fb)
            ok = got == want
            if want == "FAIL":
                ok = ok and any(ln.startswith("  FAIL (") for ln in lines)
            if "(reported" in name:
                ok = ok and any(ln.startswith("  reported, ") for ln in lines)
            bad += not ok
            print(f"{'ok  ' if ok else 'BAD '} {name}: {got} ({reason[:110]})")
        # The nan case must have reached the row parser: a fixture that substituted nothing would pass by accident.
        nan_fixture = table().replace("t +0.0 (>", "t nan (>", 1)
        sub = nan_fixture != table()
        bad += not sub
        print(f"{'ok  ' if sub else 'BAD '} the nan fixture differs from the plain table")
    print(f"SELFTEST {'OK' if not bad else 'FAILED'} ({len(cases) + 1 - bad}/{len(cases) + 1})")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
