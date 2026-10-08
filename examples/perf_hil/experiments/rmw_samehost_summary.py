#!/usr/bin/env python3
"""Reads rmw_samehost.sh's runs and applies the reading rules written in its header (repeated here, as code).

Usage: rmw_samehost_summary.py <OUT.runs dir> [--preflight]

<runs>/params.txt (written by the driver before the first run) fixes the window and the thresholds; every
<runs>/<stem>/ is one run left by rmw_samehost_cell.sh. With --preflight the exit status says whether every run was
usable (0) or not (1), and nothing is scored.

Per run, VOID when (each one implemented in check_rtt / check_tput below):
  - the cell script did not finish (no done=1), or a leftover ping/pong/perf_test/zenoh router was running before it;
  - a process's /proc/PID/maps did not show the arm's rmw library (rmw_tickle: the exact path built for this run),
    or showed more than one rmw implementation; a vendor publisher that ended before its maps were read is
    identified by its log's "RMW Implementation:" line, as rmw_keepall_rig_summary.py does;
  - rtt: no RESULT line naming the rmw, fewer than MIN_MEASURED round trips in the window, a round trip in the
    window that never came back, no stamps, or no sampler readings on both sides of the window;
  - tput: perf_test's QoS echo is not the cell's, or either log has fewer than 3 per-second rows inside the sender
    window (seconds WARM_S..DUR-COOL_S of the publisher, mapped onto the subscriber's clock by the two launch times);
    a subscriber that receives little or nothing in that window is a low delivered rate, not a VOID;
  - zenoh: the router was not listening;
  - rmw_tickle: no "traffic:" line (could not look - never read as zero), or its tx_shm share below 0.5 (the arm
    did not run on its segment, so it is not this cell), or - on cells whose payload is large enough for the bytes
    witness - tx_shm says segment while the interface bytes say the payload crossed the kernel (an instrument
    disagrees; named, not averaged).
Not VOID, reported:
  - a vendor's transport as the bytes witness reads it (shm / kernel / mixed); that is a finding about its default.
    The witness is interface bytes per sample sent (tput) or per round trip (rtt), in units of 2 x payload;
  - tput RELIABLE: a publisher that ended on "failed to publish" is REFUSED (KEEP_ALL refusing under
    max_blocking_time); samples lost after the first delivered second are DELIVERY FAILED for a vendor (excluded
    from the cell and listed) and make every metric of that cell a LOSE for rmw_tickle (docs/TESTING.md section 4).
Verdicts (rmw_tickle against the scored vendors, rmw_fastrtps_cpp and rmw_cyclonedds_cpp; rmw_zenoh_cpp is reference
only and never scored): per metric, over usable reps, docs/TESTING.md section 4's win rule - the same range rule
campaign_summary.py applies to the cross-host cells. WIN when rmw_tickle's reps' range lies entirely on the better
side of the range of every scored vendor that has 2 usable reps; LOSE when any such vendor's range lies entirely on
the better side of rmw_tickle's; otherwise (a range overlaps) DRAW. A cell where rmw_tickle has fewer than 2 usable
reps is VOID.
Until 2026-10-08 this was a 2 x combined-SE test on the means. It cannot call a lead a lead when one vendor's reps
scatter: tput Array1k best_effort CPU per sample at 03585237, rmw_tickle 13.14 / 13.73 / 13.17 us against
CycloneDDS 32.81-34.45 and FastDDS 171 / 1006 / 323, read DRAW because FastDDS's SE (about 256 us) put its 2 x SE
bound above the 487 us gap, although rmw_tickle was below every rep of both vendors. Range non-overlap does not
reward a vendor's scatter; a rmw_tickle range that touches a vendor's still reads DRAW.
"""
import math
import re
import statistics
import sys
from pathlib import Path

