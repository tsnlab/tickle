#!/usr/bin/env python3
"""stage1_payg.sh's reading rules, implemented. The rules themselves are in stage1_payg.sh's header.

    stage1_payg.py <out dir>              read every run, apply the VOID rules, print the per-cell and claim verdicts
    stage1_payg.py --residue <tickle.h>   the module's fields left in struct tt_Context when it is compiled out
"""

import math
import os
import re
import subprocess
import sys
import tempfile

REPS_MIN = 5
RSS_FLOOR_KB = 10.0  # WIRE_PLAN 10.4
PCT_FLOOR = 0.01  # WIRE_PLAN 10.4: ~1% between two builds whose code differs in size
TX_SLACK_FRAC = 0.01
TX_SLACK_ABS = 64

# metric -> (role, field, worse_when_higher)
METRICS = {
    "client_cpu_ns": ("client", "sched_cpu_s_per_Msample", True),
    "server_cpu_ns": ("server", "sched_cpu_s_per_Msample", True),
    "send_mbps": ("client", "win_send_mbps", False),
    "client_rss_kb": ("client", "peak_rss_kb", True),
    "server_rss_kb": ("server", "peak_rss_kb", True),
}
# Present when the run had the instruments (PERF=1, RSS_SPLIT=1); absent is not a VOID, present-but-unreadable is.
# Instructions per sample (user + kernel, perf stat) are work, not time: unlike CPU time they do not move with the
# PC's load or the clock, and a function's placement changes its cycles but not its instruction count.
OPTIONAL = {
    "client_instr": ("client", True),
    "server_instr": ("server", True),
    "client_exe_kb": ("client", True),
    "client_stack_kb": ("client", True),
    "client_anon_kb": ("client", True),
    "server_exe_kb": ("server", True),
    "server_stack_kb": ("server", True),
    "server_anon_kb": ("server", True),
}
OFF_ARMS = ("segoff", "alloff", "alloff_a32")


def perf_instructions(path):
    """perf stat -x, output -> (user, kernel) instruction counts, or None when the file is absent."""
    if not os.path.exists(path):
        return None
    got = {}
    for ln in open(path, errors="replace"):
        parts = ln.strip().split(",")
        if len(parts) > 2 and parts[2] in ("instructions:u", "instructions:k"):
            try:
                got[parts[2]] = float(parts[0])
            except ValueError:
                return "unreadable"
    if len(got) != 2:
        return "unreadable"
    return got["instructions:u"], got["instructions:k"]


def rss_split(path):
    try:
        lines = [ln for ln in open(path, errors="replace") if ln.startswith("RSS_SPLIT:")]
    except OSError:
        return None
    return fields(lines[-1]) if lines else None


def fields(line):
    return dict(m.groups() for m in re.finditer(r"(?:^| )([A-Za-z_][A-Za-z0-9_]*)=(\S*)", line))


def result_line(path):
    try:
        with open(path, errors="replace") as f:
            lines = [ln.strip() for ln in f if ln.startswith("RESULT:")]
    except OSError:
        return None
    return fields(lines[0]) if len(lines) == 1 else None


def num(d, k):
    try:
        return float(d[k])
    except (KeyError, TypeError, ValueError):
        return None


