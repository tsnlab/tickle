#!/usr/bin/env python3
"""Driver and analysis for frag_reasm_model.sh. Run through the .sh (it owns the netns EXIT trap).

PRE-REGISTRATION (written before the first run; the code below implements every rule here).

Grid. n = 1, 2, 4, 8, 16, 45 units (45*1446 + 16 = 65086 <= 65507, so 45 fits). p = 0, 1e-4, 1e-3,
1e-2, 5e-2 (netem loss on A's egress, per frame). Payload rates 3 MB/s and 30 MB/s (unit bytes only).
A datagram is f(n) IP frames: f = 1 for n = 1 (1462 B UDP payload <= 1472), else
ceil((8 + 16 + 1446 n) / 1480). f(n) = n for every grid point except n = 45, where f = 44. The
prediction tested is q0 = (1-p)^f (what the network can do); (1-p)^n is printed beside it.

Sample size (worked out first, printed by --plan). A cell's acceptance region must exclude a
doubling of the predicted loss L0 = 1 - q0, i.e. the 95% half-width 1.96 sqrt(L0/N) <= L0, so
N L0 >= 3.84 lost datagrams expected. Because L0 ~ f p and the datagram rate ~ 1/n, N L0 ~ (units/s)
T p: at 3 MB/s (2075 units/s) p = 1e-4 needs T >= 18.5 s, so p = 1e-4 cells measure 25 s and the
others 10 s; at 30 MB/s every cell measures 5 s (p = 1e-4: 10 lost expected). p = 0 cells measure 5 s.
P3's exclusions need far less (the cost gaps to the grid points outside the accepted set are
>= 0.0017 u; printed per p).

Warm-up and steady state. Above ipfrag_high_thresh the kernel drops EVERY new fragment
(inet_frag_find returns NULL) until incomplete queues expire after ipfrag_time, so a saturating cell
is periodic with period ~ipfrag_time and its first ipfrag_time is a transient. A cell whose predicted
steady fragment memory dg_rate * L0 * ipfrag_time * max(1, f-1) * m1 exceeds 0.3 * high_thresh is a
LONG cell: warm-up ipfrag_time + 2 s, then measure ipfrag_time (one full period). m1, the memory one
incomplete queue holding one fragment costs, is calibrated at start (n = 2, p = 0.05, 3 MB/s, 4 s:
FRAG memory / FRAG inuse). Below the threshold nothing is ever dropped, so the transient and the
steady state give the same q and other cells warm up 2 s.

Reductions to stay near 40 min: the 30 MB/s p = 0.05 cells n = 4, 8, 16 are dropped (n = 1, 2, 45
kept). No prediction reads them. If the estimate still exceeds FQ_MAX_MIN (45) minutes the run aborts.

Per cell VOID (the cell says nothing): B's Udp RcvbufErrors or InErrors delta > 0 (socket drops);
sender send errors > 0 or achieved payload rate < 95% of target (treatment not applied); global
softnet_stat dropped delta > 0 (a backlog drop the model does not contain); realized netem frame loss
(qdisc drops / frames) outside its own 99.9% binomial range around p (netem did not apply p).

P0  Independence (the model's premise). In the n = 1, p in {0.01, 0.05} cells, P(lost | previous lost)
    is inside the 95% binomial region of p over the number of lost-predecessor pairs. Else FAIL: netem
    loss is bursty and (1-p)^n is the wrong baseline.
P1  At 3 MB/s, every non-void cell's lost count is inside the exact binomial region of L0 at
    alpha = 0.05/30 (Bonferroni over the 30 cells; P(all inside | model true) = 95%). p = 0 cells
    need lost = 0. PASS if all inside, FAIL lists the cells outside and their direction.
    Secondary reading, printed not scored: cells predicted to saturate reassembly memory should be
    the ones below the region.
P2  At 30 MB/s, p = 0.01: every n >= 2 cell's lost count is ABOVE the region of L0 at alpha =
    0.05/5 (one-sided: q below (1-p)^f beyond the CI); and every p = 0 cell at 30 MB/s has lost = 0.
P3  For p in {1e-4, 1e-3, 1e-2} at 3 MB/s, argmin over the grid of cost(n) with the MEASURED q equals
    the grid point nearest n* = sqrt(h/(u p)) (linear distance) or one of its grid neighbours.
    VOID for a p if any of its cells is void. (30 MB/s argmin printed, not scored.)
P4  Fragment drop: a tc flower filter on B's ingress drops every IP fragment, p = 0, 3 MB/s: n = 1
    lost = 0 and filter hits = 0; n = 2 and n = 45 received = 0 and filter hits > 0.
Control (A/A). The fragmented (n >= 2), p > 0, non-LONG 3 MB/s cell with the largest expected lost
    count is run again at the end. If the two lost fractions differ with |z| > 1.96 (two-proportion,
    pooled) the WHOLE run is VOID: the cell-to-cell noise is larger than binomial and no CI here holds.
"""
import json
import math
import os
import subprocess
import sys
import time