SCORED = ("fastdds", "cyclonedds")
# tickle_loans (ab_loans.sh): arm tickle with ROS_DISABLE_LOANED_MESSAGES=0 - printed per run, never in the table.
ARMS = ("tickle", "tickle_shipped", "fastdds", "cyclonedds", "zenoh")
RUN_ARMS = ("tickle", "tickle_loans", "tickle_shipped", "fastdds", "cyclonedds", "zenoh")
TICKLE_ARMS = ("tickle", "tickle_loans", "tickle_shipped")
ON_SEGMENT_ARMS = ("tickle", "tickle_loans")  # the arms that set the link, so must run on their segment
RMW = {"tickle": "rmw_tickle", "tickle_loans": "rmw_tickle", "tickle_shipped": "rmw_tickle",
       "fastdds": "rmw_fastrtps_cpp", "cyclonedds": "rmw_cyclonedds_cpp", "zenoh": "rmw_zenoh_cpp"}
# Serialized sample size, for the bytes witness only (a kernel-path sample is counted leaving and arriving on lo, so
# it costs at least twice this on the interfaces). Approximate on purpose: the bands below are wide.
# RadarDetection is ab_loans.sh's control cell (not loanable): uint16 + Point + Vector3 + float64 + int64 + uint64.
PAYLOAD = {"bench": 80, "array1k": 1036, "Array1k": 1040, "Array4k": 4112, "RadarDetection": 76}
WITNESS_MIN_PAYLOAD = 512   # below this, headers dominate and bytes cannot tell the transports apart
SHM_MAX, KERNEL_MIN = 0.25, 0.75
TRAFFIC_RE = re.compile(r"traffic: .*?\btx_udp=(\d+) tx_shm=(\d+) rx_udp=(\d+) rx_shm=(\d+)")


def params(runs):
    p = {}
    for line in (runs / "params.txt").read_text().splitlines():
        for k, v in re.findall(r"(\w+)=(\S+)", line):
            p[k] = v
    return p


def read_meta(d):
    m = {"maps": {}, "launch": {}, "lines": []}
    try:
        text = (d / "meta.txt").read_text()
    except OSError:
        return None
    for line in text.splitlines():
        m["lines"].append(line)
        if line.startswith("maps "):
            role = re.search(r"role=(\S+)", line).group(1)
            m["maps"][role] = [x for x in re.search(r"libs=(\S*)", line).group(1).split(",") if x]
        elif line.startswith("launch "):
            mm = re.search(r"role=(\S+) pid=\S* t_mono_ns=(\d+)", line)
            if mm:
                m["launch"][mm.group(1)] = int(mm.group(2))
        elif line.startswith(("net_before", "net_after")):
            key = line.split()[0]
            m[key] = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
        else:
            for k, v in re.findall(r"(\w+)=(\S+)", line):
                m.setdefault(k, v)
    return m


def identity(arm, maps, tickle_lib):
    """None when the process loaded the arm's rmw and nothing else, else why not."""
    if not maps:
        return "no maps read"
    impls = {re.sub(r".*/", "", x) for x in maps if re.search(r"librmw_(tickle|fastrtps_cpp|cyclonedds_cpp|zenoh_cpp)\.so$", x)}
    if len(impls) != 1:
        return f"{len(impls)} rmw implementations mapped: {','.join(sorted(impls)) or 'none'}"
    if arm in TICKLE_ARMS:
        return None if tickle_lib in maps else f"librmw_tickle.so not from {tickle_lib}: {','.join(maps)}"
    return None if impls == {f"lib{RMW[arm]}.so"} else f"mapped {impls.pop()}, not lib{RMW[arm]}.so"


def traffic(*logs):
    """(tx_shm, tx_udp, rx_shm, rx_udp) summed over the logs' rmw_tickle traffic lines, or None if any has none."""
    tot = [0, 0, 0, 0]
    for log in logs:
        try:
            found = TRAFFIC_RE.findall(log.read_text(errors="replace"))
        except OSError:
            return None
        if not found:
            return None
        for tu, ts, ru, rs in found:
            tot[0] += int(ts)
            tot[1] += int(tu)
            tot[2] += int(rs)
            tot[3] += int(ru)
    return tuple(tot)


def transport(ratio):
    if ratio is None:
        return "n/a"
    return "shm" if ratio <= SHM_MAX else "kernel" if ratio >= KERNEL_MIN else "mixed"