def read_run(rd):
    """One run directory -> (meta, values, [void reasons])."""
    meta = fields(open(os.path.join(rd, "job")).read()) if os.path.exists(os.path.join(rd, "job")) else {}
    why = []
    st = {}
    try:
        st = fields(open(os.path.join(rd, "status")).read().strip())
    except OSError:
        why.append("no status")
    if "note" in st:
        why.append("setup: " + st["note"])
    c = result_line(os.path.join(rd, "client.log"))
    s = result_line(os.path.join(rd, "server.log"))
    if c is None:
        why.append("no single client RESULT")
    if s is None:
        why.append("no single server RESULT")
    if why:
        return meta, {}, why
    if st.get("client_rc") != "0":
        why.append("client rc " + st.get("client_rc", "?"))
    if c.get("drained") != "acked":
        why.append("drained=" + str(c.get("drained")))
    if c.get("sent") != s.get("recv") or s.get("lost") != "0":
        why.append("recv %s of sent %s, lost %s" % (s.get("recv"), c.get("sent"), s.get("lost")))
    for role, d in (("client", c), ("server", s)):
        if d.get("window") != "ok":
            why.append("%s window=%s" % (role, d.get("window")))
        if meta.get("topo") == "veth" and d.get("instrument") != "ok":
            why.append("%s instrument=%s" % (role, d.get("instrument")))
    shm = (num(c, "tx_shm") or 0) + (num(c, "rx_shm") or 0) + (num(s, "tx_shm") or 0) + (num(s, "rx_shm") or 0)
    if meta.get("topo") == "veth" and shm != 0:
        why.append("tx/rx_shm=%d: not cross-host shaped" % shm)
    if None in (num(c, "tx_shm"), num(c, "tx_udp"), num(c, "shm_attach_attempts")):
        why.append("client line lacks tx_udp/tx_shm/shm_attach_attempts")
    else:
        attempts = num(c, "shm_attach_attempts") + (num(s, "shm_attach_attempts") or 0)
        if meta.get("arm") in OFF_ARMS and attempts != 0:
            why.append("off arm made %d attach attempts: the flag never arrived" % attempts)
        if meta.get("arm") not in OFF_ARMS and meta.get("topo") == "veth" and attempts == 0:
            why.append("on arm made no attach attempt: its treatment was never applied")
        veth = num(st, "veth_tx_packets")
        tx = num(c, "tx_udp") + num(c, "tx_shm")
        if meta.get("topo") == "veth" and veth is not None and abs(veth - tx) > TX_SLACK_FRAC * tx + TX_SLACK_ABS:
            why.append("veth tx_packets %d vs client tx_udp+tx_shm %d: an uncounted send path" % (veth, tx))
    vals = {}
    for m, (role, f, _) in METRICS.items():
        v = num(c if role == "client" else s, f)
        if v is None:
            why.append("no %s %s" % (role, f))
        else:
            vals[m] = v * (1000.0 if m.endswith("_cpu_ns") else 1.0)  # s per Msample -> ns per sample
    for role, d, n in (("client", c, num(c, "sent")), ("server", s, num(s, "recv"))):
        pi = perf_instructions(os.path.join(rd, role + ".perf"))
        if pi == "unreadable":
            why.append("%s perf output unreadable" % role)
        elif pi is not None and n:
            vals[role + "_instr"] = (pi[0] + pi[1]) / n
            vals[role + "_instr_u"] = pi[0] / n
            vals[role + "_instr_k"] = pi[1] / n
        rs = rss_split(os.path.join(rd, role + ".log"))
        if rs is not None:
            for k in ("exe_kb", "stack_kb", "anon_kb"):
                if num(rs, k) is not None:
                    vals["%s_%s" % (role, k)] = num(rs, k)
    vals["veth_tx"] = num(st, "veth_tx_packets")
    vals["tx_udp"] = num(c, "tx_udp")
    vals["attempts"] = num(c, "shm_attach_attempts")
    vals["sent"] = num(c, "sent")
    return meta, vals, why


def paired(runs, shape, metric, x, ref, rss=False):
    """Mean of per-rep X/REF - 1 (or X - REF for RSS), its SE and n."""
    d = []
    for rep in sorted({r for (sh, a, r) in runs if sh == shape}):
        a, b = runs.get((shape, x, rep)), runs.get((shape, ref, rep))
        if a is None or b is None or metric not in a or metric not in b or b[metric] == 0:
            continue
        d.append(a[metric] - b[metric] if rss else a[metric] / b[metric] - 1.0)
    n = len(d)
    if n < 2:
        return None, None, n
    mean = sum(d) / n
    sd = math.sqrt(sum((v - mean) ** 2 for v in d) / (n - 1))
    return mean, sd / math.sqrt(n), n