from scipy.stats import binom

U, H = 1446, 24
NS_A, NS_B = os.environ["FQ_NS_A"], os.environ["FQ_NS_B"]
OUT = os.environ["FQ_OUT_DIR"]
PROBE = os.environ["FQ_PROBE"]
PORT = 47001
GRID_N = [1, 2, 4, 8, 16, 45]
GRID_P = [0.0, 1e-4, 1e-3, 1e-2, 5e-2]
RATES = [3e6, 30e6]
DROPPED_CELLS = {(30e6, 5e-2, 4), (30e6, 5e-2, 8), (30e6, 5e-2, 16)}
LONG_FRACTION = 0.3
MAX_MIN = float(os.environ.get("FQ_MAX_MIN", "45"))
RES = open(os.path.join(OUT, "results.txt"), "a")
JSONL = open(os.path.join(OUT, "cells.jsonl"), "a")


def log(msg=""):
    print(msg)
    RES.write(msg + "\n")
    RES.flush()


def frames(n):
    pl = 16 + U * n
    return 1 if pl <= 1472 else math.ceil((8 + pl) / 1480)


def nsx(ns, *cmd, check=True):
    r = subprocess.run(["sudo", "-n", "ip", "netns", "exec", ns, *cmd], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"failed rc={r.returncode}: {ns} {' '.join(cmd)}: {r.stderr.strip()}")
    return r.stdout


def ipc(*cmd, check=True):
    r = subprocess.run(["sudo", "-n", "ip", *cmd], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"failed rc={r.returncode}: ip {' '.join(cmd)}: {r.stderr.strip()}")
    return r.stdout


def ns_list():
    r = subprocess.run(["ip", "netns", "list"], capture_output=True, text=True)
    return {line.split()[0] for line in r.stdout.splitlines() if line.strip()}


def teardown():
    for ns in (NS_A, NS_B):
        if ns in ns_list():
            ipc("netns", "del", ns, check=False)
    left = ns_list() & {NS_A, NS_B}
    if left:
        raise RuntimeError(f"netns still present after delete: {left}")


def setup(p, fragdrop=False):
    teardown()
    ipc("netns", "add", NS_A)
    ipc("netns", "add", NS_B)
    nsx(NS_A, "ip", "link", "add", "vA", "type", "veth", "peer", "name", "vB", "netns", NS_B)
    nsx(NS_A, "ip", "addr", "add", "10.77.0.1/24", "dev", "vA")
    nsx(NS_B, "ip", "addr", "add", "10.77.0.2/24", "dev", "vB")
    for ns, dev in ((NS_A, "vA"), (NS_B, "vB")):
        nsx(ns, "ip", "link", "set", "lo", "up")
        nsx(ns, "ip", "link", "set", dev, "up")
    netem = ["tc", "qdisc", "add", "dev", "vA", "root", "netem", "limit", "100000"]
    if p > 0:
        netem += ["loss", f"{p * 100:g}%"]
    nsx(NS_A, *netem)
    q = nsx(NS_A, "tc", "qdisc", "show", "dev", "vA")
    if "netem" not in q or ((p > 0) != ("loss" in q)):
        raise RuntimeError(f"netem not as asked (p={p}): {q}")
    if fragdrop:
        nsx(NS_B, "tc", "qdisc", "add", "dev", "vB", "clsact")
        nsx(NS_B, "tc", "filter", "add", "dev", "vB", "ingress", "protocol", "ip", "flower", "ip_flags",
            "frag", "action", "drop")
        f = nsx(NS_B, "tc", "filter", "show", "dev", "vB", "ingress")
        if "frag" not in f:
            raise RuntimeError(f"frag filter missing: {f}")
    # ARP once so the first datagram is not queued behind resolution.
    nsx(NS_A, "ping", "-c", "1", "-W", "1", "10.77.0.2", check=False)