def tickle_witness(why, arm, msg, share, ratio):
    """rmw_tickle only: the two instruments must agree, and arm tickle must be on its segment (tickle_shipped is
    reported on whatever path it took)."""
    if arm not in TICKLE_ARMS:
        return
    if share is None:
        why.append("no rmw_tickle traffic line: could not look")
        return
    if share < 0.5 and arm in ON_SEGMENT_ARMS:
        why.append(f"rmw_tickle tx_shm share {share:.3f}: not on its segment, so not this cell")
    decidable = PAYLOAD[msg] >= WITNESS_MIN_PAYLOAD and ratio is not None
    if decidable and share >= 0.5 and ratio >= KERNEL_MIN:
        why.append(f"instruments disagree: tx_shm share {share:.3f} but bytes witness {ratio:.2f} (kernel)")
    if decidable and share < 0.5 and ratio <= SHM_MAX:
        why.append(f"instruments disagree: tx_shm share {share:.3f} but bytes witness {ratio:.2f} (shm)")


def sampler(d):
    rows = []
    try:
        for line in (d / "sampler.txt").read_text().splitlines():
            parts = line.split()
            if len(parts) < 2 or parts[0] != "S":
                continue
            rec = {"t": int(parts[1])}
            for f in parts[2:]:
                k, v = f.split("=", 1)
                if "/" in v:
                    cpu, hwm = v.split("/")
                    rec[k] = (int(cpu), int(hwm))
                else:
                    rec[k] = int(v)
            rows.append(rec)
    except OSError:
        pass
    return rows


def at(rows, t, key, role=None):
    """Linear interpolation of a counter at time t; None if t is not bracketed by readings that have it."""
    pts = [(r["t"], (r[role][0] if role else r[key])) for r in rows if (role in r if role else key in r)]
    before = [p for p in pts if p[0] <= t]
    after = [p for p in pts if p[0] >= t]
    if not before or not after:
        return None
    (t0, v0), (t1, v1) = before[-1], after[0]
    return v0 if t1 == t0 else v0 + (v1 - v0) * (t - t0) / (t1 - t0)


def pct(xs, q):
    s = sorted(xs)
    return s[min(len(s) - 1, int(len(s) * q))]


def check_rtt(d, m, p, arm, msg):
    why = []
    ping_log, pong_log = d / "ping.log", d / "pong.log"
    for role in ("ping", "pong"):
        bad = identity(arm, m["maps"].get(role), p["tickle_lib"])
        if bad:
            why.append(f"{role}: {bad}")
    text = ping_log.read_text(errors="replace") if ping_log.exists() else ""
    res = re.search(r"^RESULT: framework=(\S+) .*?sent=(\d+) recv=(\d+)", text, re.M)
    if not res or res.group(1) != RMW[arm]:
        why.append("no RESULT line naming " + RMW[arm])
        return why, None
    sent = int(res.group(2))
    warm, cool, need = int(p["warm_rt"]), int(p["cool_rt"]), int(p["min_measured"])
    stamps = {}
    try:
        for line in (d / "stamps.txt").read_text().splitlines():
            if line.startswith("#"):
                continue
            s, a, b = (int(x) for x in line.split())
            stamps[s] = (a, b)
    except OSError:
        why.append("no stamps")
        return why, None
    window = range(warm + 1, sent - cool + 1)
    if len(window) < need:
        why.append(f"{max(len(window), 0)} round trips in the window, fewer than {need} (sent {sent})")
        return why, None
    got = [stamps[s] for s in window if s in stamps and stamps[s][1] > 0]
    if len(got) != len(window):
        why.append(f"{len(window) - len(got)} of {len(window)} round trips in the window never came back")
        return why, None
    rtt_us = [(b - a) / 1e3 for a, b in got]
    t0, t1 = got[0][0], got[-1][1]
    rows = sampler(d)
    roles = ["ping", "pong"] + (["router"] if arm == "zenoh" else [])
    cpu = 0.0
    for role in roles:
        c0, c1 = at(rows, t0, None, role), at(rows, t1, None, role)
        if c0 is None or c1 is None:
            why.append(f"no sampler reading of {role} on both sides of the window")
            return why, None
        cpu += c1 - c0
    b0, b1 = at(rows, t0, "all_bytes"), at(rows, t1, "all_bytes")
    k0, k1 = at(rows, t0, "all_pkts"), at(rows, t1, "all_pkts")
    n = len(got)
    bytes_per_sample = (b1 - b0) / (2 * n) if b0 is not None and b1 is not None else None
    pkts_per_sample = (k1 - k0) / (2 * n) if k0 is not None and k1 is not None else None
    ratio = bytes_per_sample / (2 * PAYLOAD[msg]) if bytes_per_sample is not None else None
    hwm = {role: max((r[role][1] for r in rows if role in r), default=0) for role in roles}
    tr = traffic(ping_log, pong_log) if arm in TICKLE_ARMS else None
    share = tr[0] / (tr[0] + tr[1]) if tr and tr[0] + tr[1] else (0.0 if tr else None)
    tickle_witness(why, arm, msg, share, ratio)
    if arm == "zenoh" and m.get("router") != "listening":
        why.append("zenoh router was not listening")
    rec = dict(n=n, p50=pct(rtt_us, 0.50), p99=pct(rtt_us, 0.99), mean=statistics.mean(rtt_us),
               cpu=cpu / n / 1e3, rss=max(hwm["ping"], hwm["pong"]), rss_ping=hwm["ping"], rss_pong=hwm["pong"],
               rss_router=hwm.get("router"), bps=bytes_per_sample, pps=pkts_per_sample, ratio=ratio,
               transport=transport(ratio) if PAYLOAD[msg] >= WITNESS_MIN_PAYLOAD else "n/a(64B)", share=share)
    return why, rec


