#!/usr/bin/env python3
"""core_ab_read.py - the landing rule over a gate v2 core-cell A/B (rig_queue_core_ab.sh, stage "core").

Written 2026-10-15 before any run it reads. It does not compute a statistic: every row's verdict (WORSE / better / held,
the Bonferroni Student t and the placement floor |P - A|) is gate_v2.py's, as ab_compare.py campaign --placement printed
it into the chain's compare file. This file only decides which of those verdicts block landing.

INPUTS. --compare <TAG>_compare.txt and --done <TAG>.done (campaign_ab_chain.sh, GATE=v2), --cells "<n> ..." (the
campaign cell numbers the chain was asked for). --a / --b only label the output.

LANDING RULE (the user's, 2026-10-14/15, applied to rx-gap3b and rx-gap3c):
  FAIL  any deciding row WORSE whose metric is CPU (cpu_s_per_Msample, every role) or latency (rtt_*), B against A.
  PASS  otherwise. Every other WORSE row (instructions, rate) is listed and counted, "n/k WORSE", and does not block.
VOID (nothing is judged; exit 3), any of:
  - the compare or the done file is missing or empty (could not look);
  - the done line is not "=== chain done": the chain's own VOID / REFUSED (block 1 FAIL FAST, a block's build flags
    not echoed or not verified per Pi, no INSN_COUNT=1 line, a preflight refusal);
  - gate v2 printed NO VERDICT (no instruction counts, a pre-registered item that selected nothing, a deciding
    comparison without the placement arm P), or printed no verdict line at all;
  - the gate v2 header's k does not equal the number of PRIMARY + WITNESS rows parsed here (a row this reader cannot
    parse must not pass by being skipped);
  - a requested cell has no CPU witness row, or a latency cell (L) or service cell (S) has no rtt row: the rule could
    not look there;
  - service cells (S) were read but the arm files were not given (--arm-a/--arm-b/--arm-p) or one is missing: the
    duplicate-execution falsifier below could not look.
SERVICE CELLS (S, campaign_sweep.sh c18-c21; added 2026-10-21, before any rig run of them): a row whose verdict is
DEFECT(dup_exec:...) - the server's callback answered some call twice (service_cell_verdict.py) - never reaches the
compare file (only ok rows are compared), so the arm files themselves are read:
  FAIL  any DEFECT row in arm B, whatever else is VOID: a duplicate execution is the cells' falsifier ("exactly one
        execution per call"), a finding about B's code, and must never read as "could not look". It fails B even
        when A has DEFECT rows too: the criterion is B's own, not B against A.
  REPORTED  DEFECT rows in A or P (and how many): the control runs a callback twice. Not a VOID - those rows are left
        out of A's statistics like any non-ok row, and the comparison of the rest stands - but listed, because then
        main itself fails the criterion.
REPORTED, never deciding: gate v2's own verdict line; cells with void rows; PLACEMENT P vs A "beyond" rows and gate v2's
NOTE lines (A and P drifting apart: the run-to-run spread the floor absorbs); PRIMARY user-instruction rows with
B - A above +0.1% and above their floor (the rx-gap3 queues' "core regression" line, kept as a report).
Last line: "CORE AB VERDICT: PASS|FAIL|VOID (...)". Exit status 0 PASS, 1 FAIL, 3 VOID, 2 usage.

  core_ab_read.py --compare F --done F --cells "1 2 ..." [--a SHA] [--b SHA] [--arm-a F --arm-b F --arm-p F]
  core_ab_read.py --selftest
"""
import io
import os
import re
import sys
from contextlib import redirect_stdout

# The cell label is "cN <shape> <payload> <qos> <network>", and the network may hold spaces ("loss 5% each way", the
# service cells' N4): the label ends where the metric begins, and a metric is always <role>.<name>.
ROW = re.compile(
    r"^(?P<fam>PRIMARY|WITNESS)\s+(?P<verdict>\S+)\s+(?P<cell>c(?P<num>\d+) (?P<shape>[A-Z]) \S+ \S+ .+?)\s+"
    r"(?P<metric>[a-z]+\.[A-Za-z_]\w*)\s+A\s+(?P<a>\S+) \(se \S+, n\d+\)\s+B\s+(?P<b>\S+) \(se \S+, n\d+\)\s+(?P<pct>[+-]?[\d.na]+)%"
    r"\s+t \S+ \(> \S+\)(?:\s+floor (?P<floor>\S+) \(\S+%\))?\s*$")
