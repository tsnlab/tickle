#!/usr/bin/env python3
"""largemsg_insn_ab.sh's reading rules, implemented (the rules are in the .sh header).

    largemsg_insn_ab.py <out dir> <arm label> ...    first label is the reference A; A2 (if present) is the A/A control
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stage1_payg import fields, num, perf_instructions, result_line  # noqa: E402

PCT_FLOOR = 0.003
RSS_FLOOR_KB = 4.0
METRICS = ("client_instr_u", "client_instr_k", "client_instr", "server_instr_u", "client_rss_kb", "server_rss_kb",
           "client_stack_kb", "client_anon_kb", "client_exe_kb")


def read_run(rd):
    why = []
    meta = fields(open(os.path.join(rd, "job")).read()) if os.path.exists(os.path.join(rd, "job")) else {}
    try:
        st = fields(open(os.path.join(rd, "status")).read().strip())
    except OSError:
        return meta, {}, ["no status"]
    if "note" in st:
        return meta, {}, ["setup: " + st["note"]]
    c = result_line(os.path.join(rd, "client.log"))
    s = result_line(os.path.join(rd, "server.log"))
    if c is None or s is None:
        return meta, {}, ["no single RESULT line"]
    if st.get("client_rc") != "0":
        why.append("client rc " + st.get("client_rc", "?"))
    if c.get("drained") != "acked":
        why.append("drained=" + str(c.get("drained")))
    if c.get("sent") != s.get("recv") or s.get("lost") != "0":
        why.append("recv %s of sent %s lost %s" % (s.get("recv"), c.get("sent"), s.get("lost")))
    for role, d in (("client", c), ("server", s)):
        if d.get("window") != "ok":
            why.append(role + " window=" + str(d.get("window")))
    shm = sum(num(d, k) or 0 for d in (c, s) for k in ("tx_shm", "rx_shm"))
    if shm:
        why.append("tx/rx_shm=%d: not cross-host shaped" % shm)
    vals = {"client_rss_kb": num(c, "peak_rss_kb"), "server_rss_kb": num(s, "peak_rss_kb")}
    for role, n in (("client", num(c, "sent")), ("server", num(s, "recv"))):
        pi = perf_instructions(os.path.join(rd, role + ".perf"))
        if not isinstance(pi, tuple) or not n:
            why.append(role + " no perf counts")
            continue
        vals[role + "_instr_u"] = pi[0] / n
        vals[role + "_instr_k"] = pi[1] / n
        vals[role + "_instr"] = (pi[0] + pi[1]) / n
    try:
        rs = [ln for ln in open(os.path.join(rd, "client.log"), errors="replace") if ln.startswith("RSS_SPLIT:")]
        if rs:
            f = fields(rs[-1])
            for k in ("stack_kb", "anon_kb", "exe_kb"):
                vals["client_" + k] = num(f, k)
    except OSError:
        pass
    return meta, vals, why


def paired(runs, shape, metric, x, ref, absolute):
    ds = []
    for rep, arms in runs.get(shape, {}).items():
        a, b = arms.get(ref, {}).get(metric), arms.get(x, {}).get(metric)
        if a is None or b is None or not a:
            continue
        ds.append(b - a if absolute else b / a - 1.0)
    if len(ds) < 2:
        return None
    m = sum(ds) / len(ds)
    sd = math.sqrt(sum((d - m) ** 2 for d in ds) / (len(ds) - 1))
    return m, sd / math.sqrt(len(ds)), len(ds)


def main():
    outd, labels = sys.argv[1], sys.argv[2:]
    ref = labels[0]
    runs, voids = {}, []
    for name in sorted(os.listdir(os.path.join(outd, "runs"))):
        meta, vals, why = read_run(os.path.join(outd, "runs", name))
        if why:
            voids.append("VOID %s: %s" % (name, "; ".join(why)))
            continue
        runs.setdefault(meta["shape"], {}).setdefault(meta["rep"], {})[meta["arm"]] = vals
    for v in voids:
        print(v)
    print("MEANS (per sample; kB)")
    for shape in sorted(runs):
        for arm in labels:
            row = []
            for m in METRICS:
                xs = [r[arm][m] for r in runs[shape].values() if arm in r and r[arm].get(m) is not None]
                row.append("%s=%.1f" % (m, sum(xs) / len(xs)) if xs else m + "=-")
            print("  %s %-5s n=%d %s" % (shape, arm, sum(1 for r in runs[shape].values() if arm in r), " ".join(row)))
    print("COMPARISONS vs %s (instructions: relative; RSS: kB)" % ref)
    for shape in sorted(runs):
        for m in METRICS:
            absolute = m.endswith("_kb")
            aa = paired(runs, shape, m, "A2", ref, absolute) if "A2" in labels else None
            floor = RSS_FLOOR_KB if absolute else PCT_FLOOR
            if aa is not None:
                floor = max(floor, abs(aa[0]) + 2 * aa[1])
            for x in labels[1:]:
                p = paired(runs, shape, m, x, ref, absolute)
                if p is None:
                    print("  %s %-16s %-5s n<2" % (shape, m, x))
                    continue
                d, se, n = p
                verdict = "DIFFERS" if abs(d) > 2 * se and abs(d) > floor else "SAME"
                fmt = "%+8.2f kB (se %.2f)" if absolute else "%+7.3f%% (se %.3f)"
                scale = 1 if absolute else 100
                print("  %s %-16s %-5s %s n=%d floor=%s  %s" % (
                    shape, m, x, fmt % (d * scale, se * scale), n,
                    ("%.2f kB" % floor) if absolute else ("%.3f%%" % (floor * 100)), verdict))
    # Old pair, its own reference.
    if "O" in labels and "OB" in labels:
        print("CONTROL PAIR OB vs O")
        for shape in sorted(runs):
            for m in ("client_instr_u", "client_instr", "client_rss_kb"):
                p = paired(runs, shape, m, "OB", "O", m.endswith("_kb"))
                if p:
                    s = 1 if m.endswith("_kb") else 100
                    print("  %s %-16s %+8.3f (se %.3f) n=%d" % (shape, m, p[0] * s, p[1] * s, p[2]))


main()