def perf_rows(path):
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return [], ""
    lines = text.splitlines()
    try:
        start = lines.index("---EXPERIMENT-START---")
    except ValueError:
        return [], text
    header = [h.strip() for h in lines[start + 1].split(",") if h.strip()]
    out = []
    for line in lines[start + 2:]:
        cells = [c.strip() for c in line.split(",") if c.strip()]
        if len(cells) < len(header) or not re.match(r"^[0-9.]+$", cells[0]):
            continue
        try:
            out.append({h: float(c) for h, c in zip(header, cells)})
        except ValueError:
            continue
    return out, text


def windowed(rows, offset, lo, hi):
    """The rows covering SENDER time (lo, hi] - a row's end, T_experiment - offset, in (lo + 0.5, hi + 0.5] - and the
    cumulative CPU (s) at the window's start and end. offset is how long after this process the publisher was
    launched (0 for the publisher itself), so publisher and subscriber are windowed on the same sender seconds
    (docs/TESTING.md section 5 rule 9), and a subscriber that stalls while the publisher sends reads as a low rate,
    not as a shorter window."""
    idx = [i for i, r in enumerate(rows) if lo + 0.5 < r["T_experiment"] - offset <= hi + 0.5]
    if len(idx) < 3:
        return [], None
    first, last = idx[0], idx[-1]
    cpu = lambda r: r["ru_utime"] + r["ru_stime"]  # noqa: E731
    return rows[first:last + 1], (cpu(rows[first - 1]) if first > 0 else 0.0, cpu(rows[last]))


