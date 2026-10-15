#!/usr/bin/env python3
"""nosend_benefit_read.py - the reading of nosend_benefit_rig.sh (2026-10-15), pre-registered here, before any run.

A = the control (a writer with no known reader broadcasts every sample), B = the change (faa178a3: it sends nothing).

PER-RUN VOID (the run is listed and left out; a cell with a void run is VOID):
  - a role's RESULT line is missing, or its BUILD line names another tree than the arm's commit;
  - a role's /proc/PID/exe sha256 is not the sha256 of the binary built for that host, arm and variant (builds.txt);
  - the treatment: B's writer must report every publish in core's traffic line as tx_unmatched_dropped or
    tx_unmatched_kept (U: their sum >= published; F: > 0, it published before its reader matched); A's writer must
    report no tx_unmatched field at all, or 0;
  - U: a publishing cell whose writer published fewer than 95% of rate x U_DUR.

CELL U (Ua p1 BEST_EFFORT 1 kHz, Ub p1 RELIABLE 1 kHz, Uc 1 MiB RELIABLE 30 Hz; U0 the idle-Publisher control):
  data packets of a run = its eth0 packet delta (writer tx, Pi B rx) minus the same arm's U0 median packet rate x the
  run's window (floored at 0); without U0 in the run, the raw delta (said so).
  PREMISE (else VOID): A's writer put data packets on the wire for at least 95% of its published samples (median;
  95%, not 100%, set after the first PC preflight and before any rig run: net of the background, 4996 of 5000).
  CONTROL (else VOID): U0's Pi B rx packets, B against A medians, differ by at most max(50, 25% of A).
  CPU WORSE: B's median writer CPU per sample > 1.05 x A's median AND min(B) > max(A).
  PASS (B improved): B's median writer-tx data packets < 1% of A's AND B's median Pi B rx data packets < 1% of A's
  AND writer CPU per sample not WORSE. Otherwise FAIL, with the failing clause named.
  U VERDICT: PASS if every requested publishing cell PASSes; VOID if any is VOID; FAIL otherwise.
  Reported, not deciding: wire bytes, Pi B Ip InReceives / ReasmReqds, Udp InDatagrams / RcvbufErrors / NoPorts,
  Pi B softirq jiffies and NET_RX softirq count, the observer's CPU and rx_datagrams.

CELL F (Fa, Fb, Fc as Ua, Ub, Uc) per loss: Subscriber creation -> first sample, n / median / p90 / max per arm, and
  B - A median with a bootstrap 95% CI (10000 resamples, seed 1). The discovery control: context creation -> first
  announce of another context, the same statistics.
  VOID: any A run that never received within F_TIMEOUT (the setup failed, not the change); at 0% loss, the discovery
  control's median moving by more than 5 ms B against A; fewer than 3 valid runs in an arm.
  REGRESSION: at 0% loss B's median - A's median > 5 ms; at any loss, any B run that never received.
  F VERDICT: VOID if any row is VOID; REGRESSION if any row is; NO REGRESSION otherwise.

Exit status: 0 U PASS (or no U cell) and F NO REGRESSION (or no F cell); 2 U FAIL or F REGRESSION; 3 any VOID.
--selftest runs the rules on synthetic runs, including ones that must FAIL, REGRESS and VOID.
"""

import os
import random
import re
import statistics
import sys

PUBLISHING = {"Ua": 1000.0, "Ub": 1000.0, "Uc": 30.0}
PCT_THRESHOLD = 0.01
CPU_WORSE_RATIO = 1.05
CONTROL_PKTS_ABS = 50
CONTROL_PKTS_REL = 0.25
F_REGRESS_MS = 5.0
F_CONTROL_MS = 5.0
F_MIN_RUNS = 3
PUBLISHED_FRACTION = 0.95


def kv(line):
    return {k: v for k, v in re.findall(r"(\w+)=(\S+)", line)}


def first_line(text, prefix):
    for line in text.splitlines():
        if line.startswith(prefix):
            return line
    return None