def analyse(outd):
    runs = {}
    voids = []
    counts = {}
    pos = None
    for name in sorted(os.listdir(os.path.join(outd, "runs"))):
        rd = os.path.join(outd, "runs", name)
        meta, vals, why = read_run(rd)
        if meta.get("topo") == "samens":
            c = result_line(os.path.join(rd, "client.log")) or {}
            pos = num(c, "tx_shm")
            continue
        key = (meta.get("shape"), meta.get("arm"), meta.get("rep"))
        if why:
            voids.append("%s: %s" % (name, "; ".join(why)))
            continue
        runs[key] = vals
        counts[key[:2]] = counts.get(key[:2], 0) + 1
    shapes = sorted({k[0] for k in runs} | {k[0] for k in counts})
    arms = sorted({k[1] for k in runs})
    print("RUNS valid=%d void=%d" % (len(runs), len(voids)))
    for v in voids:
        print("VOID_RUN " + v)
    overall_void = []
    if not pos:
        overall_void.append("positive control saw tx_shm=%s: the witness cannot see the segment" % pos)
    for sh in shapes:
        for a in arms:
            if counts.get((sh, a), 0) < REPS_MIN:
                overall_void.append("%s %s has %d valid runs (< %d)" % (sh, a, counts.get((sh, a), 0), REPS_MIN))
    print("POSITIVE_CONTROL tx_shm=%s -> %s" % (pos, "ok" if pos else "FAILED"))

    # Means per (shape, arm), for the table.
    print("\nMEANS (ns per sample = sched CPU; Mbps = window send rate; KB = peak RSS)")
    print("%-5s %-11s %4s %10s %10s %10s %9s %9s %9s" %
          ("shape", "arm", "n", "cli_ns", "srv_ns", "Mbps", "cliRSS", "srvRSS", "attempts"))
    for sh in shapes:
        for a in arms:
            rs = [v for (s_, a_, _), v in runs.items() if s_ == sh and a_ == a]
            if not rs:
                continue

            def m(k, rs=rs):
                return sum(r[k] for r in rs) / len(rs)

            print("%-5s %-11s %4d %10.1f %10.1f %10.2f %9.0f %9.0f %9.0f" %
                  (sh, a, len(rs), m("client_cpu_ns"), m("server_cpu_ns"), m("send_mbps"), m("client_rss_kb"),
                   m("server_rss_kb"), m("attempts")))
            if all("client_instr" in r and "server_instr" in r for r in rs):
                print("      instr/sample client %.1f (user %.1f kernel %.1f)  server %.1f (user %.1f kernel %.1f)" %
                      (m("client_instr"), m("client_instr_u"), m("client_instr_k"), m("server_instr"),
                       m("server_instr_u"), m("server_instr_k")))
            if all("client_exe_kb" in r and "server_exe_kb" in r for r in rs):
                print("      RSS at exit, KB: client exe %.0f stack %.0f anon %.0f   server exe %.0f stack %.0f anon %.0f" %
                      (m("client_exe_kb"), m("client_stack_kb"), m("client_anon_kb"), m("server_exe_kb"),
                       m("server_stack_kb"), m("server_anon_kb")))

    comparisons = [("on", "alloff", "UNUSED: on vs alloff"), ("segoff", "alloff", "lending in: segoff vs alloff"),
                   ("on", "on_base", "the fix: on vs on_base"),
                   ("on_a32", "alloff_a32", "aligned twin: on_a32 vs alloff_a32"),
                   ("on_a32", "on", "flag alone on on"), ("alloff_a32", "alloff", "flag alone on alloff")]
    comparisons = [c for c in comparisons if c[0] in arms and c[1] in arms]
    print("\nCELLS (d = mean of per-rep X/REF-1, or X-REF in KB for RSS; floor = max(1%, flag-alone effects))")
    tallies = {}
    for sh in shapes:
        present = {k for r in runs.values() for k in r}
        all_metrics = [(k, v[2]) for k, v in METRICS.items()] + [(k, v[1]) for k, v in OPTIONAL.items() if k in present]
        for metric, worse_high in all_metrics:
            rss = metric.endswith("_kb")
            floor = RSS_FLOOR_KB if rss else PCT_FLOOR
            if not rss:
                for x, ref in (("on_a32", "on"), ("alloff_a32", "alloff")):
                    d, _, _ = paired(runs, sh, metric, x, ref)
                    if d is not None:
                        floor = max(floor, abs(d))
            twin = paired(runs, sh, metric, "on_a32", "alloff_a32", rss)
            for x, ref, label in comparisons:
                d, se, n = paired(runs, sh, metric, x, ref, rss)
                if d is None:
                    continue
                if abs(d) <= 2 * se:
                    v = "HELD"
                elif abs(d) <= floor:
                    v = "FLOOR"
                else:
                    worse = (d > 0) == worse_high
                    v = "COST" if worse else "BETTER"
                    if x == "on" and ref == "alloff":
                        td, tse, _ = twin
                        if td is None or abs(td) <= 2 * tse or (td > 0) != (d > 0):
                            v = "LAYOUT"
                    elif x == "segoff":
                        v = "CANDIDATE_" + v
                unit = "KB" if rss else "%"
                shown = d if rss else 100 * d
                sh_se = 2 * se if rss else 200 * se
                fl = floor if rss else 100 * floor
                print("CELL %-3s %-14s %-36s d=%+8.2f%s 2SE=%6.2f%s floor=%5.2f%s n=%d %s" %
                      (sh, metric, label, shown, unit, sh_se, unit, fl, unit, n, v))
                tallies.setdefault((label, metric), []).append(v)
    print("\nCLAIMS (a cost needs COST in at least half the shapes for one metric - WIRE 8.3)")
    verdicts = {}
    for (label, metric), vs in sorted(tallies.items()):
        if label.startswith("flag alone") or label.startswith("aligned"):
            continue
        costs = sum(1 for v in vs if v.endswith("COST"))
        fired = costs * 2 >= len(vs) and costs > 0
        verdicts.setdefault(label, []).append((metric, costs, len(vs), fired))
    for label, rows in verdicts.items():
        fired = [r for r in rows if r[3]]
        detail = ", ".join("%s %d/%d" % (m, c, n) for m, c, n, _ in rows)
        print("CLAIM %-30s %s  (%s)" % (label, "COST SHOWN" if fired else "no cost shown", detail))
    if overall_void:
        print("\nVERDICT VOID: " + "; ".join(overall_void))
    else:
        print("\nVERDICT valid: every arm and shape has >= %d valid runs and the positive control saw the segment"
              % REPS_MIN)