def check_tput(d, m, p, arm, msg, qos):
    why = []
    sub, stext = perf_rows(d / "sub.log")
    pub, ptext = perf_rows(d / "pub.log")
    want_qos = ("Reliability: RELIABLE Durability: VOLATILE History kind: KEEP_ALL History depth: 1000"
                if qos == "reliable" else
                "Reliability: BEST_EFFORT Durability: VOLATILE History kind: KEEP_LAST History depth: 1")
    for role, text in (("sub", stext), ("pub", ptext)):
        if want_qos not in text:
            why.append(f"{role}: QoS echo is not '{want_qos}'")
    bad = identity(arm, m["maps"].get("sub"), p["tickle_lib"])
    if bad:
        why.append(f"sub: {bad}")
    bad = identity(arm, m["maps"].get("pub"), p["tickle_lib"])
    hdr = re.search(r"RMW Implementation: (\S+)", ptext)
    if bad and not (arm not in TICKLE_ARMS and hdr and hdr.group(1) == RMW[arm] and m["maps"].get("pub") is None):
        why.append(f"pub: {bad}")
    refused = "failed to publish" in ptext
    warm, cool, dur = int(p["warm_s"]), int(p["cool_s"]), int(m["dur"])
    t_sub, t_pub = m["launch"].get("sub"), m["launch"].get("pub")
    if t_sub is None or t_pub is None:
        why.append("no launch time for the publisher or the subscriber")
        return why, None
    swin, scpu = windowed(sub, (t_pub - t_sub) / 1e9, warm, dur - cool)
    if not swin:
        why.append("the subscriber's log has fewer than 3 rows in the sender window")
        return why, None
    pwin, pcpu = windowed(pub, 0.0, warm, dur - cool)
    rate = sum(r["received"] for r in swin) / sum(r["T_loop"] for r in swin)
    sent_rate = (sum(r["sent"] for r in pwin) / sum(r["T_loop"] for r in pwin)) if pwin else float("nan")
    sub_cpu_s = (scpu[1] - scpu[0]) / sum(r["T_loop"] for r in swin)
    pub_cpu_s = ((pcpu[1] - pcpu[0]) / sum(r["T_loop"] for r in pwin)) if pwin else float("nan")
    cpu = (sub_cpu_s + pub_cpu_s) / rate * 1e6 if rate else float("nan")
    # Loss after the first delivered second (the pre-match gap is not loss; rmw_keepall_rig_summary.py).
    first = next((i for i, r in enumerate(sub) if r.get("received", 0) > 0), None)
    lost = round(sum(r.get("lost", 0) for r in sub[first + 1:])) if first is not None else 0
    recv = sum(r.get("received", 0) for r in sub)
    # The bytes witness is per sample PUBLISHED, not per sample received. Until 2026-10-09 it divided by the samples
    # perf_test received, and at BEST_EFFORT KEEP_LAST 1 the subscriber takes as little as 0.2% of what is sent, so
    # the 0.9% of datagrams rmw_tickle broadcasts before its peer is known (tx_udp 162,934 of 17,791,221) read as 0.90
    # payloads per sample - "kernel" against a tx_shm share of 0.991, a VOID with both instruments right. A datagram
    # crosses (or does not cross) the kernel whether or not the reader keeps it, so the denominator is what was sent.
    # max(): perf_test may not print the publisher's last partial second, so a RELIABLE run's sent can trail recv.
    sent = sum(r.get("sent", 0) for r in pub)
    nsamp = max(sent, recv)
    nb, na = m.get("net_before"), m.get("net_after")
    bps = (na["all_bytes"] - nb["all_bytes"]) / nsamp if nb and na and nsamp else None
    pps = (na["all_pkts"] - nb["all_pkts"]) / nsamp if nb and na and nsamp else None
    ratio = bps / (2 * PAYLOAD[msg]) if bps is not None else None
    router_cpu = None
    if arm == "zenoh":
        if m.get("router") != "listening":
            why.append("zenoh router was not listening")
        rows = sampler(d)
        t0 = m["launch"].get("pub")
        last = [r["t"] for r in rows if "pub" in r]
        if t0 and last:
            c0, c1 = at(rows, t0, None, "router"), at(rows, last[-1], None, "router")
            if c0 is not None and c1 is not None and recv:
                router_cpu = (c1 - c0) / recv / 1e3
    tr = traffic(d / "pub.log") if arm in TICKLE_ARMS else None
    share = tr[0] / (tr[0] + tr[1]) if tr and tr[0] + tr[1] else (0.0 if tr else None)
    tickle_witness(why, arm, msg, share, ratio)
    rss_pub = pub[-1]["ru_maxrss"] if pub else 0
    rss_sub = sub[-1]["ru_maxrss"] if sub else 0
    rec = dict(rate=rate, sent_rate=sent_rate, cpu=cpu + (router_cpu or 0.0), cpu_router=router_cpu,
               rss=max(rss_pub, rss_sub), rss_pub=rss_pub, rss_sub=rss_sub, lost=lost, refused=refused,
               seconds=len(swin), bps=bps, pps=pps, ratio=ratio, transport=transport(ratio), share=share,
               lat=statistics.mean([r["latency_mean (ms)"] for r in swin if r["received"] > 0] or [float("nan")]))
    return why, rec