def traffic_unmatched(text):
    """(dropped, kept) from core's traffic line, None for each field the line does not carry; None if no line."""
    line = None
    for l in text.splitlines():
        if " traffic: " in l:
            line = l
    if line is None:
        return None
    d = kv(line)
    get = lambda k: int(d[k]) if k in d else None
    return get("tx_unmatched_dropped"), get("tx_unmatched_kept")


def pct(values, q):
    s = sorted(values)
    if not s:
        return float("nan")
    i = min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))
    return s[i]


def bootstrap_ci(a, b, n=10000, seed=1):
    rng = random.Random(seed)
    ds = []
    for _ in range(n):
        ra = [rng.choice(a) for _ in a]
        rb = [rng.choice(b) for _ in b]
        ds.append(statistics.median(rb) - statistics.median(ra))
    ds.sort()
    return ds[int(0.025 * n)], ds[int(0.975 * n) - 1]


class Data:
    """What a run directory holds, through one reading function, so the self-test drives the same code."""

    def __init__(self, meta, builds, runs, files):
        self.meta, self.builds, self.runs, self.files = meta, builds, runs, files

    @staticmethod
    def load(out):
        def read(p):
            try:
                with open(p, errors="replace") as f:
                    return f.read()
            except OSError:
                return None

        meta = dict(l.split("=", 1) for l in (read(os.path.join(out, "meta.txt")) or "").splitlines() if "=" in l)
        builds = {}
        for l in (read(os.path.join(out, "builds.txt")) or "").splitlines():
            d = kv(l)
            if {"host", "arm_sha", "variant", "sha256"} <= d.keys():
                builds[(d["host"], d["arm_sha"], d["variant"])] = d["sha256"]
        runs = [kv(l) for l in (read(os.path.join(out, "index.txt")) or "").splitlines() if l.startswith("RUN ")]
        files = {}
        rd = os.path.join(out, "runs")
        for r in runs:
            for suffix in ("pub.log", "obs.log", "sub.log", "pub.exe", "obs.exe", "sub.exe", "wk_pre.txt",
                           "wk_post.txt", "rk_pre.txt", "rk_post.txt"):
                t = read(os.path.join(rd, f"{r['stem']}_{suffix}"))
                if t is not None:
                    files[(r["stem"], suffix)] = t
        return Data(meta, builds, runs, files)


def check_role(data, run, role, host_key, why):
    """Returns the role's RESULT dict, or None (and appends the reason) when the role cannot be read as the arm's."""
    log = data.files.get((run["stem"], f"{role}.log"))
    if log is None:
        why.append(f"{role}: no log")
        return None
    res = first_line(log, "RESULT:")
    build = first_line(log, "BUILD:")
    if res is None:
        why.append(f"{role}: no RESULT line")
        return None
    if build is None or kv(build).get("tree") != run["sha"]:
        why.append(f"{role}: BUILD names {kv(build or '').get('tree')}, not {run['sha'][:12]}")
        return None
    exe = (data.files.get((run["stem"], f"{role}.exe")) or "").strip()
    want = data.builds.get((run[host_key], run["sha"], run["variant"]))
    if want is None or exe != want:
        why.append(f"{role}: /proc/PID/exe sha256 {exe[:12] or '(none)'} is not the build's {str(want)[:12]}")
        return None
    return kv(res)


def check_treatment(data, run, published, why, kind):
    t = traffic_unmatched(data.files.get((run["stem"], "pub.log"), ""))
    if t is None:
        why.append("pub: no traffic line (could not look)")
        return
    dropped, kept = t
    total = (dropped or 0) + (kept or 0)
    if run["arm"] == "A":
        if total != 0:
            why.append(f"treatment: A's writer reports tx_unmatched={total}")
    elif dropped is None and kept is None:
        why.append("treatment: B's writer has no tx_unmatched fields (not the change's core)")
    elif kind == "U" and total < published:
        why.append(f"treatment: B's writer counted {total} unmatched of {published} published")
    elif kind == "F" and total == 0:
        why.append("treatment: B's writer counted no unmatched publish before its reader matched")


