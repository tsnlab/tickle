#!/usr/bin/env python3
"""hybrid_step_cost.sh's reading rules, implemented. The rules themselves are in hybrid_step_cost.sh's header.

    hybrid_step_cost.py <out dir> "<arm> <arm> ..."     the first arm is the reference, the second its A/A twin
"""

import math
import os
import re
import sys

REPS_MIN = 5
PCT_FLOOR = 0.01  # WIRE_PLAN 10.4
TX_SLACK_FRAC = 0.01
TX_SLACK_ABS = 64


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


def perf_instructions(path):
    """perf stat -x, output -> (user, kernel), or a string saying why not."""
    if not os.path.exists(path):
        return "missing"
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


def read_run(rd):
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
    shm = sum(num(d, k) or 0 for d in (c, s) for k in ("tx_shm", "rx_shm"))
    if shm != 0:
        why.append("tx/rx_shm=%d: not cross-host shaped" % shm)
    veth, tx = num(st, "veth_tx_packets"), num(c, "tx_udp")
    if veth is None or tx is None:
        why.append("no veth tx_packets or client tx_udp")
    elif abs(veth - tx) > TX_SLACK_FRAC * tx + TX_SLACK_ABS:
        why.append("veth tx_packets %d vs client tx_udp %d: an uncounted send path" % (veth, tx))
    vals = {}
    for role, n in (("client", num(c, "sent")), ("server", num(s, "recv"))):
        pi = perf_instructions(os.path.join(rd, role + ".perf"))
        if isinstance(pi, str):
            why.append("%s perf %s" % (role, pi))
        elif not n:
            why.append("%s counted no samples" % role)
        else:
            vals[role] = (pi[0] + pi[1]) / n
            vals[role + "_u"] = pi[0] / n
            vals[role + "_k"] = pi[1] / n
    vals["sent"] = num(c, "sent")
    return meta, vals, why


def paired(runs, shape, metric, x, ref):
    d = []
    for rep in sorted({r for (sh, _a, r) in runs if sh == shape}):
        a, b = runs.get((shape, x, rep)), runs.get((shape, ref, rep))
        if a is None or b is None or metric not in a or metric not in b or b[metric] == 0:
            continue
        d.append(a[metric] / b[metric] - 1.0)
    n = len(d)
    if n < 2:
        return None, None, n
    mean = sum(d) / n
    sd = math.sqrt(sum((v - mean) ** 2 for v in d) / (n - 1))
    return mean, sd / math.sqrt(n), n


def analyse(outd, arms):
    ref, twin = arms[0], arms[1]
    runs, voids, counts = {}, [], {}
    for name in sorted(os.listdir(os.path.join(outd, "runs"))):
        meta, vals, why = read_run(os.path.join(outd, "runs", name))
        key = (meta.get("shape"), meta.get("arm"), meta.get("rep"))
        if why:
            voids.append("%s: %s" % (name, "; ".join(why)))
            continue
        runs[key] = vals
        counts[key[:2]] = counts.get(key[:2], 0) + 1
    shapes = sorted({k[0] for k in runs} | {k[0] for k in counts})
    print("RUNS valid=%d void=%d" % (len(runs), len(voids)))
    for v in voids:
        print("VOID_RUN " + v)
    overall_void = []
    for sh in shapes:
        for a in arms:
            if counts.get((sh, a), 0) < REPS_MIN:
                overall_void.append("%s %s has %d valid runs (< %d)" % (sh, a, counts.get((sh, a), 0), REPS_MIN))
    # Identity: same commit -> same client bytes; different commits -> different client bytes.
    static = {}
    try:
        for ln in open(os.path.join(outd, "static.txt")):
            f = fields(ln)
            static[(f.get("arm"), f.get("shape"))] = (f.get("sha"), f.get("client_sha"))
    except OSError:
        overall_void.append("no static.txt")
    for sh in shapes:
        for i, a in enumerate(arms):
            for b in arms[i + 1:]:
                sa, sb = static.get((a, sh)), static.get((b, sh))
                if sa is None or sb is None:
                    continue
                if sa[0] == sb[0] and sa[1] != sb[1]:
                    overall_void.append("%s: %s and %s are one commit with different clients" % (sh, a, b))
                if sa[0] != sb[0] and sa[1] == sb[1]:
                    overall_void.append("%s: %s and %s are different commits with one client" % (sh, a, b))

    print("\nMEANS (instructions per sample, user + kernel; client per sent, server per received)")
    print("%-5s %-10s %3s %10s %10s %10s %10s %10s %10s" %
          ("shape", "arm", "n", "client", "cli_user", "cli_kern", "server", "srv_user", "srv_kern"))
    for sh in shapes:
        for a in arms:
            rs = [v for (s_, a_, _), v in runs.items() if s_ == sh and a_ == a]
            if not rs:
                continue

            def m(k, rs=rs):
                return sum(r[k] for r in rs) / len(rs)

            print("%-5s %-10s %3d %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f" %
                  (sh, a, len(rs), m("client"), m("client_u"), m("client_k"), m("server"), m("server_u"),
                   m("server_k")))

    print("\nCELLS (d = mean of per-rep X/%s - 1; floor = max(1%%, |d(%s)| + 2 SE(%s)))" % (ref, twin, twin))
    tallies = {}
    for sh in shapes:
        for metric in ("client", "server"):
            floor = PCT_FLOOR
            aa_d, aa_se, _ = paired(runs, sh, metric, twin, ref)
            if aa_d is not None:
                floor = max(floor, abs(aa_d) + 2 * aa_se)
            for x in arms[1:]:
                d, se, n = paired(runs, sh, metric, x, ref)
                if d is None:
                    continue
                if abs(d) <= 2 * se:
                    v = "HELD"
                elif abs(d) <= floor or x == twin:
                    v = "FLOOR"
                else:
                    v = "COST" if d > 0 else "BETTER"
                print("CELL %-3s %-7s %-10s d=%+7.3f%% 2SE=%6.3f%% floor=%5.2f%% n=%d %s" %
                      (sh, metric, x, 100 * d, 200 * se, 100 * floor, n, v))
                tallies.setdefault((x, metric), []).append(v)
    print("\nCLAIMS (an arm costs iff COST in at least half the shapes for one process)")
    for x in arms[2:]:
        rows = [(m, tallies.get((x, m), [])) for m in ("client", "server")]
        fired = [m for m, vs in rows if vs and sum(v == "COST" for v in vs) * 2 >= len(vs)]
        detail = ", ".join("%s COST %d/%d" % (m, sum(v == "COST" for v in vs), len(vs)) for m, vs in rows)
        print("CLAIM %-10s %s  (%s)" % (x, "COST SHOWN (%s)" % "/".join(fired) if fired else "no cost shown", detail))
    if overall_void:
        print("\nVERDICT VOID: " + "; ".join(overall_void))
    else:
        print("\nVERDICT valid: every arm and shape has >= %d valid runs; arm identity as expected" % REPS_MIN)


if __name__ == "__main__":
    if len(sys.argv) != 3 or len(sys.argv[2].split()) < 2:
        sys.exit(__doc__)
    analyse(sys.argv[1], sys.argv[2].split())