HEADER = re.compile(r"^--- gate v2: (\d+) primary \(instructions\) \+ (\d+) witness comparison\(s\), k = (\d+);")
USER_INSN_PCT = 0.1
# A campaign_sweep.sh row of a TickLE run whose verdict is DEFECT(...) (service_cell_verdict.py).
DEFECT_ROW = re.compile(r"^c(?P<num>\d+)\s+[A-Z]\s+p\d\s+Q\d\s+.+?\s+tickle\s+rep(?P<rep>\d+)\s+\|\s+"
                        r"(?P<verdict>DEFECT\(\S*?\))\s+\|")


def arm_defects(path):
    """The DEFECT rows of one arm file as "cN repR DEFECT(...)", or None when the file cannot be read or is empty."""
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None
    if not text.strip():
        return None
    return [f"c{m['num']} rep{m['rep']} {m['verdict']}" for m in map(DEFECT_ROW.match, text.splitlines()) if m]


def blocking(metric):
    """True for a metric the landing rule blocks on: CPU time per sample, or latency."""
    name = metric.split(".", 1)[-1]
    return name == "cpu_s_per_Msample" or name.startswith("rtt_")


def read(compare, done, cells, arms=None):
    """Returns (verdict, reason, lines). verdict: PASS | FAIL | VOID. arms: {"A": path, "B": path, "P": path}, the
    chain's arm files, read for service-cell DEFECT rows; None when not given."""
    lines, void = [], []
    texts = {}
    for what, path in (("compare", compare), ("done", done)):
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                texts[what] = fh.read()
        except OSError as err:
            texts[what] = ""
            void.append(f"{what} file not read ({err.strerror})")
            continue
        if not texts[what].strip():
            void.append(f"{what} file {path} is empty")
    done_line = texts["done"].strip().splitlines()[-1] if texts["done"].strip() else ""
    lines.append(f"chain: {done_line or '(none)'}")
    if done_line and not done_line.startswith("=== chain done"):
        void.append("the chain did not end done: " + done_line[:160])
    rows, k, gate_line, notes, voidcells, beyond = [], None, "", [], [], []
    for raw in texts["compare"].splitlines():
        m = HEADER.match(raw)
        if m:
            k = int(m.group(3))
            continue
        if raw.startswith(("VERDICT:", "NO VERDICT")):
            gate_line = raw
            continue
        if raw.startswith("NOTE:"):
            notes.append(raw)
            continue
        if re.match(r"^c\d+ .*: void A ", raw):
            voidcells.append(raw)
            continue
        if raw.startswith("PLACEMENT") and "P vs A beyond" in raw:
            beyond.append(raw)
            continue
        m = ROW.match(raw)
        if m:
            rows.append(dict(m.groupdict(), line=raw.rstrip()))
    lines.append(f"gate v2 (reported; the landing rule below decides): {gate_line or '(no verdict line)'}")
    if texts["compare"].strip():
        if gate_line.startswith("NO VERDICT"):
            void.append("gate v2 " + gate_line[:160])
        elif not gate_line:
            void.append("gate v2 printed no verdict line")
        if k is None:
            void.append("no gate v2 header (k unknown)")
        elif k != len(rows):
            void.append(f"gate v2 says k = {k}, this reader parsed {len(rows)} PRIMARY/WITNESS row(s)")
    for c in cells:
        mine = [r for r in rows if r["num"] == str(c)]
        if not mine:
            void.append(f"cell c{c} has no deciding row")
            continue
        if not any(r["metric"].endswith(".cpu_s_per_Msample") for r in mine):
            void.append(f"cell c{c} has no CPU witness row")
        if mine[0]["shape"] in ("L", "S") and not any(r["metric"].split(".", 1)[-1].startswith("rtt_") for r in mine):
            void.append(f"{'latency' if mine[0]['shape'] == 'L' else 'service'} cell c{c} has no rtt row")
    # Service cells: the duplicate-execution falsifier, read from the arm files (a DEFECT row is not an ok row, so the
    # compare file never shows it).
    b_defects, ctl_defects = [], []
    if any(r["shape"] == "S" for r in rows) or arms:
        if not arms:
            void.append("service cells read without the arm files (--arm-a/--arm-b/--arm-p): duplicates not looked for")
        else:
            for arm in ("A", "P", "B"):
                found = arm_defects(arms[arm]) if arms.get(arm) else None
                if found is None:
                    void.append(f"arm {arm} file {arms.get(arm) or '(not given)'} not read: duplicates not looked for")
                    continue
                for d in found:
                    lines.append(f"  DEFECT arm {arm}: {d}")
                (b_defects if arm == "B" else ctl_defects).extend(f"{arm} {d}" for d in found)
            if ctl_defects:
                lines.append(f"  REPORTED the control ran a service callback twice: {len(ctl_defects)} DEFECT row(s) "
                             f"in A/P ({', '.join(ctl_defects[:4])}) - main itself fails the one-execution criterion")
    worse = [r for r in rows if r["verdict"] == "WORSE"]
    block = [r for r in worse if blocking(r["metric"])]
    other = [r for r in worse if not blocking(r["metric"])]
    for r in block:
        lines.append(f"  BLOCKING  {r['line']}")
    for r in other:
        lines.append(f"  OTHER     {r['line']}")
    for raw in voidcells:
        lines.append(f"  void rows: {raw}")
    for raw in beyond + notes:
        lines.append(f"  placement: {raw}")
    for r in rows:
        if r["fam"] != "PRIMARY" or not r["metric"].endswith(".insn_u_per_sample") or r["floor"] is None:
            continue
        try:
            pct, diff, floor = float(r["pct"]), float(r["b"]) - float(r["a"]), float(r["floor"])
        except ValueError:
            continue
        if pct > USER_INSN_PCT and diff > floor:
            lines.append(f"  REPORTED user insn > +{USER_INSN_PCT}% and floor: {r['line']}")
    kk = k if k is not None else len(rows)
    lines.append(f"  {len(worse)}/{kk} WORSE ({len(block)} CPU/latency, blocking; {len(other)} other, listed)")
    if b_defects:
        why = f"B ran a service callback twice for one call: {len(b_defects)} DEFECT row(s), " + ", ".join(b_defects[:4])
        if void:
            why += f"; also VOID otherwise ({'; '.join(void)[:200]})"
        return "FAIL", why, lines
    if void:
        return "VOID", "; ".join(void), lines
    if block:
        names = ", ".join(f"c{r['num']} {r['metric']}" for r in block)
        return "FAIL", f"{len(block)} CPU/latency row(s) WORSE vs A: {names}; {len(worse)}/{kk} WORSE", lines
    return "PASS", f"no CPU witness or latency row WORSE vs A; {len(worse)}/{kk} WORSE (listed, not blocking)", lines


