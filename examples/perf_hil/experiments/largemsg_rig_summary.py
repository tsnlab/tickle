#!/usr/bin/env python3
"""Large-message step A on the rig: the L3 and L4 readings of rmw_keepall_rig.sh runs (docs/DESIGN.md section 8).

Usage: largemsg_rig_summary.py <OUT.runs> <DUR> [--l3] [--preflight]
  (default) L4, the vendor cells; rmw_keepall_rig.sh calls it this way when SUMMARY names it.
  --l3      L3, the end-of-sample HEARTBEAT: tickle@head (B) against tickle@pre (B without it, PRE_SHA = the -nohb
            branch), with the same arms at 0% loss as the control.
  --preflight  also apply the PC preflight's rule (rmw_keepall_rig.sh PC_PREFLIGHT=1) and exit 1 when it fails: no
            VOID run; every cross-host tickle run delivered something; with the probe, every cross-host run that
            delivered has a usable clock and both probes within 200 us of 0 (one host: the probe's own control).
Written 2026-10-10 for ~/rig_queue_largemsg_A4.sh before its run. Every rule below is enforced in this code.

WHAT A RUN GIVES (perf_test's per-second rows; received, sent and lost are whole counts per row):
  delivered   samples received, rate_hz (mean over the steady seconds), lost = perf_test's id gaps after the first
              delivered second (the match split as rmw_keepall_rig_summary.py does), delivered_pct = received /
              (received + lost), and end_gap = published - pre-match - received - lost: the samples after the last
              one delivered (the publisher exits with ~0.4 s in flight). A run is INCOMPLETE when lost > 0, end_gap is
              more than one second of samples (a stall), or it received nothing.
  latency     perf_test stamps with the publisher's system_clock and takes the latency on the subscriber's. Two
              readings, both from the steady seconds (the first 2 delivered seconds and the last 1 dropped, the same
              warm-up and cool-down for every arm):
              - one clock, by the probe: rmw_keepall_rig.sh CLOCK_PROBE=1 measured the offset (server - client)
                before and after the run; it is interpolated linearly to each second (the subscriber's recorded start
                + T_experiment) and subtracted. Same-host runs are one clock already (offset 0). The clock of a run is
                UNUSABLE when either probe failed, its best round trip was over 1 ms, the offset moved more than
                200 us per second between the probes, or the corrected minimum is below 95% of the wire floor
                (bytes x 8 x 1538 / 1452 at 1 Gb/s: 8.9 ms at 1 MB, 35.5 ms at 4 MB) - a one-way latency cannot be
                shorter than the wire. Its latency columns then read '-'; delivery and CPU still count.
                lat_med  the median over seconds of the per-second mean (perf_test gives no per-sample latencies).
                p99_lb   a LOWER BOUND on the run's p99: with N steady samples and k = floor(N / 100), the (k+1)-th
                         largest per-second maximum (at most k samples can lie above the p99, and each lies in a
                         second whose maximum is at least as large). The true p99 lies between p99_lb and max.
              - offset-free, no probe needed: each second's latencies less that second's minimum (the clocks move
                ~30 us in a second). ex_p99_lb is p99_lb of that excess, ex_mean the mean excess. This is what a lost
                fragment's repair adds, and it is L3's metric.
  CPU         getrusage of each process over its steady seconds (cumulative utime + stime, last steady row less the
              row before the first), in ms per MB (1e6 B): the subscriber's per MB delivered, the publisher's per MB
              published. RSS: each process's peak ru_maxrss, kB.
  VOID        only identity: the subscriber's maps do not show the arm's rmw library (for tickle the exact path), or
              the publisher's neither do nor does its log name the rmw. A run that received nothing is NOT void: it
              delivered 0%.

L4 READING (per cell; the cell is named from topic, rate, loss, QoS and SAMEHOST as in DESIGN.md's table):
  - Sample count first: each arm's reps and delivered samples, and the smallest loss rate a run of that many samples
    would show with 95% probability, 1 - 0.05^(1/N).
  - A vendor arm with no usable rep that delivered anything: "does not run", with its numbers; not scored.
  - Incomplete delivery (INCOMPLETE in any usable rep) in every cell except I4s: rmw_tickle LOSES the cell (the user's
    rule); a vendor arm is listed as incomplete and not scored on the other metrics. I4s is a saturation cell where
    nobody is expected to deliver everything: no one is excluded there.
  - Each metric, rmw_tickle (tickle@head) against every scored vendor arm (fastdds, fastdds@mms1472, cyclonedds):
    WIN when every rmw_tickle rep is better than every rep of every scored arm, LOSE when every rep of some scored arm
    is better than every rmw_tickle rep, DRAW otherwise (docs/TESTING.md section 4's range rule). Latency metrics use
    only reps with a usable clock; fewer than 2 on either side: "no clock". tickle@pre (L3's arm) is printed, never
    scored.
  - DESIGN.md's falsification checks, printed as HOLDS / FALSIFIED / NOT READ (with why):
    F1 I1L: rmw_tickle's median delivered_pct below any Fast DDS arm's, or its median p99_lb above twice that arm's.
    F2 I1 cross-host (each QoS): rmw_tickle's median subscriber CPU per MB not below every DDS arm that runs.
    F3 I4s: rmw_tickle's median delivered samples below the best DDS arm's.

L3 READING (--l3; cells Array1m at 5% loss, the test, and at 0%, the control):
  - Primary: ex_p99_lb, per rep; tickle@head against tickle@pre; at least 3 usable reps of each in each cell, or NO
    VERDICT. t = Welch (mean head - mean pre) / SE; r = mean pre / mean head.
  - Control (0% loss: nothing for the HEARTBEAT to reveal): t0, r0, R0 = max(r0, 1 / r0). If t0 < -2 (head lower even
    with nothing to repair) the two builds differ in more than the repair: NO VERDICT.
  - KEPT if t5 < -2 and r5 > R0: the end-of-sample HEARTBEAT lowers the 1 MB cross-host tail at 5% loss beyond what
    the same pair shows with no loss. Otherwise REMOVED (DESIGN.md L3: "lower; if not, removed").
  - Printed beside, not deciding: ex_mean, the probe-corrected lat_med and p99_lb, delivered_pct, and the vendor arms'
    ex_p99_lb range per cell (the rig's own tail spread in that cell).
"""
import math
import re
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rmw_keepall_rig_summary import INDEX_RE, loaded, rows  # noqa: E402