def ksnap_delta(data, stem, side):
    pre = kv(data.files.get((stem, f"{side}_pre.txt"), ""))
    post = kv(data.files.get((stem, f"{side}_post.txt"), ""))
    out = {}
    for k, v in post.items():
        if k in pre:
            try:
                out[k] = int(post[k]) - int(pre[k])
            except ValueError:
                pass
    return out


def med(xs):
    return statistics.median(xs) if xs else float("nan")


def read_u(data, lines):
    u_dur = float(data.meta.get("U_DUR", "60"))
    rows = {}
    voids = []
    for run in data.runs:
        if run.get("kind") != "U":
            continue
        why = [] if run.get("leftover", "0") == "0" else [f"{run['leftover']} bench process(es) left over at its end"]
        pub = check_role(data, run, "pub", "writer", why)
        obs = check_role(data, run, "obs", "reader", why)
        published = int(pub.get("published", "0")) if pub else 0
        cell = run["cell"]
        if pub is not None:
            if cell in PUBLISHING and published < PUBLISHED_FRACTION * PUBLISHING[cell] * u_dur:
                why.append(f"published {published} < 95% of {PUBLISHING[cell]:g} Hz x {u_dur:g} s")
            check_treatment(data, run, published, why, "U")
        w = ksnap_delta(data, run["stem"], "wk")
        r = ksnap_delta(data, run["stem"], "rk")
        for need, d in (("if_tx_packets", w), ("if_rx_packets", r), ("t_ns", w)):
            if need not in d:
                why.append(f"no kernel counter {need}")
        if why:
            voids.append(f"{run['stem']}: " + "; ".join(why))
            rows.setdefault((cell, run["arm"]), []).append(None)
            continue
        rows.setdefault((cell, run["arm"]), []).append(
            {
                "published": published,
                "cpu": float(pub["cpu_ns_per_sample"]),
                "w_pkts": w["if_tx_packets"],
                "w_bytes": w.get("if_tx_bytes", 0),
                "r_pkts": r["if_rx_packets"],
                "r_bytes": r.get("if_rx_bytes", 0),
                "secs": w["t_ns"] / 1e9,
                "r_softirq": r.get("cpu_softirq", 0),
                "r_netrx": r.get("softirqs_NET_RX", 0),
                "r_ip_in": r.get("Ip_InReceives", 0),
                "r_reasm": r.get("Ip_ReasmReqds", 0),
                "r_udp_in": r.get("Udp_InDatagrams", 0),
                "r_rcvbuf": r.get("Udp_RcvbufErrors", 0),
                "r_noports": r.get("Udp_NoPorts", 0),
                "obs_cpu_ms": (int(obs["cpu_user_ns"]) + int(obs["cpu_sys_ns"])) / 1e6,
                "obs_rx": int(obs["rx_datagrams"]),
            }
        )
    cells = sorted({c for c, _ in rows})
    if not cells:
        return None
    lines.append("== cell U (unwatched topic) ==")
    for v in voids:
        lines.append(f"  VOID RUN {v}")

    def ok(cell, arm):
        return [x for x in rows.get((cell, arm), []) if x is not None]

    def has_void(cell):
        return any(x is None for arm in "AB" for x in rows.get((cell, arm), []))

    # The control: the link's background per second, per arm.
    bg = {}
    control_void = None
    if "U0" in cells:
        for arm in "AB":
            runs = ok("U0", arm)
            if runs:
                bg[arm] = (med([x["w_pkts"] / x["secs"] for x in runs]), med([x["r_pkts"] / x["secs"] for x in runs]))
        a, b = ok("U0", "A"), ok("U0", "B")
        if has_void("U0") or not a or not b:
            control_void = "the control U0 has a void run or an empty arm"
        else:
            ma, mb = med([x["r_pkts"] for x in a]), med([x["r_pkts"] for x in b])
            if abs(mb - ma) > max(CONTROL_PKTS_ABS, CONTROL_PKTS_REL * ma):
                control_void = f"the control U0 differs: Pi B rx packets A {ma:g} B {mb:g}"
            lines.append(
                f"  U0 control: Pi B rx packets median A {ma:g} B {mb:g}; background/s writer tx A {bg['A'][0]:.1f} "
                f"B {bg['B'][0]:.1f}, Pi B rx A {bg['A'][1]:.1f} B {bg['B'][1]:.1f}"
                + (f" - VOID: {control_void}" if control_void else " - ok")
            )
    else:
        lines.append("  no U0 in this run: data packets are the raw eth0 deltas (background included)")

    def net(x, arm, idx, key):
        base = bg.get(arm, (0.0, 0.0))[idx] * x["secs"]
        return max(0.0, x[key] - base)

    verdicts = {}
    for cell in [c for c in cells if c in PUBLISHING]:
        a, b = ok(cell, "A"), ok(cell, "B")
        why_void = []
        if has_void(cell):
            why_void.append("a void run")
        if not a or not b:
            why_void.append("an arm with no valid run")
        if control_void:
            why_void.append(control_void)
        if a and b:
            aw = [net(x, "A", 0, "w_pkts") for x in a]
            bw = [net(x, "B", 0, "w_pkts") for x in b]
            ar = [net(x, "A", 1, "r_pkts") for x in a]
            br = [net(x, "B", 1, "r_pkts") for x in b]
            apub = med([x["published"] for x in a])
            if med(aw) < PUBLISHED_FRACTION * apub:
                why_void.append(f"premise: A's writer put {med(aw):g} data packets on the wire for {apub:g} samples")
            ac, bc = [x["cpu"] for x in a], [x["cpu"] for x in b]
            cpu_worse = med(bc) > CPU_WORSE_RATIO * med(ac) and min(bc) > max(ac)
            fails = []
            if not med(bw) < PCT_THRESHOLD * med(aw):
                fails.append(f"writer tx data packets B {med(bw):g} not < 1% of A {med(aw):g}")
            if not med(br) < PCT_THRESHOLD * med(ar):
                fails.append(f"Pi B rx data packets B {med(br):g} not < 1% of A {med(ar):g}")
            if cpu_worse:
                fails.append(f"writer CPU/sample WORSE: B {med(bc):.0f} ns vs A {med(ac):.0f} ns")
            lines.append(f"  {cell} (n A={len(a)} B={len(b)}):")
            lines.append(
                f"    writer eth0 tx data packets  A {med(aw):12.0f}  B {med(bw):12.0f}  B/A {med(bw) / med(aw) if med(aw) else float('nan'):.5f}"
            )
            lines.append(
                f"    Pi B eth0 rx data packets    A {med(ar):12.0f}  B {med(br):12.0f}  B/A {med(br) / med(ar) if med(ar) else float('nan'):.5f}"
            )
            for key, name in (("w_bytes", "writer eth0 tx bytes"), ("r_bytes", "Pi B eth0 rx bytes"),
                              ("r_ip_in", "Pi B Ip InReceives"), ("r_reasm", "Pi B Ip ReasmReqds"),
                              ("r_udp_in", "Pi B Udp InDatagrams"), ("r_rcvbuf", "Pi B Udp RcvbufErrors"),
                              ("r_noports", "Pi B Udp NoPorts"), ("r_softirq", "Pi B softirq jiffies"),
                              ("r_netrx", "Pi B NET_RX softirqs"), ("obs_rx", "observer rx_datagrams"),
                              ("obs_cpu_ms", "observer CPU ms")):
                lines.append(f"    {name:<28} A {med([x[key] for x in a]):12.1f}  B {med([x[key] for x in b]):12.1f}")
            lines.append(
                f"    writer CPU ns/sample         A {med(ac):12.1f}  B {med(bc):12.1f}  (A {min(ac):.0f}..{max(ac):.0f}, "
                f"B {min(bc):.0f}..{max(bc):.0f}){' WORSE' if cpu_worse else ''}"
            )
            lines.append(f"    published (median)           A {apub:12.0f}  B {med([x['published'] for x in b]):12.0f}")
        else:
            fails = []
        if why_void:
            verdicts[cell] = "VOID"
            lines.append(f"  {cell}: VOID - {'; '.join(why_void)}")
        elif fails:
            verdicts[cell] = "FAIL"
            lines.append(f"  {cell}: FAIL - {'; '.join(fails)}")
        else:
            verdicts[cell] = "PASS"
            lines.append(f"  {cell}: PASS (B improved: <1% of A's data packets on the wire and at Pi B; CPU not WORSE)")
    if not verdicts:
        lines.append("U VERDICT: VOID (no publishing U cell read)")
        return "VOID"
    v = "VOID" if "VOID" in verdicts.values() else "FAIL" if "FAIL" in verdicts.values() else "PASS"
    lines.append(f"U VERDICT: {v} ({', '.join(f'{c} {x}' for c, x in sorted(verdicts.items()))})")
    return v