def main(argv):
    if argv[1:] == ["--selftest"]:
        return selftest()
    opt = {"--compare": None, "--done": None, "--cells": None, "--a": "", "--b": "", "--arm-a": None, "--arm-b": None,
           "--arm-p": None}
    args = argv[1:]
    while args:
        if args[0] not in opt or len(args) < 2:
            print(__doc__)
            return 2
        opt[args[0]] = args[1]
        args = args[2:]
    if not (opt["--compare"] and opt["--done"] and opt["--cells"]):
        print(__doc__)
        return 2
    try:
        cells = [int(c) for c in opt["--cells"].split()]
    except ValueError:
        print("--cells takes campaign cell numbers")
        return 2
    print(f"=== core A/B landing rule: A {opt['--a'] or '?'} vs B {opt['--b'] or '?'}, cells [{opt['--cells']}],"
          f" {opt['--compare']}")
    arms = {arm: opt[f"--arm-{arm.lower()}"] for arm in "ABP" if opt[f"--arm-{arm.lower()}"]} or None
    verdict, reason, lines = read(opt["--compare"], opt["--done"], cells, arms)
    for ln in lines:
        print(ln)
    print(f"CORE AB VERDICT: {verdict} ({reason})")
    return {"PASS": 0, "FAIL": 1, "VOID": 3}[verdict]