SIZES = {"Array1k": 1024, "Array4k": 4096, "Array16k": 16384, "Array64k": 65536, "Array256k": 262144,
         "Array1m": 1048576, "Array2m": 2097152, "Array4m": 4194304, "Array8m": 8388608}
WARM, COOL = 2, 1
VENDORS = ("fastdds", "fastdds@mms1472", "cyclonedds")
SUBJECT = "tickle@head"


def wire_floor_ms(size):
    return size * 8 * 1538 / 1452 / 1e9 * 1e3


def read_meta(runs):
    meta = {}
    try:
        for line in (runs / "meta.txt").read_text().splitlines():
            k, _, v = line.partition("=")
            meta[k] = v
    except OSError:
        pass
    return meta


def read_probe(path):
    out = {}
    try:
        for line in path.read_text().splitlines():
            label, _, rest = line.partition(" ")
            out[label] = dict(re.findall(r"(\w+)=(-?\d+)", rest))
    except OSError:
        pass
    return out


def read_int(path):
    try:
        return int(path.read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def clock_model(runs, stem, samehost):
    """(offset_ns(t_ns) or None, label)."""
    if samehost:
        return (lambda t: 0.0), "one host"
    pr = read_probe(runs / f"{stem}_clock.txt")
    b, a = pr.get("before"), pr.get("after")
    if not b or not a:
        return None, "no probe"
    if b.get("ok") != "1" or a.get("ok") != "1":
        return None, "probe failed"
    if max(int(b["delay_min_ns"]), int(a["delay_min_ns"])) > 1_000_000:
        return None, "probe round trip > 1 ms"
    tb, ta, ob, oa = int(b["wall_ns"]), int(a["wall_ns"]), int(b["offset_ns"]), int(a["offset_ns"])
    if ta <= tb:
        return None, "probe times out of order"
    rate = abs(oa - ob) / ((ta - tb) / 1e9)
    if rate > 200_000:
        return None, f"offset moved {rate / 1e3:.0f} us/s"
    return (lambda t: ob + (oa - ob) * (t - tb) / (ta - tb)), f"offset {ob / 1e3:+.0f}..{oa / 1e3:+.0f} us"


def counts(r, key):
    # perf_test's received/sent/lost columns are whole counts per row (they read 29, 30, 31 at 30 Hz with T_loop 1.03);
    # data_received is not used, since it is scaled.
    return r.get(key, 0)


def cpu(r):
    return r.get("ru_utime", 0.0) + r.get("ru_stime", 0.0)


def steady_window(rs, key):
    """(steady rows, the row before the first of them or None) of the rows with key > 0."""
    live = [i for i, r in enumerate(rs) if r.get(key, 0) > 0]
    if len(live) <= WARM + COOL + 1:
        return [], None
    idx = live[WARM:-COOL] if COOL else live[WARM:]
    return [rs[i] for i in idx], (rs[idx[0] - 1] if idx[0] > 0 else None)


def p99_lb(maxima, n_samples):
    if not maxima:
        return float("nan")
    k = min(int(n_samples // 100), len(maxima) - 1)
    return sorted(maxima, reverse=True)[k]


def run_record(runs, stem, fields, samehost, rate, wire=True):
    pmaps, smaps, _pk, _sk, want = fields
    arm, topic, loss, rep = re.match(r"(.+?)_(Array\w+|Struct\w+)_l(\d+)_r(\d+)$", stem).groups()
    sub, stext = rows(runs / f"{stem}_sub.log")
    pub, ptext = rows(runs / f"{stem}_pub.log")
    why = []
    header_rmw = re.search(r"RMW Implementation: (\S+)", ptext)
    vendor_by_log = not want.startswith("/") and header_rmw and want.startswith("lib" + header_rmw.group(1))
    if not loaded(want, pmaps) and not vendor_by_log:
        why.append(f"publisher did not load {want} (maps: {pmaps.strip() or 'none'})")
    if not loaded(want, smaps):
        why.append(f"subscriber did not load {want} (maps: {smaps.strip() or 'none'})")
    rec = dict(arm=arm, topic=topic, loss=int(loss), rep=int(rep), void="; ".join(why))
    if why:
        return rec
    size = SIZES.get(topic, 0)
    recv = round(sum(counts(r, "received") for r in sub))
    sent = round(sum(counts(r, "sent") for r in pub))
    first = next((i for i, r in enumerate(sub) if r.get("received", 0) > 0), None)
    lost_all = round(sum(counts(r, "lost") for r in sub))
    prematch = round(sum(counts(r, "lost") for r in sub[:first + 1])) if first is not None else 0
    lost = lost_all - prematch if first is not None else 0
    # Samples published after the last one delivered are not id gaps: the publisher exits at its max_runtime with its
    # last ~0.4 s in flight (A3's L3 runs: 8-14 of 1,742 at 30 Hz, every arm). Counted apart as end_gap; one second's
    # worth is allowed, more is a stall and counts as incomplete delivery.
    end_gap = sent - prematch - recv - lost
    allow = rate if rate > 0 else 100
    rec.update(recv=recv, sent=sent, lost=lost, prematch=prematch, end_gap=end_gap,
               delivered_pct=(100.0 * recv / (recv + lost) if recv + lost > 0 else 0.0),
               incomplete=lost > 0 or end_gap > allow or recv == 0,
               refused="failed to publish" in ptext,
               errors=[m for m in ("terminate", "exception", "Segmentation", "Aborted") if m in ptext + stext])
    ss, before = steady_window(sub, "received")
    rec["seconds"] = len(ss)
    rec["rate_hz"] = st.mean(counts(r, "received") for r in ss) if ss else 0.0
    # CPU per MB and RSS
    if ss and before is not None and size:
        mb = sum(counts(r, "received") for r in ss) * size / 1e6
        rec["cpu_sub"] = (cpu(ss[-1]) - cpu(before)) * 1e3 / mb if mb else float("nan")
    else:
        rec["cpu_sub"] = float("nan")
    ps, pbefore = steady_window(pub, "sent")
    if ps and pbefore is not None and size:
        mb = sum(counts(r, "sent") for r in ps) * size / 1e6
        rec["cpu_pub"] = (cpu(ps[-1]) - cpu(pbefore)) * 1e3 / mb if mb else float("nan")
    else:
        rec["cpu_pub"] = float("nan")
    rec["rss_sub"] = max((r.get("ru_maxrss", 0) for r in sub), default=float("nan"))
    rec["rss_pub"] = max((r.get("ru_maxrss", 0) for r in pub), default=float("nan"))
    # latency
    n_steady = sum(counts(r, "received") for r in ss)
    mins = [r["latency_min (ms)"] for r in ss]
    maxs = [r["latency_max (ms)"] for r in ss]
    means = [r["latency_mean (ms)"] for r in ss]
    rec["ex_p99_lb"] = p99_lb([mx - mn for mx, mn in zip(maxs, mins)], n_steady) if ss else float("nan")
    rec["ex_mean"] = st.mean(m - mn for m, mn in zip(means, mins)) if ss else float("nan")
    offset, label = clock_model(runs, stem, samehost)
    start = read_int(runs / f"{stem}_sub.start")
    rec["clock"] = label
    rec["lat_med"] = rec["p99_lb"] = rec["lat_max"] = rec["lat_min"] = float("nan")
    if ss and offset is not None and (start is not None or samehost):
        corr = [offset((start or 0) + r["T_experiment"] * 1e9) / 1e6 for r in ss]
        cmins = [m - c for m, c in zip(mins, corr)]
        floor = wire_floor_ms(size) if wire and not samehost and size else 0.0
        if min(cmins) < 0.95 * floor:
            rec["clock"] = (f"{label}; UNUSABLE: corrected min {min(cmins):.2f} ms below 95% of the wire floor "
                            f"{floor:.2f}")
        else:
            rec["lat_med"] = st.median(m - c for m, c in zip(means, corr))
            rec["p99_lb"] = p99_lb([m - c for m, c in zip(maxs, corr)], n_steady)
            rec["lat_max"] = max(m - c for m, c in zip(maxs, corr))
            rec["lat_min"] = min(cmins)
    elif offset is not None and start is None and not samehost:
        rec["clock"] = f"{label}; UNUSABLE: no subscriber start time"
    elif offset is not None and not ss:
        rec["clock"] = f"{label}; no steady seconds to place on it"
    # The clock itself (not whether this run had seconds to read on it): what the preflight checks.
    rec["clock_ok"] = offset is not None and "UNUSABLE" not in rec["clock"] and (start is not None or samehost)
    tx = re.findall(r"tx_shm=(\d+)", ptext)
    ux = re.findall(r"tx_udp=(\d+)", ptext)
    rec["witness"] = f"tx_shm={tx[-1]} tx_udp={ux[-1]}" if tx and ux else ""
    return rec


def load(runs):
    meta = read_meta(runs)
    samehost = meta.get("samehost") == "1"
    index = {}
    for line in (runs / "index.txt").read_text().splitlines():
        m = re.match(INDEX_RE, line)
        if m:
            index[m.group(1)] = m.groups()[1:]
    rate = int(meta.get("pub_rate", "0") or 0)
    wire = meta.get("pc_preflight") != "1"  # a veth has no 1 Gb/s wire: no floor in the PC preflight
    recs = [run_record(runs, stem, f, samehost, rate, wire) for stem, f in sorted(index.items())]
    return meta, samehost, recs


def cell_name(topic, loss, meta, samehost):
    rate = meta.get("pub_rate", "?")
    qos = meta.get("qos_args", "")
    q = "reliable" if "--reliable" in qos else "sensor_data"
    where = "same-host" if samehost else "cross-host"
    name = "?"
    if topic == "Array1m" and loss == 0:
        name = "I1"
    elif topic == "Array1m" and loss > 0:
        name = "I1L"
    elif topic == "Array4m" and loss == 0:
        name = "I4s" if (not samehost and rate == "30") else "I4"
    return name, f"{name} {topic} {rate} Hz {q} {where} loss {loss}%", q


def fmt(x, w=8, p=2):
    return f"{x:{w}.{p}f}" if isinstance(x, (int, float)) and not math.isnan(x) else f"{'-':>{w}}"


def finite(xs):
    return [x for x in xs if isinstance(x, (int, float)) and not math.isnan(x)]


def med(xs):
    xs = finite(xs)
    return st.median(xs) if xs else float("nan")


def print_runs(recs):
    print("=== per run (steady seconds; latency ms; CPU ms per MB; RSS kB) ===")
    for r in sorted(recs, key=lambda r: (r["topic"], r["loss"], r["arm"], r["rep"])):
        if r["void"]:
            print(f"{r['topic']:8} l{r['loss']:<2} {r['arm']:16} r{r['rep']} VOID: {r['void']}")
            continue
        print(f"{r['topic']:8} l{r['loss']:<2} {r['arm']:16} r{r['rep']} sent {r['sent']:5} recv {r['recv']:5} "
              f"lost {r['lost']:4} end {r['end_gap']:3} ({fmt(r['delivered_pct'], 6, 2)}%) "
              f"{fmt(r['rate_hz'], 5, 1)} Hz  "
              f"lat med {fmt(r['lat_med'])} p99>= {fmt(r['p99_lb'])} max {fmt(r['lat_max'])}  "
              f"excess p99>= {fmt(r['ex_p99_lb'])} mean {fmt(r['ex_mean'])}  cpu sub {fmt(r['cpu_sub'], 6)} "
              f"pub {fmt(r['cpu_pub'], 6)}  rss {fmt(r['rss_sub'], 7, 0)}/{fmt(r['rss_pub'], 7, 0)}  [{r['clock']}]"
              f"{'  REFUSED' if r['refused'] else ''}{('  ERR ' + ','.join(r['errors'])) if r['errors'] else ''}"
              f"{('  ' + r['witness']) if r['witness'] else ''}")


METRICS = (("delivered_pct", "delivered %", "higher", False), ("rate_hz", "rate Hz", "higher", False),
           ("lat_med", "latency median ms", "lower", True), ("p99_lb", "latency p99 (lower bound) ms", "lower", True),
           ("cpu_sub", "sub CPU ms/MB", "lower", False), ("cpu_pub", "pub CPU ms/MB", "lower", False),
           ("rss_sub", "sub peak RSS kB", "lower", False), ("rss_pub", "pub peak RSS kB", "lower", False))


def range_rule(t, vendors, better):
    """WIN / LOSE / DRAW, docs/TESTING.md section 4's range rule."""
    def beats(a, b):
        return min(a) > max(b) if better == "higher" else max(a) < min(b)
    if all(beats(t, v) for v in vendors.values()):
        return "WIN"
    losers = [n for n, v in vendors.items() if beats(v, t)]
    return f"LOSE (to {', '.join(losers)})" if losers else "DRAW"


def l4(recs, meta, samehost):
    cells = {}
    for r in recs:
        cells.setdefault((r["topic"], r["loss"]), []).append(r)
    falsif = []
    for (topic, loss), rs in sorted(cells.items()):
        name, title, qos = cell_name(topic, loss, meta, samehost)
        print(f"\n--- {title} ---")
        if not samehost:
            print(f"  wire floor at 1 Gb/s: {wire_floor_ms(SIZES.get(topic, 0)):.1f} ms")
        arms = {}
        for r in rs:
            arms.setdefault(r["arm"], []).append(r)
        usable = {a: [r for r in v if not r["void"]] for a, v in arms.items()}
        status = {}
        for a, v in sorted(usable.items()):
            n = sum(r["recv"] for r in v)
            pmin = 1 - 0.05 ** (1 / n) if n else float("nan")
            print(f"  {a:16} {len(v)} usable of {len(arms[a])} reps, {n} samples delivered (a loss rate above "
                  f"{fmt(100 * pmin, 1, 3)}% would show in them with 95% probability)")
            if a == "tickle@pre":
                status[a] = "L3 arm (B without the end-of-sample HEARTBEAT), not scored"
            elif not any(r["recv"] > 0 for r in v):
                status[a] = "does not run"
            elif name != "I4s" and any(r["incomplete"] for r in v):
                status[a] = "incomplete delivery"
            else:
                status[a] = "complete" if name != "I4s" else "runs"
        for a in sorted(arms):
            if status.get(a) in ("does not run", "incomplete delivery") or not usable.get(a):
                v = usable.get(a, [])
                print(f"  {a}: {status.get(a, 'no usable rep')} - recv " + ", ".join(str(r['recv']) for r in v)
                      + " lost " + ", ".join(str(r['lost']) for r in v)
                      + ("; VOID: " + " | ".join(r["void"] for r in arms[a] if r["void"]) if any(
                          r["void"] for r in arms[a]) else ""))
        print(f"  {'metric':30} " + " ".join(f"{a:>22}" for a in sorted(usable)) + "   rmw_tickle")
        t_status = status.get(SUBJECT)
        scored = [a for a in VENDORS if a in usable and status.get(a) in ("complete", "runs")]
        for key, label, better, needs_clock in METRICS:
            vals = {a: finite(r[key] for r in v if (not needs_clock or not math.isnan(r[key]))) for a, v in
                    usable.items()}
            cols = " ".join(f"{fmt(med(vals[a]), 10, 2)} [{len(vals[a])}]{'':>7}" for a in sorted(usable))
            if SUBJECT not in usable or not usable[SUBJECT]:
                verdict = "NO rmw_tickle run"
            elif t_status == "does not run":
                verdict = "LOSE (rmw_tickle delivered nothing)"
            elif t_status == "incomplete delivery":
                verdict = "LOSE (rmw_tickle delivered incompletely)"
            elif not scored:
                verdict = "no vendor to score against"
            elif len(vals[SUBJECT]) < 2 or any(len(vals[a]) < 2 for a in scored):
                verdict = "no clock" if needs_clock else "too few reps"
            else:
                verdict = range_rule(vals[SUBJECT], {a: vals[a] for a in scored}, better)
            print(f"  {label:30} {cols}   {verdict}")
        print("  (median over usable reps [reps counted]; scored against: " + (", ".join(scored) or "none") + ")")
        # falsification
        tick = usable.get(SUBJECT, [])
        fdds = [a for a in ("fastdds", "fastdds@mms1472") if a in usable and status.get(a) != "does not run"]
        if name == "I1L" and qos == "reliable" and not samehost:
            if not tick or not fdds:
                falsif.append(f"F1 {title}: NOT READ (no rmw_tickle or no running Fast DDS arm)")
            else:
                bad = []
                for a in fdds:
                    if med(r["delivered_pct"] for r in tick) < med(r["delivered_pct"] for r in usable[a]):
                        bad.append(f"delivers less than {a}")
                    tp, fp = med(r["p99_lb"] for r in tick), med(r["p99_lb"] for r in usable[a])
                    if math.isnan(tp) or math.isnan(fp):
                        bad.append(f"p99 vs {a} NOT READ (no clock)")
                    elif tp > 2 * fp:
                        bad.append(f"p99_lb {tp:.1f} ms > 2 x {a}'s {fp:.1f}")
                hard = [b for b in bad if "NOT READ" not in b]
                falsif.append(f"F1 {title}: {'FALSIFIED: ' + '; '.join(hard) if hard else 'HOLDS'}"
                              + (f" ({'; '.join(b for b in bad if 'NOT READ' in b)})" if len(hard) < len(bad) else ""))
        if name == "I1" and not samehost:
            dds = [a for a in VENDORS if a in usable and status.get(a) != "does not run"]
            tc = med(r["cpu_sub"] for r in tick)
            if not tick or not dds or math.isnan(tc):
                falsif.append(f"F2 {title}: NOT READ")
            else:
                worse = [a for a in dds if not tc < med(r["cpu_sub"] for r in usable[a])]
                falsif.append(f"F2 {title}: " + (f"FALSIFIED: sub CPU {tc:.2f} ms/MB not below "
                                                 + ", ".join(f"{a} {med(r['cpu_sub'] for r in usable[a]):.2f}"
                                                             for a in worse) if worse else
                                                 f"HOLDS (sub CPU {tc:.2f} ms/MB below every DDS arm)"))
        if name == "I4s":
            dds = [a for a in VENDORS if a in usable]
            if not tick or not dds:
                falsif.append(f"F3 {title}: NOT READ")
            else:
                best = max(dds, key=lambda a: med(r["recv"] for r in usable[a]))
                tr, br = med(r["recv"] for r in tick), med(r["recv"] for r in usable[best])
                falsif.append(f"F3 {title}: {'FALSIFIED' if tr < br else 'HOLDS'} (rmw_tickle {tr:.0f} complete "
                              f"samples, best DDS {best} {br:.0f})")
    print("\n=== DESIGN.md falsification checks in these runs ===")
    print("\n".join(falsif) if falsif else "(none of F1-F3's cells is in these runs)")


def welch(a, b):
    se = math.sqrt(st.variance(a) / len(a) + st.variance(b) / len(b))
    return (st.mean(b) - st.mean(a)) / se if se > 0 else 0.0


def l3(recs):
    pre, head = "tickle@pre", SUBJECT
    by = {}
    for r in recs:
        if not r["void"] and r["topic"] == "Array1m":
            by.setdefault(r["loss"], {}).setdefault(r["arm"], []).append(r)
    print("\n=== L3: the end-of-sample HEARTBEAT (tickle@head = B, tickle@pre = B without it) ===")
    res = {}
    for loss in sorted(by):
        arms = by[loss]
        print(f"Array1m loss {loss}%:")
        for a in sorted(arms):
            v = arms[a]
            print(f"  {a:16} n={len(v)} ex_p99_lb " + ", ".join(fmt(r['ex_p99_lb'], 1) for r in v)
                  + f" | ex_mean med {fmt(med(r['ex_mean'] for r in v), 1)}"
                  + f" | lat_med med {fmt(med(r['lat_med'] for r in v), 1)}"
                  + f" p99_lb med {fmt(med(r['p99_lb'] for r in v), 1)} | delivered % med "
                  + f"{fmt(med(r['delivered_pct'] for r in v), 1, 2)} lost {sum(r['lost'] for r in v)}")
        a, b = [r["ex_p99_lb"] for r in arms.get(pre, [])], [r["ex_p99_lb"] for r in arms.get(head, [])]
        a, b = finite(a), finite(b)
        if len(a) >= 3 and len(b) >= 3:
            t = welch(a, b)
            r_ = st.mean(a) / st.mean(b) if st.mean(b) > 0 else float("inf")
            res[loss] = (t, r_)
            print(f"  head vs pre: ex_p99_lb {st.mean(b):.2f} vs {st.mean(a):.2f} ms, t {t:+.2f}, pre/head {r_:.3f}")
        else:
            print(f"  head vs pre: too few usable reps ({len(b)} head, {len(a)} pre; 3 each needed)")
    test = [x for x in res if x > 0]
    if 0 not in res or not test:
        print("L3 VERDICT: NO VERDICT - the 5%-loss test or the 0%-loss control lacks 3 usable reps per arm")
        return
    t0, r0 = res[0]
    R0 = max(r0, 1 / r0) if r0 > 0 else float("inf")
    if t0 < -2:
        print(f"L3 VERDICT: NO VERDICT - at 0% loss head is already lower (t {t0:+.2f}): the builds differ beyond the "
              "repair the HEARTBEAT exists for")
        return
    for loss in test:
        t5, r5 = res[loss]
        if t5 < -2 and r5 > R0:
            print(f"L3 VERDICT (loss {loss}%): KEPT - the end-of-sample HEARTBEAT lowers the tail (t {t5:+.2f}, "
                  f"{r5:.2f}x) beyond the 0%-loss pair's {R0:.2f}x")
        else:
            print(f"L3 VERDICT (loss {loss}%): REMOVED - not lower beyond 2 x SE and the 0%-loss pair's {R0:.2f}x "
                  f"(t {t5:+.2f}, {r5:.2f}x); DESIGN.md L3: 'if not, removed'")


def main():
    runs = Path(sys.argv[1])  # argv[2] (DUR) is rmw_keepall_rig.sh's calling convention; the rows carry the time
    meta, samehost, recs = load(runs)
    print(f"runs {runs}: qos [{meta.get('qos_args', '?')}] rate {meta.get('pub_rate', '?')} Hz samehost "
          f"{int(samehost)} clock_probe {meta.get('clock_probe', '?')} pc_preflight {meta.get('pc_preflight', '0')}")
    print_runs(recs)
    voids = [r for r in recs if r["void"]]
    if voids:
        print(f"\n{len(voids)} VOID run(s) (identity), listed above, never averaged")
    if "--l3" in sys.argv[3:]:
        l3(recs)
    else:
        l4(recs, meta, samehost)
    if "--preflight" in sys.argv[3:]:
        sys.exit(preflight(runs, recs, meta, samehost))


def preflight(runs, recs, meta, samehost):
    """The PC preflight's pass rule (rmw_keepall_rig.sh PC_PREFLIGHT=1; module docstring, --preflight)."""
    bad = []
    if meta.get("pc_preflight") != "1":
        bad.append("meta.txt does not say pc_preflight=1")
    if not recs:
        bad.append("no runs")
    bad += [f"VOID {r['arm']} {r['topic']} l{r['loss']}: {r['void']}" for r in recs if r["void"]]
    for r in recs:
        if r["void"]:
            continue
        if r["arm"].startswith("tickle@") and not samehost and r["recv"] == 0:
            bad.append(f"{r['arm']} {r['topic']} l{r['loss']} delivered nothing cross-host")
        if not samehost and meta.get("clock_probe") == "1" and r["recv"] > 0:
            if not r["clock_ok"]:
                bad.append(f"{r['arm']} {r['topic']} l{r['loss']}: clock unusable ({r['clock']})")
            pr = read_probe(runs / f"{r['arm']}_{r['topic']}_l{r['loss']}_r{r['rep']}_clock.txt")
            for label in ("before", "after"):
                off = abs(int(pr.get(label, {}).get("offset_ns", 10 ** 9)))
                if off > 200_000:
                    bad.append(f"{r['arm']} {r['topic']} l{r['loss']}: {label} probe offset {off / 1e3:.0f} us on one "
                               "host (must be ~0)")
    if bad:
        print("\nPREFLIGHT FAIL:\n  " + "\n  ".join(bad))
        return 1
    print(f"\nPREFLIGHT PASS: {len(recs)} runs, no VOID, every cross-host tickle run delivered, every clock usable")
    return 0


if __name__ == "__main__":
    main()