def snmp(ns):
    lines = nsx(ns, "cat", "/proc/net/snmp").splitlines()
    out = {}
    for i in range(0, len(lines) - 1, 2):
        k = lines[i].split()
        v = lines[i + 1].split()
        if k[0] == v[0]:
            for name, val in zip(k[1:], v[1:]):
                out[k[0][:-1] + "." + name] = int(val)
    return out


def frag_sockstat(ns):
    for line in nsx(ns, "cat", "/proc/net/sockstat").splitlines():
        if line.startswith("FRAG:"):
            t = line.split()
            return int(t[2]), int(t[4])
    return None, None


def softnet_drops():
    with open("/proc/net/softnet_stat") as fh:
        return sum(int(line.split()[1], 16) for line in fh)


def qdisc_stats():
    txt = nsx(NS_A, "tc", "-s", "qdisc", "show", "dev", "vA")
    t = txt.split()
    i = t.index("Sent")
    return int(t[i + 3]), int(t[i + 6].rstrip(","))  # packets sent, dropped


def filter_hits():
    txt = nsx(NS_B, "tc", "-s", "filter", "show", "dev", "vB", "ingress")
    t = txt.split()
    # "Sent X bytes Y pkt" inside the action stats
    return int(t[t.index("Sent") + 3]) if "Sent" in t else -1


def sysctls(ns):
    out = {}
    for k in ("ipfrag_high_thresh", "ipfrag_low_thresh", "ipfrag_time", "ipfrag_max_dist"):
        out[k] = int(nsx(ns, "cat", f"/proc/sys/net/ipv4/{k}").strip())
    for k in ("rmem_max", "rmem_default", "netdev_max_backlog"):
        with open(f"/proc/sys/net/core/{k}") as fh:
            out[k] = int(fh.read().strip())
    return out


def popcount_range(x, lo, hi):
    """received count among seq lo..hi inclusive; x is the bitmap as a little-endian int"""
    if hi < lo:
        return 0
    x >>= lo
    x &= (1 << (hi - lo + 1)) - 1
    return x.bit_count()