# ---------------------------------------------------------------------------------------------------------- selftest
def selftest():
    """Fixtures printed by gate_v2.report() itself, so the parser is checked against the producer's real format."""
    import tempfile
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gate_v2

    same, up, down = (100.0, 0.05, 6), (110.0, 0.05, 6), (90.0, 0.05, 6)
    pp = (100.01, 0.05, 6)

    def row(cell, metric, fam, b, p=pp):
        return {"cell": cell, "metric": metric, "family": fam, "a": same, "b": b, "p": p, "hi_better": False}

    def table(**over):
        """Two cells, c1 (T) and c10 (L); over maps metric keys to B's stats."""
        spec = [("c1 T p1 Q0 none", "server.cpu_s_per_Msample", "witness"),
                ("c1 T p1 Q0 none", "client.insn_u_per_sample", "primary"),
                ("c1 T p1 Q0 none", "server.recv_mbps", "witness"),
                ("c10 L p1 Q0 none", "client.cpu_s_per_Msample", "witness"),
                ("c10 L p1 Q0 none", "client.rtt_avg_ms", "witness"),
                ("c10 L p1 Q0 none", "server.insn_u_per_sample", "primary")]
        rows = []
        for cell, metric, fam in spec:
            key = cell.split()[0] + ":" + metric
            r = row(cell, metric, fam, over.get(key, same))
            if metric.endswith("recv_mbps"):
                r["hi_better"] = True
            if over.get("noP") == key:
                r["p"] = None
            rows.append(r)
        buf = io.StringIO()
        with redirect_stdout(buf):
            gate_v2.report(rows, title="A a.txt  B b.txt  P p.txt")
        return buf.getvalue()

    done_ok = "=== chain done 2026-10-15T03:44:28+09:00: VERDICT: WORSE (1 of 6) ===\n"
    cases = [
        ("all held -> PASS", table(), done_ok, [1, 10], "PASS"),
        ("user insn +10% WORSE only -> PASS, listed", table(**{"c10:server.insn_u_per_sample": up}), done_ok,
         [1, 10], "PASS"),
        ("rate WORSE only -> PASS, listed", table(**{"c1:server.recv_mbps": down}), done_ok, [1, 10], "PASS"),
        ("CPU witness WORSE -> FAIL", table(**{"c1:server.cpu_s_per_Msample": up}), done_ok, [1, 10], "FAIL"),
        ("latency WORSE -> FAIL", table(**{"c10:client.rtt_avg_ms": up}), done_ok, [1, 10], "FAIL"),
        ("CPU better -> PASS", table(**{"c1:server.cpu_s_per_Msample": down}), done_ok, [1, 10], "PASS"),
        ("no P arm on a row -> gate NO VERDICT -> VOID",
         table(**{"noP": "c1:server.cpu_s_per_Msample"}), done_ok, [1, 10], "VOID"),
        ("chain VOID done line -> VOID", table(), "=== chain VOID 2026: block 1 (A) stopped at once - x ===\n",
         [1, 10], "VOID"),
        ("requested cell absent -> VOID", table(), done_ok, [1, 10, 4], "VOID"),
        ("a row the parser cannot read -> k mismatch -> VOID",
         table().replace("WITNESS   held", "WITNESS   held  ?", 1), done_ok, [1, 10], "VOID"),
        ("empty compare -> VOID", "", done_ok, [1, 10], "VOID"),
    ]
    # The floor case, built directly: P 20 away from A makes a +10% WORSE only held(placement-sized).
    wide = (120.0, 0.05, 6)
    rows = [row("c1 T p1 Q0 none", "server.cpu_s_per_Msample", "witness", up, wide),
            row("c1 T p1 Q0 none", "client.insn_u_per_sample", "primary", same)]
    buf = io.StringIO()
    with redirect_stdout(buf):
        gate_v2.report(rows)
    cases.append(("CPU +10% inside a 20% placement floor (held) -> PASS", buf.getvalue(), done_ok, [1], "PASS"))

    # Service cells (S): a network label with spaces, the rtt rule, and the arm files' DEFECT rows.
    def svc_table(**over):
        spec = [("c18 S p1 Q0 none", "client.cpu_s_per_Msample", "witness"),
                ("c18 S p1 Q0 none", "client.rtt_avg_ms", "witness"),
                ("c18 S p1 Q0 none", "server.insn_u_per_sample", "primary"),
                ("c21 S p1 Q0 loss 5% each way", "server.cpu_s_per_Msample", "witness"),
                ("c21 S p1 Q0 loss 5% each way", "client.rtt_avg_ms", "witness"),
                ("c21 S p1 Q0 loss 5% each way", "client.calls_per_s", "witness")]
        rows = []
        for cell, metric, fam in spec:
            r = row(cell, metric, fam, over.get(cell.split()[0] + ":" + metric, same))
            r["hi_better"] = metric.endswith("calls_per_s")
            rows.append(r)
        buf = io.StringIO()
        with redirect_stdout(buf):
            gate_v2.report(rows, title="A a.txt  B b.txt  P p.txt")
        return buf.getvalue()

    arm_ok = ("c21 S p1 Q0 loss 5% each way   tickle     rep1 | ok | role=client sent=9000 rtt_avg_ms=0.04\n"
              "c18 S p1 Q0 none                   tickle     rep1 | ok | role=client sent=9000\n")
    arm_defect = arm_ok + ("c21 S p1 Q0 loss 5% each way   tickle     rep2 | DEFECT(dup_exec:1_first_k:64) | "
                           "role=client sent=9000\n")
    clean = {"A": arm_ok, "P": arm_ok, "B": arm_ok}
    cases += [
        ("service cells, clean arms -> PASS", svc_table(), done_ok, [18, 21], "PASS", clean),
        ("service rtt WORSE -> FAIL", svc_table(**{"c21:client.rtt_avg_ms": up}), done_ok, [18, 21], "FAIL", clean),
        ("service rate WORSE only -> PASS, listed", svc_table(**{"c21:client.calls_per_s": down}), done_ok, [18, 21],
         "PASS", clean),
        ("service DEFECT in B -> FAIL", svc_table(), done_ok, [18, 21], "FAIL", {**clean, "B": arm_defect}),
        ("service DEFECT in B wins over a VOID -> FAIL", svc_table(), done_ok, [18, 21, 4], "FAIL",
         {**clean, "B": arm_defect}),
        ("service DEFECT in A only -> PASS, reported", svc_table(), done_ok, [18, 21], "PASS",
         {**clean, "A": arm_defect}),
        ("service DEFECT in A and B -> FAIL (B's own criterion)", svc_table(), done_ok, [18, 21], "FAIL",
         {**clean, "A": arm_defect, "B": arm_defect}),
        ("service cells without arm files -> VOID", svc_table(), done_ok, [18, 21], "VOID", None),
        ("service cells, arm B file empty -> VOID", svc_table(), done_ok, [18, 21], "VOID", {**clean, "B": ""}),
    ]
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        for name, compare, done, cells, want, *extra in cases:
            cp, dp = os.path.join(d, "c.txt"), os.path.join(d, "d.txt")
            with open(cp, "w", encoding="utf-8") as fh:
                fh.write(compare)
            with open(dp, "w", encoding="utf-8") as fh:
                fh.write(done)
            arms = None
            if extra and extra[0] is not None:
                arms = {}
                for arm, text in extra[0].items():
                    arms[arm] = os.path.join(d, f"arm_{arm}.txt")
                    with open(arms[arm], "w", encoding="utf-8") as fh:
                        fh.write(text)
            got, reason, lines = read(cp, dp, cells, arms)
            ok = got == want
            if want == "PASS" and "listed" in name:
                ok = ok and any(ln.startswith("  OTHER ") for ln in lines) and " 1/" in lines[-1]
            if want == "FAIL" and "DEFECT" in name:
                ok = ok and any(ln.startswith("  DEFECT arm B: c21 rep2 DEFECT(") for ln in lines)
            elif want == "FAIL":
                ok = ok and any(ln.startswith("  BLOCKING ") for ln in lines)
            if "reported" in name:
                ok = ok and any(ln.startswith("  REPORTED the control ran a service callback twice") for ln in lines)
            bad += not ok
            print(f"{'ok  ' if ok else 'BAD '} {name}: {got} ({reason[:110]})")
        missing = read(os.path.join(d, "absent.txt"), os.path.join(d, "absent.done"), [1])[0]
        bad += missing != "VOID"
        print(f"{'ok  ' if missing == 'VOID' else 'BAD '} missing files -> {missing}")
    print(f"SELFTEST {'OK' if not bad else 'FAILED'} ({len(cases) + 1 - bad}/{len(cases) + 1})")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