def read_f(data, lines):
    rows = {}
    voids = []
    for run in data.runs:
        if run.get("kind") != "F":
            continue
        why = [] if run.get("leftover", "0") == "0" else [f"{run['leftover']} bench process(es) left over at its end"]
        pub = check_role(data, run, "pub", "writer", why)
        sub = check_role(data, run, "sub", "reader", why)
        if pub is not None:
            check_treatment(data, run, int(pub.get("published", "0")), why, "F")
        key = (run["cell"], int(run["loss"]), run["arm"])
        if why:
            voids.append(f"{run['stem']}: " + "; ".join(why))
            rows.setdefault(key, []).append(None)
            continue
        rows.setdefault(key, []).append(
            {"first": float(sub["sub_to_first_ms"]), "announce": float(sub["ctx_to_announce_ms"])}
        )
    if not rows:
        return None
    lines.append("== cell F (time to first sample; reader's own monotonic clock) ==")
    for v in voids:
        lines.append(f"  VOID RUN {v}")
    verdicts = {}
    for cell, loss in sorted({(c, l) for c, l, _ in rows}):
        name = f"{cell} loss {loss}%"
        allr = {arm: rows.get((cell, loss, arm), []) for arm in "AB"}
        ok = {arm: [x for x in allr[arm] if x is not None] for arm in "AB"}
        why_void, regress = [], []
        if any(x is None for arm in "AB" for x in allr[arm]):
            why_void.append("a void run")
        if len(ok["A"]) < F_MIN_RUNS or len(ok["B"]) < F_MIN_RUNS:
            why_void.append(f"fewer than {F_MIN_RUNS} valid runs in an arm")
        never = {arm: sum(1 for x in ok[arm] if x["first"] < 0) for arm in "AB"}
        if never["A"]:
            why_void.append(f"{never['A']} A run(s) never received (the setup, not the change)")
        if never["B"]:
            regress.append(f"{never['B']} B run(s) never received within the timeout")
        lines.append(f"  {name} (n A={len(ok['A'])} B={len(ok['B'])}):")
        got = {arm: [x["first"] for x in ok[arm] if x["first"] >= 0] for arm in "AB"}
        ann = {arm: [x["announce"] for x in ok[arm] if x["announce"] >= 0] for arm in "AB"}
        for arm in "AB":
            g = got[arm]
            lines.append(
                f"    {arm} sub->first ms  median {med(g):9.3f}  p90 {pct(g, 0.9):9.3f}  max {max(g) if g else float('nan'):9.3f}"
                f"  never {never[arm]}   | ctx->announce ms median {med(ann[arm]):9.3f} p90 {pct(ann[arm], 0.9):9.3f}"
            )
        if got["A"] and got["B"]:
            d = med(got["B"]) - med(got["A"])
            lo, hi = bootstrap_ci(got["A"], got["B"])
            lines.append(f"    B-A median sub->first {d:+.3f} ms, 95% CI [{lo:+.3f}, {hi:+.3f}]")
            if loss == 0 and d > F_REGRESS_MS:
                regress.append(f"median delay +{d:.3f} ms > {F_REGRESS_MS} ms at 0% loss")
        if ann["A"] and ann["B"]:
            da = med(ann["B"]) - med(ann["A"])
            lo, hi = bootstrap_ci(ann["A"], ann["B"])
            lines.append(f"    B-A median ctx->announce (control) {da:+.3f} ms, 95% CI [{lo:+.3f}, {hi:+.3f}]")
            if loss == 0 and abs(da) > F_CONTROL_MS:
                why_void.append(f"discovery control moved {da:+.3f} ms at 0% loss")
        elif not why_void:
            why_void.append("no announce time in an arm (the control could not be read)")
        if why_void:
            verdicts[name] = "VOID"
            lines.append(f"  {name}: VOID - {'; '.join(why_void)}")
        elif regress:
            verdicts[name] = "REGRESSION"
            lines.append(f"  {name}: REGRESSION - {'; '.join(regress)}")
        else:
            verdicts[name] = "NO REGRESSION"
            lines.append(f"  {name}: NO REGRESSION")
    v = ("VOID" if "VOID" in verdicts.values() else "REGRESSION" if "REGRESSION" in verdicts.values()
         else "NO REGRESSION")
    lines.append(f"F VERDICT: {v}")
    return v