# ------------------------------------------------------------------------------------------------ residue
RESIDUE_NAME = re.compile(r"(segment|shm|_by_transport|seq_span|span_absorbed|keep_last_delivered|ring_turns|same_host)")


def residue(header):
    """Fields of struct tt_Context whose names belong to the module, declared outside #if tt_SEGMENT_ENABLED."""
    text = open(header).read().splitlines()
    start = next(i for i, ln in enumerate(text) if ln.startswith("struct tt_Context {"))
    depth_guard = []  # stack of booleans: inside a tt_SEGMENT_ENABLED / tt_SAMPLE_LENDING block
    brace = 0
    names = []
    for ln in text[start:]:
        s = ln.strip()
        if s.startswith("#if"):
            depth_guard.append("tt_SEGMENT_ENABLED" in s or "tt_SAMPLE_LENDING" in s)
            continue
        if s.startswith("#endif"):
            depth_guard.pop()
            continue
        if s.startswith("#else") or s.startswith("#elif"):
            continue
        brace += ln.count("{") - ln.count("}")
        if brace == 0 and s.startswith("};"):
            break
        if s.startswith("//") or any(depth_guard) or brace != 1:
            continue  # members of a nested struct are counted through the named member that closes it
        m = re.match(r"\}\s*([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?;", s)
        if m:
            if RESIDUE_NAME.search(m.group(1)):
                names.append(m.group(1))
            continue
        m = re.match(r"[A-Za-z_][\w\s\*]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;", s)
        if m and RESIDUE_NAME.search(m.group(1)):
            names.append(m.group(1))
    inc = os.path.dirname(os.path.dirname(header))
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "r.c")
        with open(src, "w") as f:
            f.write("#include <stdio.h>\n#include <tickle/tickle.h>\nint main(void){struct tt_Context*c=0;(void)c;\n")
            for n in names:
                f.write('printf("RESIDUE_FIELD %s %%zu\\n", sizeof(c->%s));\n' % (n, n))
            f.write("return 0;}\n")
        exe = os.path.join(td, "r")
        subprocess.run(["gcc", "-Dtt_SEGMENT_ENABLED=0", "-Dtt_SAMPLE_LENDING=0", "-I" + inc, src, "-o", exe],
                       check=True)
        out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    total = 0
    for ln in out.splitlines():
        print(ln)
        total += int(ln.split()[-1])
    print("RESIDUE fields=%d bytes=%d (struct tt_Context members named for the module, present with it compiled out;"
          " field bytes, before padding)" % (len(names), total))


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--residue":
        residue(sys.argv[2])
    elif len(sys.argv) == 2:
        analyse(sys.argv[1])
    else:
        sys.exit(__doc__)