def verdict(cell, metric, lower_better, tickle_lose_all):
    """docs/TESTING.md section 4's win rule on the reps' ranges (module docstring)."""
    t = [r[metric] for r in cell.get("tickle", []) if r.get(metric) is not None]
    if len(t) < 2:
        return "VOID"
    if tickle_lose_all:
        return "LOSE(tickle delivery)"
    t_lo, t_hi = min(t), max(t)
    better_all, worse_any, compared = True, False, 0
    for v in SCORED:
        xs = [r[metric] for r in cell.get(v, []) if r.get(metric) is not None]
        if len(xs) < 2:
            continue
        compared += 1
        v_lo, v_hi = min(xs), max(xs)
        tickle_clear = t_hi < v_lo if lower_better else t_lo > v_hi    # every tickle rep better than every vendor rep
        vendor_clear = v_hi < t_lo if lower_better else v_lo > t_hi
        if not tickle_clear:
            better_all = False
        if vendor_clear:
            worse_any = True
    if compared == 0:
        return "NO VENDOR"
    return "LOSE" if worse_any else "WIN" if better_all else "DRAW"


def fmt(v, metric):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "-"
    if metric in ("rate", "rss"):
        return f"{v:,.0f}"
    return f"{v:.1f}" if metric in ("p50", "p99", "mean") else f"{v:.2f}"