def read_all(data):
    lines = [f"nosend benefit reading: A={data.meta.get('A', '?')[:12]} B={data.meta.get('B', '?')[:12]} "
             f"harness={data.meta.get('HARNESS', '?')[:12]} pc_preflight={data.meta.get('PC_PREFLIGHT', '?')}"]
    if data.meta.get("PC_PREFLIGHT") == "1":
        lines.append("  PC PREFLIGHT: two namespaces on one PC, a veth, no wire - a harness check, never a rig figure")
    u = read_u(data, lines)
    f = read_f(data, lines)
    if u is None and f is None:
        lines.append("NOSEND BENEFIT VERDICT: VOID (no run read)")
        return lines, 3
    rc = 0
    if "VOID" in (u, f):
        rc = 3
    elif u == "FAIL" or f == "REGRESSION":
        rc = 2
    lines.append(f"NOSEND BENEFIT VERDICT: U {u or 'not run'}; F {f or 'not run'}")
    return lines, rc


# ---- self-test -------------------------------------------------------------------------------------------------------
def synth(u_b_wire=5, f_b_never=False, f_b_ms=1.2, b_treat=True, exe_ok=True, a_never=False, ctl_b=None):
    """A run directory in memory: A broadcasts, B (per the arguments) does not."""
    A, B = "a" * 40, "b" * 40
    meta = {"A": A, "B": B, "HARNESS": "c" * 40, "PC_PREFLIGHT": "0", "U_DUR": "60"}
    builds = {}
    for host in ("w", "r"):
        for sha in (A, B):
            for v in ("small", "large"):
                builds[(host, sha, v)] = (sha[0] + v[0]) * 32
    runs, files = [], {}

    def add(kind, cell, arm, rep, loss, v, sub_ms=None, published=60000, wire=None, rx=None, cpu=4000.0):
        sha = A if arm == "A" else B
        stem = f"{cell}_l{loss}_{arm}_r{rep}"
        runs.append({"kind": kind, "cell": cell, "arm": arm, "sha": sha, "rep": str(rep), "loss": str(loss),
                     "variant": v, "stem": stem, "writer": "w", "reader": "r"})
        traffic = "Node 1 traffic: tx_datagrams=1"
        if arm == "B" and b_treat:
            traffic += f" tx_unmatched_dropped={published if kind == 'U' else 3000} tx_unmatched_kept=0"
        files[(stem, "pub.log")] = (f"BUILD: tree={sha} sample_bytes=76 large=0\nRESULT: role=pub published={published} "
                                    f"cpu_ns_per_sample={cpu} cpu_user_ns=1 cpu_sys_ns=1\n[INFO] {traffic}\n")
        good = (sha[0] + v[0]) * 32
        files[(stem, "pub.exe")] = good if exe_ok else "0" * 64
        if kind == "U":
            files[(stem, "obs.log")] = (f"BUILD: tree={sha}\nRESULT: role=observe cpu_user_ns=1000000 "
                                        f"cpu_sys_ns=1000000 rx_datagrams={rx}\n")
            files[(stem, "obs.exe")] = good
            files[(stem, "wk_pre.txt")] = "t_ns=0 if_tx_packets=0 if_tx_bytes=0 if_rx_packets=0 if_rx_bytes=0"
            files[(stem, "wk_post.txt")] = f"t_ns=62000000000 if_tx_packets={wire} if_tx_bytes={wire * 120} if_rx_packets=0 if_rx_bytes=0"
            files[(stem, "rk_pre.txt")] = "t_ns=0 if_rx_packets=0 if_rx_bytes=0 if_tx_packets=0 cpu_softirq=0"
            files[(stem, "rk_post.txt")] = f"t_ns=62000000000 if_rx_packets={rx} if_rx_bytes={rx * 120} if_tx_packets=0 cpu_softirq=5"
        else:
            files[(stem, "sub.log")] = (f"BUILD: tree={sha}\nRESULT: role=sub ctx_to_announce_ms=2.0 "
                                        f"sub_to_first_ms={sub_ms} delivered=10\n")
            files[(stem, "sub.exe")] = good

    for rep in (1, 2, 3):
        add("U", "U0", "A", rep, 0, "small", published=0, wire=120, rx=120)
        add("U", "U0", "B", rep, 0, "small", published=0, wire=ctl_b or 120, rx=ctl_b or 120)
        add("U", "Ua", "A", rep, 0, "small", wire=60120, rx=60120, cpu=4000.0 + rep)
        add("U", "Ua", "B", rep, 0, "small", wire=120 + u_b_wire, rx=120 + u_b_wire, cpu=1500.0 + rep)
    for rep in range(1, 6):
        add("F", "Fa", "A", rep, 0, "small", sub_ms=-1 if (a_never and rep == 1) else 0.5 + rep * 0.01)
        add("F", "Fa", "B", rep, 0, "small", sub_ms=-1 if (f_b_never and rep == 2) else f_b_ms + rep * 0.01)
    return Data(meta, builds, runs, files)