def run_cell(rate, n, p, warm, meas, tag, fragdrop=False):
    setup(p, fragdrop)
    sc = sysctls(NS_B)
    load = open("/proc/loadavg").read().split()[:3]
    s0b, s0a, sn0, qd0 = snmp(NS_B), snmp(NS_A), softnet_drops(), qdisc_stats()
    bmfile = os.path.join(OUT, f"bitmap_{tag}.bin")
    deadline = warm + meas + 2.0
    with open(bmfile, "wb") as bmf:
        recv = subprocess.Popen(["sudo", "-n", "ip", "netns", "exec", NS_B, PROBE, "recv", str(PORT),
                                 str(deadline)], stdout=bmf, stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)
        t0 = time.time()
        send = subprocess.Popen(["sudo", "-n", "ip", "netns", "exec", NS_A, PROBE, "send", "10.77.0.2",
                                 str(PORT), str(n), str(rate), str(warm), str(meas)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        series = []
        while send.poll() is None:
            time.sleep(1.0)
            inuse, mem = frag_sockstat(NS_B)
            series.append((round(time.time() - t0, 1), inuse, mem))
        sout, serr = send.communicate()
        _, rerr = recv.communicate(timeout=deadline + 30)
    s1b, s1a, sn1, qd1 = snmp(NS_B), snmp(NS_A), softnet_drops(), qdisc_stats()
    fend = frag_sockstat(NS_B)
    hits = filter_hits() if fragdrop else None
    snd = dict(kv.split("=") for kv in sout.split()[1:]) if sout.startswith("SEND") else {}
    rcv = dict(kv.split("=") for kv in rerr.split()[1:]) if rerr.startswith("RECV") else {}
    if not snd or not rcv:
        raise RuntimeError(f"probe output missing: send={sout!r} {serr!r} recv={rerr!r}")
    with open(bmfile, "rb") as fh:
        bm = int.from_bytes(fh.read(), "little")
    fm, last = int(snd["first_measure_seq"]), int(snd["last_seq"])
    N = last - fm + 1
    rx = popcount_range(bm, fm, last)
    d = lambda a, b, k: b.get(k, 0) - a.get(k, 0)  # noqa: E731
    f = frames(n)
    q0 = (1 - p) ** f
    meas_series = [s for s in series if s[0] >= warm]
    c = dict(tag=tag, rate=rate, n=n, f=f, p=p, warm=warm, meas=meas, fragdrop=fragdrop, load=load,
             send_pid=send.pid, recv_pid=recv.pid, N=N, rx=rx, lost=N - rx, q=rx / N if N else float("nan"),
             q0=q0, qn=(1 - p) ** n, send=snd, recv=rcv,
             rcvbuf_err=d(s0b, s1b, "Udp.RcvbufErrors"), in_err=d(s0b, s1b, "Udp.InErrors"),
             reasm_reqds=d(s0b, s1b, "Ip.ReasmReqds"), reasm_oks=d(s0b, s1b, "Ip.ReasmOKs"),
             reasm_fails=d(s0b, s1b, "Ip.ReasmFails"), reasm_timeout=d(s0b, s1b, "Ip.ReasmTimeout"),
             frag_creates=d(s0a, s1a, "Ip.FragCreates"), softnet_drop=sn1 - sn0,
             qdisc_pkts=qd1[0] - qd0[0], qdisc_drops=qd1[1] - qd0[1],
             frag_mem_max=max((s[2] for s in meas_series), default=None),
             frag_mem_mean=(sum(s[2] for s in meas_series) / len(meas_series)) if meas_series else None,
             frag_inuse_max=max((s[1] for s in meas_series), default=None), frag_end=fend,
             frag_series=series, filter_hits=hits, sysctl=sc)
    # per-second q over the measure window (shows the reassembly duty cycle in LONG cells)
    dg_rate = rate / (U * n)
    step = max(1, int(round(dg_rate)))
    c["q_per_s"] = [round(popcount_range(bm, s, min(s + step - 1, last)) / (min(s + step - 1, last) - s + 1), 3)
                    for s in range(fm, last + 1, step)]
    if n == 1 and p >= 0.01:
        x = bm >> fm
        lost_prev = pairs = 0
        prev = None
        for i in range(N):
            cur = not ((x >> i) & 1)
            if prev:
                lost_prev += 1
                pairs += cur
            prev = cur
        c["indep"] = dict(prev_lost=lost_prev, both_lost=pairs)
    # VOID rules
    why = []
    if c["rcvbuf_err"] > 0 or c["in_err"] > 0:
        why.append(f"socket drops rcvbuf={c['rcvbuf_err']} inerr={c['in_err']}")
    if int(snd["send_errs"]) > 0:
        why.append(f"send_errs={snd['send_errs']}")
    if float(snd["achieved_Bps"]) < 0.95 * rate:
        why.append(f"rate {float(snd['achieved_Bps']):.0f} < 95% of {rate:.0f}")
    if c["softnet_drop"] > 0:
        why.append(f"softnet drops {c['softnet_drop']}")
    tot = c["qdisc_pkts"] + c["qdisc_drops"]
    c["p_realized"] = c["qdisc_drops"] / tot if tot else float("nan")
    if p > 0 and tot:
        lo, hi = binom.ppf(0.0005, tot, p), binom.isf(0.0005, tot, p)
        if not lo <= c["qdisc_drops"] <= hi:
            why.append(f"netem realized {c['p_realized']:.5f} outside 99.9% of p={p}")
    elif p == 0 and c["qdisc_drops"] > 0:
        why.append(f"qdisc dropped {c['qdisc_drops']} at p=0")
    c["void"] = why
    JSONL.write(json.dumps(c) + "\n")
    JSONL.flush()
    log(f"  {tag:<22} N={N:<8} lost={N - rx:<8} q={c['q']:.5f} q0={q0:.5f} realized_p={c['p_realized']:.5f} "
        f"reasm req/ok/fail/tmo={c['reasm_reqds']}/{c['reasm_oks']}/{c['reasm_fails']}/{c['reasm_timeout']} "
        f"fragmem max={c['frag_mem_max']} inuse max={c['frag_inuse_max']} rcvbufErr={c['rcvbuf_err']} "
        f"inErr={c['in_err']} load={' '.join(load)}" + (f" filter_hits={hits}" if fragdrop else "")
        + (f" VOID[{'; '.join(why)}]" if why else ""))
    teardown()
    return c


def region(N, L0, alpha):
    if L0 <= 0:
        return 0, 0
    return int(binom.ppf(alpha / 2, N, L0)), int(binom.isf(alpha / 2, N, L0))


def cost(n, q):
    return (U + H / n) + (1 - q) * (U + H)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "full"
    log(f"=== frag_reasm_model {time.strftime('%Y-%m-%d %H:%M:%S')} mode={mode} out={OUT}")
    log(f"probe={PROBE} netns={NS_A},{NS_B}")
    log("--- calibration: n=2 p=0.05 3 MB/s 4 s (m1 = FRAG memory / inuse just after the sender stops)")
    setup(0.05)
    sc = sysctls(NS_B)
    log(f"default sysctls read in fresh netns B (not changed): {sc}")
    teardown()
    cal = run_cell(3e6, 2, 0.05, 0.0, 4.0, "calib")
    inuse, mem = cal["frag_end"]
    if not inuse:
        raise RuntimeError("calibration saw no reassembly queues")
    m1 = mem / inuse
    T, HI = sc["ipfrag_time"], sc["ipfrag_high_thresh"]
    log(f"m1 = {mem}/{inuse} = {m1:.0f} B per queue holding one fragment; ipfrag_time={T} high_thresh={HI}")

    cells = []
    for rate in RATES:
        for p in GRID_P:
            for n in GRID_N:
                if (rate, p, n) in DROPPED_CELLS:
                    continue
                f = frames(n)
                L0 = 1 - (1 - p) ** f
                dg = rate / (U * n)
                pred_mem = dg * L0 * T * max(1, f - 1) * m1 if f >= 2 else 0.0
                long_ = p > 0 and f >= 2 and pred_mem > LONG_FRACTION * HI
                if long_:
                    warm, meas = T + 2.0, float(T)
                else:
                    warm = 2.0
                    meas = 5.0 if p == 0 or rate > 3e6 else (25.0 if p == 1e-4 else 10.0)
                N = dg * meas
                cells.append(dict(rate=rate, p=p, n=n, f=f, L0=L0, N=N, warm=warm, meas=meas, long=long_,
                                  pred_mem=pred_mem))
    aa = max((c for c in cells if c["rate"] == 3e6 and c["n"] >= 2 and c["p"] > 0 and not c["long"]),
             key=lambda c: c["N"] * c["L0"])
    fd = [dict(rate=3e6, p=0.0, n=n, warm=1.0, meas=4.0) for n in (1, 2, 45)]
    over = 4.0
    est = sum(c["warm"] + c["meas"] + over for c in cells) + aa["warm"] + aa["meas"] + over \
        + sum(c["warm"] + c["meas"] + over for c in fd)
    log("--- plan (sample size: expected N, expected lost N*L0, 95% half-width / L0 must be <= 1)")
    for c in cells:
        hw = 1.96 * math.sqrt(c["L0"] * (1 - c["L0"]) / c["N"]) / c["L0"] if c["L0"] > 0 else 0.0
        log(f"  {c['rate'] / 1e6:>4.0f}MB/s p={c['p']:<6g} n={c['n']:<2} f={c['f']:<2} N~{c['N']:<8.0f} "
            f"lost~{c['N'] * c['L0']:<8.1f} hw/L0={hw:<5.2f} pred_mem={c['pred_mem'] / 1e6:<8.2f}MB "
            f"{'LONG' if c['long'] else 'short'} warm={c['warm']:.0f}s meas={c['meas']:.0f}s"
            + ("  UNDERPOWERED" if hw > 1 else ""))
    for p in (1e-4, 1e-3, 1e-2):
        ns = math.sqrt(H / (U * p))
        cs = {n: cost(n, (1 - p) ** frames(n)) for n in GRID_N}
        log(f"  P3 p={p:g}: n*={ns:.2f} predicted cost/u-1: "
            + " ".join(f"{n}:{cs[n] / U - 1:.5f}" for n in GRID_N))
    log(f"  A/A control cell: 3MB/s n={aa['n']} p={aa['p']:g} (expected lost {aa['N'] * aa['L0']:.0f})")
    log(f"  reduced: dropped {sorted((r / 1e6, p, n) for r, p, n in DROPPED_CELLS)}")
    log(f"  ESTIMATE: {len(cells) + 1 + len(fd)} cells, {est / 60:.1f} min")
    if mode == "--plan":
        return 0
    if mode == "--smoke":
        cells = [c for c in cells if c["p"] in (0.0, 1e-2) and c["n"] in (1, 8)]
        for c in cells:
            c["warm"], c["meas"] = 1.0, 2.0
        fd = fd[:2]
        aa = dict(cells[-1])
    elif est / 60 > MAX_MIN:
        log(f"ABORT: estimate {est / 60:.1f} min > {MAX_MIN}")
        return 3

    res = {}
    for c in cells:
        tag = f"r{c['rate'] / 1e6:g}_p{c['p']:g}_n{c['n']}"
        res[(c["rate"], c["p"], c["n"])] = run_cell(c["rate"], c["n"], c["p"], c["warm"], c["meas"], tag)
        res[(c["rate"], c["p"], c["n"])]["long"] = c["long"]
        res[(c["rate"], c["p"], c["n"])]["pred_mem"] = c["pred_mem"]
    fdres = [run_cell(c["rate"], c["n"], c["p"], c["warm"], c["meas"], f"fragdrop_n{c['n']}", fragdrop=True)
             for c in fd]
    aar = run_cell(aa["rate"], aa["n"], aa["p"], aa["warm"], aa["meas"], f"AA_n{aa['n']}_p{aa['p']:g}")
    if mode == "--smoke":
        log("SMOKE done: plumbing only, no verdicts")
        return 0
    verdicts(res, fdres, aar, aa, mode)
    return 0


def verdicts(res, fdres, aar, aa, mode):
    log("=== q(n): measured / predicted (1-p)^f   [* = outside P1/P2 region, V = VOID, L = LONG cell]")
    for rate in RATES:
        log(f"--- {rate / 1e6:g} MB/s payload")
        log("  n  f  " + "".join(f"{'p=' + format(p, 'g'):>24}" for p in GRID_P))
        for n in GRID_N:
            row = f" {n:>2} {frames(n):>2} "
            for p in GRID_P:
                c = res.get((rate, p, n))
                if not c:
                    row += f"{'-':>24}"
                    continue
                lo, hi = region(c["N"], 1 - c["q0"], 0.05 / 30 if rate == 3e6 else 0.05 / 5)
                mark = ("V" if c["void"] else "") + ("L" if c.get("long") else "") + \
                    ("*" if not lo <= c["lost"] <= hi else "")
                row += f"{c['q']:>10.5f}/{c['q0']:.5f}{mark:<3}"
            log(row)
    # P0
    log("=== verdicts")
    p0 = []
    for (rate, p, n), c in sorted(res.items()):
        if "indep" in c and rate == 3e6:
            k, m = c["indep"]["both_lost"], c["indep"]["prev_lost"]
            lo, hi = binom.ppf(0.025, m, p), binom.isf(0.025, m, p)
            ok = lo <= k <= hi and not c["void"]
            p0.append(ok)
            log(f"  P0 p={p:g}: P(lost|prev lost) = {k}/{m} = {k / max(m, 1):.4f} vs p (95% region {lo:.0f}..{hi:.0f})"
                f" realized p={c['p_realized']:.5f} -> {'inside' if ok else 'OUTSIDE'}")
    log(f"P0 {'PASS' if p0 and all(p0) else 'FAIL'} (netem loss per-frame independent)")
    # P1
    outside, void = [], []
    for (rate, p, n), c in sorted(res.items()):
        if rate != 3e6:
            continue
        if c["void"]:
            void.append(c["tag"])
            continue
        lo, hi = region(c["N"], 1 - c["q0"], 0.05 / 30)
        if not lo <= c["lost"] <= hi:
            outside.append(f"{c['tag']}(lost {c['lost']} vs {lo}..{hi}, {'excess' if c['lost'] > hi else 'deficit'}, "
                           f"pred_mem {c['pred_mem'] / 1e6:.1f}MB{', LONG' if c.get('long') else ''})")
    log(f"P1 {'PASS' if not outside else 'FAIL'}: {len(outside)} of {sum(1 for k in res if k[0] == 3e6) - len(void)} "
        f"3 MB/s cells outside; void={void}")
    for o in outside:
        log(f"    {o}")
    # P2
    p2, p2v = [], []
    for n in GRID_N[1:]:
        c = res[(30e6, 1e-2, n)]
        lo, hi = region(c["N"], 1 - c["q0"], 0.05 / 5)
        if c["void"]:
            p2v.append(c["tag"])
        p2.append(c["lost"] > hi)
        log(f"  P2 n={n}: q={c['q']:.5f} vs q0={c['q0']:.5f} lost {c['lost']} region {lo}..{hi} "
            f"-> {'BELOW (excess loss)' if c['lost'] > hi else 'not below'}{' VOID' if c['void'] else ''}")
    zero_ok = all(res[(30e6, 0.0, n)]["lost"] == 0 for n in GRID_N)
    log(f"  P2 p=0 at 30 MB/s: lost = {[res[(30e6, 0.0, n)]['lost'] for n in GRID_N]}")
    log(f"P2 {'VOID' if p2v else ('PASS' if all(p2) and zero_ok else 'FAIL')}")
    # P3
    for rate in RATES:
        for p in (1e-4, 1e-3, 1e-2):
            cs = {n: cost(n, res[(rate, p, n)]["q"]) for n in GRID_N}
            am = min(cs, key=cs.get)
            ns = math.sqrt(H / (U * p))
            near = min(GRID_N, key=lambda n: abs(n - ns))
            i = GRID_N.index(near)
            acc = set(GRID_N[max(0, i - 1):i + 2])
            vd = any(res[(rate, p, n)]["void"] for n in GRID_N)
            v = "VOID" if vd else ("PASS" if am in acc else "FAIL")
            log(f"{'P3' if rate == 3e6 else '  (30 MB/s, not scored)'} p={p:g}: n*={ns:.2f} nearest={near} "
                f"accepted={sorted(acc)} measured argmin={am} -> {v}; cost/u-1: "
                + " ".join(f"{n}:{cs[n] / U - 1:.5f}" for n in GRID_N))
    # P4
    ok4 = True
    for c in fdres:
        good = (c["lost"] == 0 and c["filter_hits"] == 0) if c["n"] == 1 else (c["rx"] == 0 and c["filter_hits"] > 0)
        ok4 &= good
        log(f"  P4 n={c['n']}: received {c['rx']}/{c['N']} filter_hits={c['filter_hits']} "
            f"reasm_reqds={c['reasm_reqds']} -> {'as predicted' if good else 'NOT as predicted'}")
    log(f"P4 {'PASS' if ok4 else 'FAIL'}")
    # A/A
    a = res[(aa["rate"], aa["p"], aa["n"])]
    p1, p2_ = a["lost"] / a["N"], aar["lost"] / aar["N"]
    pp = (a["lost"] + aar["lost"]) / (a["N"] + aar["N"])
    se = math.sqrt(pp * (1 - pp) * (1 / a["N"] + 1 / aar["N"])) if 0 < pp < 1 else 0
    z = (p1 - p2_) / se if se else 0.0
    log(f"A/A {aa['n']=} {aa['p']=}: lost {a['lost']}/{a['N']} vs {aar['lost']}/{aar['N']} z={z:.2f} -> "
        f"{'RUN VOID' if abs(z) > 1.96 else 'consistent (run valid)'}")
    log(f"mode={mode}" + ("  (SMOKE: not a measurement)" if mode == "--smoke" else ""))


if __name__ == "__main__":
    try:
        sys.exit(main())
    finally:
        try:
            teardown()
        except Exception as e:  # the .sh EXIT trap is the backstop
            print(f"teardown error: {e}")