def main():
    runs = Path(sys.argv[1])
    preflight = "--preflight" in sys.argv[2:]
    p = params(runs)
    cells, voids = {}, []
    for d in sorted(x for x in runs.iterdir() if x.is_dir()):
        m = read_meta(d)
        if m is None:
            voids.append(f"{d.name}: no meta.txt")
            continue
        arm, kind, msg, qos, wait = m.get("arm"), m.get("kind"), m.get("msg"), m.get("qos"), m.get("wait")
        rep = re.search(r"_r(\d+)$", d.name)
        why = []
        if m.get("done") != "1":
            why.append("cell script did not finish")
        if m.get("leftovers", "0") != "0":
            why.append(f"{m['leftovers']} leftover process(es) before the run")
        if kind == "rtt":
            w2, rec = check_rtt(d, m, p, arm, msg)
            key = ("rtt", msg, qos, wait)
        else:
            w2, rec = check_tput(d, m, p, arm, msg, qos)
            key = ("tput", msg, qos, "-")
        why += w2
        if why or rec is None:
            voids.append(f"{d.name}: " + "; ".join(why or ["no record"]))
            continue
        rec["rep"] = int(rep.group(1)) if rep else 0
        rec["roudi"] = m.get("roudi_procs")
        cells.setdefault(key, {}).setdefault(arm, []).append(rec)

    print(f"=== rmw same-host, {p.get('host', '?')}: window rtt {p['warm_rt']}+{p['cool_rt']} round trips "
          f"excluded, tput {p['warm_s']}+{p['cool_s']} s excluded; min measured {p['min_measured']} ===")
    print("\n--- per run ---")
    for key, arms in sorted(cells.items()):
        for arm in RUN_ARMS:
            for r in sorted(arms.get(arm, []), key=lambda r: r["rep"]):
                share = "-" if r["share"] is None else f"{r['share']:.3f}"
                wit = (f"witness {r['transport']} (bytes/sample {r['bps']:.0f}, pkts/sample {r['pps']:.2f}, "
                       f"ratio {r['ratio']:.2f}) tx_shm share {share} roudi {r['roudi']}"
                       if r["bps"] is not None else "witness: no counters")
                if key[0] == "rtt":
                    print(f"{'/'.join(key)} {arm:10} r{r['rep']} n={r['n']} p50 {r['p50']:.1f} p99 {r['p99']:.1f} "
                          f"mean {r['mean']:.1f} us  cpu {r['cpu']:.2f} us/rt  rss ping/pong {r['rss_ping']}/"
                          f"{r['rss_pong']}{'/router ' + str(r['rss_router']) if r['rss_router'] else ''} kB  {wit}")
                else:
                    print(f"{'/'.join(key[:3])} {arm:10} r{r['rep']} delivered {r['rate']:,.0f}/s (sent "
                          f"{r['sent_rate']:,.0f}/s) over {r['seconds']} s  lost {r['lost']}  cpu {r['cpu']:.2f} us/"
                          f"sample{' (router ' + format(r['cpu_router'], '.2f') + ')' if r['cpu_router'] else ''}"
                          f"  rss pub/sub {r['rss_pub']:.0f}/{r['rss_sub']:.0f} kB  lat {r['lat']:.3f} ms (lat_us {1000 * r['lat']:.3f}) "
                          f"{'REFUSED ' if r['refused'] else ''}{wit}")
    if voids:
        print("\nVOID runs:")
        for v in voids:
            print("  " + v)

    if preflight:
        n = sum(len(x) for a in cells.values() for x in a.values())
        print(f"\nPREFLIGHT {'PASS' if not voids and n else 'FAIL'}: {n} usable runs, {len(voids)} VOID")
        sys.exit(0 if not voids and n else 1)

    print("\n--- table (medians over usable reps; verdict: rmw_tickle (link set) vs FastDDS and CycloneDDS, reps' "
          "ranges must not overlap; rmw_tickle as shipped and zenoh are printed, not scored) ---")
    print("| cell | metric | rmw_tickle | rmw_tickle shipped | rmw_fastrtps_cpp | rmw_cyclonedds_cpp | "
          "rmw_zenoh_cpp (ref) | verdict |")
    print("|---|---|---|---|---|---|---|---|")
    for key, arms in sorted(cells.items()):
        excluded = []
        tickle_lose = False
        if key[0] == "tput":
            for arm in list(arms):
                recs = arms[arm]
                if key[2] == "reliable":
                    failed = [r for r in recs if r["lost"] > 0 and not r["refused"]]
                    if failed and arm == "tickle":
                        tickle_lose = True
                        excluded.append(f"tickle LOST samples in {len(failed)} rep(s): every metric is a LOSE")
                    elif failed:
                        excluded.append(f"{arm} DELIVERY FAILED in {len(failed)} rep(s)")
                        arms[arm] = [r for r in recs if r not in failed]
                    refused = [r for r in arms[arm] if r["refused"]]
                    if refused:
                        excluded.append(f"{arm} REFUSED in {len(refused)} rep(s)")
                        arms[arm] = [r for r in arms[arm] if not r["refused"]]
        metrics = ([("p50", True, "RTT p50 us"), ("p99", True, "RTT p99 us"), ("mean", True, "RTT mean us"),
                    ("cpu", True, "CPU us per round trip"), ("rss", True, "peak RSS kB (larger of ping, pong)")]
                   if key[0] == "rtt" else
                   [("rate", False, "delivered msg/s"), ("cpu", True, "CPU us per delivered sample (pub+sub)"),
                    ("rss", True, "peak RSS kB (larger of pub, sub)")])
        label = " ".join(k for k in key if k != "-")
        for metric, lower, name in metrics:
            vals = []
            for arm in ARMS:
                xs = [r[metric] for r in arms.get(arm, []) if r.get(metric) is not None]
                med = statistics.median(xs) if xs else None
                tag = f" (n={len(xs)}, {arms[arm][0]['transport']})" if xs else ""
                vals.append(fmt(med, metric) + tag)
            print(f"| {label} | {name} | " + " | ".join(vals) + f" | {verdict(arms, metric, lower, tickle_lose)} |")
        for e in excluded:
            print(f"|  | {e} | | | | | | |")


if __name__ == "__main__":
    main()