def selftest():
    cases = [
        ("the expected outcome", synth(), 0, ["U VERDICT: PASS", "F VERDICT: NO REGRESSION"]),
        ("B still broadcasting", synth(u_b_wire=60000), 2, ["Ua: FAIL", "U VERDICT: FAIL"]),
        ("B never receives once", synth(f_b_never=True), 2, ["F VERDICT: REGRESSION"]),
        ("B 8 ms slower at 0%", synth(f_b_ms=8.6), 2, ["median delay +8."]),
        ("B without the treatment", synth(b_treat=False), 3, ["no tx_unmatched fields", "U VERDICT: VOID"]),
        ("a foreign binary", synth(exe_ok=False), 3, ["is not the build's"]),
        ("A never receives", synth(a_never=True), 3, ["never received (the setup"]),
        ("the control moved", synth(ctl_b=1000), 3, ["the control U0 differs"]),
    ]
    bad = 0
    for name, data, want_rc, want_text in cases:
        lines, rc = read_all(data)
        text = "\n".join(lines)
        missing = [t for t in want_text if t not in text]
        if rc != want_rc or missing:
            bad += 1
            print(f"SELFTEST FAIL: {name}: rc {rc} (want {want_rc}), missing {missing}\n{text}")
        else:
            print(f"selftest ok: {name} (rc {rc})")
    print("SELFTEST OK" if bad == 0 else f"SELFTEST FAILED ({bad})")
    return 0 if bad == 0 else 1


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        sys.exit(selftest())
    if len(sys.argv) != 2:
        print("usage: nosend_benefit_read.py OUT_DIR | --selftest", file=sys.stderr)
        sys.exit(64)
    lines, rc = read_all(Data.load(sys.argv[1]))
    print("\n".join(lines))
    sys.exit(rc)


if __name__ == "__main__":
    main()
