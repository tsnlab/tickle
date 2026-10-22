#!/usr/bin/env python3
# Copyright (c) 2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.
"""Reads a wifi_burst_loss_netns.sh output directory and applies its pre-registered rules.

The rules, their thresholds and their reasons are in wifi_burst_loss_netns.sh's header; this is the code that
applies them, so a criterion is not only a comment. Usage: wifi_burst_read.py OUT_DIR. Exit 0 always once the
directory is read (the verdicts are in the text); 2 when it cannot be read.
"""

import math
import os
import re
import statistics
import sys

KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")
DECISION_LOSS = {"ge5b2": (5.0, 2.0), "ge5b4": (5.0, 4.0)}
LARGE_ARMS = ("ge5b4", "ge10b4")
SMALL_MIN_REP = 2000
# Pooled latencies a small decision cell needs: two reps the PC did not load. NOT pre-registered - added after the first
# baseline (2026-10-22), whose reps other sessions' builds loaded past nproc/2; the header fixed only the 2000 a rep.
POOLED_MIN = 2 * SMALL_MIN_REP
LARGE_MIN_REP = 100
PERIOD_MS = 10.0  # one sample period at 100 Hz: detection by the next sample
ROUND_FACTOR = 1.5  # a repair round: a round trip and half a round trip of retry timer
P99_TARGET = 0.01
REFRESH_NS = 10_000_000  # 10 ms


def kv(line):
    return {k: v for k, v in KV.findall(line)}


def num(d, key, default=0.0):
    try:
        return float(d.get(key, default))
    except ValueError:
        return default


def read_lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def result_of(lines, role_prefix):
    for line in lines:
        if line.startswith(role_prefix):
            return kv(line)
    return None


def counters_of(path):
    """Per node label: veth tx_bytes and, per netem handle (10 broadcast, 20 unicast), sent packets and drops."""
    nodes = {}
    label = None
    handle = None
    for line in read_lines(path):
        if line.startswith("== "):
            label = line.split()[1]
            d = kv(line)
            nodes[label] = {"tx_bytes": int(d.get("tx_bytes", 0)), "tx_packets": int(d.get("tx_packets", 0)),
                            "bands": {}}
            handle = None
        elif line.startswith("qdisc netem") and label is not None:
            handle = line.split()[2].rstrip(":")
        elif line.strip().startswith("Sent") and label is not None and handle is not None:
            m = re.search(r"Sent (\d+) bytes (\d+) pkt \(dropped (\d+)", line)
            if m:
                nodes[label]["bands"][handle] = (int(m.group(2)), int(m.group(3)))
            handle = None
    return nodes


def quantile(sorted_values, q):
    if not sorted_values:
        return float("nan")
    i = min(len(sorted_values) - 1, int(len(sorted_values) * q))
    return sorted_values[i]


WIFI_MISS_THRESHOLD = 6  # tt_LINK_WIFI_MISS_THRESHOLD; the default is 3
DEFAULT_MISS_THRESHOLD = 3


def treatment(r, profile):
    """The discovery each node ran, against the profile the run names (header, AMENDED 2026-10-23): (problems, totals).
    A RESULT line without the fields (a run before the amendment) is a problem only for a wifi run."""
    problems = []
    totals = {"copies": 0, "repeats": 0, "copies_idle": 0, "repeats_ping": 0}
    for role in ("ping", "echo", "idle"):
        res = r[role]
        if res is None:
            continue
        if "miss_threshold" not in res:
            if profile == "wifi":
                problems.append(f"{role} reports no discovery treatment")
            continue
        miss, copies, repeats = int(num(res, "miss_threshold")), int(num(res, "summary_copies")), int(
            num(res, "announce_repeats"))
        totals["copies"] += copies
        totals["repeats"] += repeats
        totals["copies_idle"] += copies if role == "idle" else 0
        totals["repeats_ping"] += repeats if role == "ping" else 0
        if profile == "wifi" and miss != WIFI_MISS_THRESHOLD:
            problems.append(f"{role} ran miss_threshold {miss}, not WIFI's {WIFI_MISS_THRESHOLD}")
        if profile != "wifi" and (miss != DEFAULT_MISS_THRESHOLD or copies or repeats):
            problems.append(f"{role} ran miss_threshold {miss}, {copies} copies, {repeats} repeats on ethernet")
    if profile == "wifi" and r["idle"] is not None and "miss_threshold" in r["idle"] and totals["copies_idle"] == 0:
        problems.append("the idle node sent no summary copy")
    if profile == "wifi" and r["ping"] is not None and "miss_threshold" in r["ping"] and totals["repeats_ping"] == 0:
        problems.append("ping sent no announce repeat")
    return problems, totals


def load_runs(out, profile="ethernet"):
    runs = []
    nproc = os.cpu_count() or 1
    for name in sorted(os.listdir(os.path.join(out, "runs"))):
        parts = name.rsplit("-", 2)
        if len(parts) != 3:
            continue
        arm, cell, rep = parts
        d = os.path.join(out, "runs", name)
        r = {"arm": arm, "cell": cell, "rep": rep, "dir": d, "void": []}
        ping = read_lines(os.path.join(d, "ping.log"))
        echo = read_lines(os.path.join(d, "echo.log"))
        idle = read_lines(os.path.join(d, "idle.log"))
        meta = kv(" ".join(read_lines(os.path.join(d, "meta.txt"))))
        r["loaded"] = num(meta, "load") > nproc / 2
        r["idle"] = result_of(idle, "RESULT role=idle")
        r["events"] = [kv(x) for x in ping + echo + idle if x.startswith("EVENT")]
        r["ends"] = {}
        r["counters"] = counters_of(os.path.join(d, "counters.txt"))
        if cell == "rel_large":
            r["ping"] = result_of(ping, "RESULT: role=pub")
            r["echo"] = result_of(echo, "RESULT: role=sub")
        else:
            r["ping"] = result_of(ping, "RESULT role=ping")
            r["echo"] = result_of(echo, "RESULT role=echo")
            r["owd"] = sorted(int(x) for x in read_lines(os.path.join(d, "owd_us.txt")) if x.strip().isdigit())
            r["rtt"] = sorted(int(x) for x in read_lines(os.path.join(d, "rtt_us.txt")) if x.strip().isdigit())
            r["rstats"] = {}
            for line in ping + echo:
                if line.startswith("RSTATS:"):
                    d2 = kv(line)
                    for k, v in d2.items():
                        r["rstats"][d2.get("role", "?") + "." + k] = v
        for res in (r["ping"], r["echo"], r["idle"]):
            if res is not None and "ctx" in res and "end_ns" in res:
                r["ends"][res["ctx"]] = int(res["end_ns"])
        if r["ping"] is None or r["echo"] is None or r["idle"] is None:
            r["void"].append("a RESULT line is missing")
        r["treat_void"], r["treat"] = treatment(r, profile)
        runs.append(r)
    return runs


def false_deaths(r, dur):
    """Departures seen before the departed peer's own end: no peer leaves before it ends."""
    count = idle = 0
    # A departure answered by the same peer's re-appearance within REFRESH_NS, seen by the same node, is the
    # discovery table replacing an entry (seen in the first smoke run, 2 us apart), not a liveliness verdict.
    refreshed = set()
    for ev in r["events"]:
        if ev.get("kind") == "reappear":
            refreshed.add((ev.get("role_ctx"), ev.get("peer"), int(ev.get("t_ns", 0))))
    for ev in r["events"]:
        if ev.get("kind") != "depart":
            continue
        peer = ev.get("peer")
        t_ns = int(ev.get("t_ns", 0))
        if any(rc == ev.get("role_ctx") and p == peer and 0 <= t - t_ns <= REFRESH_NS for rc, p, t in refreshed):
            continue
        end = r["ends"].get(peer)
        if end is None and r["idle"] is not None:
            # rel_large: the large bench prints no end_ns; its pub ends ~1 + DUR + 5 s after the idle node starts
            end = int(r["idle"]["create_ns"]) + int((dur + 5.5) * 1e9)
        if end is not None and t_ns < end:
            count += 1
            # "idle" here: a death in a pair that exchanges no data (the idle node seen or seeing), whose liveliness
            # rests on broadcast summaries alone; the rest are ping and echo, which carry data to each other.
            idle += 1 if "3" in (peer, ev.get("role_ctx")) else 0
    return count, idle


def per_run(r, dur):
    p, e = r["ping"], r["echo"]
    m = {}
    if p is None or e is None:
        return None
    c = r["counters"]
    fwd = c.get("p", {}).get("tx_bytes", 0)
    rev = c.get("e", {}).get("tx_bytes", 0)
    if r["cell"] == "rel_large":
        published = int(num(p, "published"))
        offered = max(0, published - int(num(e, "first_index")))
        delivered = int(num(e, "delivered"))
        m.update(offered=offered, delivered=delivered, p50=num(e, "latency_median_us"), p99=num(e, "latency_p99_us"),
                 retx=int(num(p, "retransmitted")), cpu_ms=num(p, "cpu_ms") + num(e, "cpu_ms"),
                 dups=int(num(e, "large_duplicate")), join_ms=float("nan"))
        if offered < LARGE_MIN_REP:
            r["void"].append(f"{offered} samples offered, under {LARGE_MIN_REP}")
    else:
        published = int(num(p, "published"))
        offered = max(0, published - int(num(p, "warmup")))
        delivered = int(num(e, "scored"))
        m.update(offered=offered, delivered=delivered, pongs=int(num(p, "scored")), retx=int(num(p, "retransmitted")),
                 cpu_ms=num(p, "cpu_ms") + num(e, "cpu_ms"), join_ms=num(p, "join_ms"))
        # Copies that reached the receiver after the sample (core's own count, RSTATS): the echo's decode never sees
        # them - a RELIABLE reader drops a copy below its acknowledged sequence before decoding - so the node's own
        # "duplicates" is always 0 for RELIABLE and cannot decide R7 (found on the first baseline, 2026-10-22).
        rs = r.get("rstats", {})
        m["dups"] = int(num(rs, "echo.late_below_ack")) + int(num(rs, "echo.duplicates")) + int(num(e, "duplicates"))
        if published == 0:
            r["void"].append("the ping Publisher never matched")
        if offered < SMALL_MIN_REP:
            r["void"].append(f"{offered} samples offered, under {SMALL_MIN_REP}")
    m["loaded"] = r["loaded"]
    m["fwd_bytes"] = fwd
    m["rev_bytes"] = rev
    m["false_deaths"], m["idle_deaths"] = false_deaths(r, dur)
    m["idle_join_ms"] = idle_join_ms(r)
    bands = c.get("p", {}).get("bands", {})
    # Per skb, as netem drops: a GSO skb is one drop but its gso_segs in tc's "Sent ... pkt", so the unicast band's
    # skbs are the veth's tx_packets less the broadcast band's (never GSO) - not tc's packet count.
    u_drop = bands.get("20", (0, 0))[1]
    b10 = bands.get("10", (0, 0))[0]
    m["u_sent"], m["u_drop"] = max(0, c.get("p", {}).get("tx_packets", 0) - b10), u_drop
    m["u_segs"] = bands.get("20", (0, 0))[0] + u_drop
    b_sent = b_drop = all_drop = 0
    for node in c.values():
        s, d = node["bands"].get("10", (0, 0))
        b_sent += s
        b_drop += d
        all_drop += sum(x[1] for x in node["bands"].values())
    m["b_sent"], m["b_drop"], m["all_drop"] = b_sent, b_drop, all_drop
    return m


def idle_join_ms(r):
    """The idle node's first sight of the ping context, from the ping node's own create time (small cells)."""
    p = r["ping"]
    if p is None or "create_ns" not in p:
        return float("nan")
    for ev in r["events"]:
        if ev.get("role_ctx") == "3" and ev.get("kind") == "appear" and ev.get("peer") == "1":
            return (int(ev["t_ns"]) - int(p["create_ns"])) / 1e6
    return float("nan")


def pool(runs, dur):
    cells = {}
    for r in runs:
        m = per_run(r, dur)
        key = (r["arm"], r["cell"])
        c = cells.setdefault(key, {"reps": 0, "void": [], "m": [], "owd": [], "rtt": [], "owd_all": [], "lat_void": 0,
                                   "treat_void": [], "copies": 0, "repeats": 0})
        c["reps"] += 1
        c["void"] += [f"rep {r['rep']}: {v}" for v in r["void"]]
        c["treat_void"] += [f"rep {r['rep']}: {v}" for v in r.get("treat_void", [])]
        c["copies"] += r.get("treat", {}).get("copies", 0)
        c["repeats"] += r.get("treat", {}).get("repeats", 0)
        if m is None:
            continue
        c["m"].append(m)
        c["owd_all"] += r.get("owd", [])
        if r["loaded"]:
            c["lat_void"] += 1
        elif r["cell"] != "rel_large":
            c["owd"] += r.get("owd", [])
            c["rtt"] += r.get("rtt", [])
    for c in cells.values():
        c["owd"].sort()
        c["owd_all"].sort()
        c["rtt"].sort()
        ms = c["m"]

        def tot(k, ms=ms):
            return sum(x[k] for x in ms)

        c["offered"] = tot("offered") if ms else 0
        c["delivered"] = tot("delivered") if ms else 0
        c["goodput"] = c["delivered"] / c["offered"] if c["offered"] else float("nan")
        c["retx"] = tot("retx") if ms else 0
        c["dups"] = tot("dups") if ms else 0
        c["fwd"] = tot("fwd_bytes") if ms else 0
        c["rev"] = tot("rev_bytes") if ms else 0
        c["cpu_ms"] = tot("cpu_ms") if ms else 0.0
        c["false_deaths"] = tot("false_deaths") if ms else 0
        c["idle_deaths"] = tot("idle_deaths") if ms else 0
        joins = [x["join_ms"] for x in ms if not math.isnan(x["join_ms"])]
        c["join"] = sorted(joins)
        c["idle_join"] = sorted(x["idle_join_ms"] for x in ms if not math.isnan(x["idle_join_ms"]))
        us, ud = (tot("u_sent"), tot("u_drop")) if ms else (0, 0)
        bs, bd = (tot("b_sent"), tot("b_drop")) if ms else (0, 0)
        c["u_loss"] = ud / (us + ud) if us + ud else float("nan")
        c["b_loss"] = bd / (bs + bd) if bs + bd else float("nan")
        c["all_drop"] = tot("all_drop") if ms else 0
        # Latency from reps the PC did not load (the header's VOID rule), as for the small cells' pooled lists.
        c["p50s"] = [x.get("p50") for x in ms if "p50" in x and not x["loaded"]]
        c["p99s"] = [x.get("p99") for x in ms if "p99" in x and not x["loaded"]]
        c["u_drops"] = ud
        # Frames per skb on ping's unicast band: tc counts a multi-frame (GSO) skb's segments, the veth its skbs.
        c["segs_per_skb"] = tot("u_segs") / (us + ud) if ms and us + ud else float("nan")
    return cells


def fmt(x, nd=1):
    return "nan" if x is None or (isinstance(x, float) and math.isnan(x)) else f"{x:.{nd}f}"


def lat_ms(c, kind, q):
    if c.get("lat_void_all"):
        return float("nan")
    if kind in ("owd", "rtt"):
        return quantile(c[kind], q) / 1000.0
    vals = c["p50s"] if q == 0.5 else c["p99s"]
    if not vals:
        return float("nan")
    return (statistics.median(vals) if q == 0.5 else max(vals)) / 1000.0


def table(cells, cell_names, arm_names):
    print("cell       arm     n_off  deliv%   owd_p50 owd_p99 rtt_p50 rtt_p99 (ms)  dups  retx/off retx/loss "
          "fwdB/deliv rev/fwd%  cpu_us/deliv  join_p50/max(ms) idle_join_max  false_deaths(no-data pair)  loss_u%  loss_b%")
    for cell in cell_names:
        for arm in arm_names:
            c = cells.get((arm, cell))
            if c is None:
                continue
            c["lat_void_all"] = c["lat_void"] == c["reps"] and c["reps"] > 0
            d = c["delivered"] or 1
            j = c["join"]
            ij = c["idle_join"]
            print(f"{cell:10} {arm:7} {c['offered']:6d} {fmt(100 * c['goodput'], 2):>7} "
                  f"{fmt(lat_ms(c, 'owd' if cell != 'rel_large' else 'large', 0.5), 2):>8} "
                  f"{fmt(lat_ms(c, 'owd' if cell != 'rel_large' else 'large', 0.99), 2):>7} "
                  f"{fmt(lat_ms(c, 'rtt', 0.5) if cell != 'rel_large' else float('nan'), 2):>7} "
                  f"{fmt(lat_ms(c, 'rtt', 0.99) if cell != 'rel_large' else float('nan'), 2):>7}      "
                  f"{c['dups']:5d} {fmt(c['retx'] / c['offered'] if c['offered'] else float('nan'), 4):>9} "
                  f"{fmt(c['retx'] / c['u_drops'] if c['u_drops'] else float('nan'), 2):>9} "
                  f"{fmt(c['fwd'] / d, 0):>10} {fmt(100 * c['rev'] / c['fwd'] if c['fwd'] else float('nan'), 2):>8} "
                  f"{fmt(1000 * c['cpu_ms'] / d, 1):>12}  "
                  f"{fmt(statistics.median(j) if j else float('nan'), 0):>7}/{fmt(max(j) if j else float('nan'), 0):<7} "
                  f"{fmt(max(ij) if ij else float('nan'), 0):>12}  {c['false_deaths']:5d} (nodata {c['idle_deaths']:2d})  "
                  f"{fmt(100 * c['u_loss'], 2):>7}  {fmt(100 * c['b_loss'], 2):>7}"
                  f"{'  LATENCY-VOID(load)' if c['lat_void_all'] else ''}")


def arm_loss(arm):
    m = re.fullmatch(r"ge(\d+)b(\d+)", arm)
    return (float(m.group(1)), float(m.group(2))) if m else None


def validity(cells):
    """VOID conditions of the header, per (arm, cell); the control's failure voids everything."""
    void = {k: list(c["void"]) for k, c in cells.items()}
    campaign_void = []
    for cell in ("be_small", "rel_small", "rel_large"):
        c = cells.get(("ctl", cell))
        if c is None:
            continue
        need = 0.999 if cell == "be_small" else 1.0
        if c["offered"] and c["goodput"] < need:
            campaign_void.append(f"ctl {cell} delivered {100 * c['goodput']:.3f}% < {100 * need:.1f}%")
        if c["all_drop"]:
            campaign_void.append(f"ctl {cell}: {c['all_drop']} tc drops with no loss configured")
    for (arm, cell), c in cells.items():
        lb = arm_loss(arm)
        if lb is not None and not math.isnan(c["u_loss"]):
            lo, hi = 0.5 * lb[0] / 100, 1.5 * lb[0] / 100
            if not lo <= c["u_loss"] <= hi:
                void[(arm, cell)].append(f"realized unicast loss {100 * c['u_loss']:.2f}% outside [{100 * lo:.2f}, "
                                         f"{100 * hi:.2f}]%: the arm did not apply its treatment")
        if arm == "bc" and not math.isnan(c["b_loss"]) and not 0.15 <= c["b_loss"] <= 0.45:
            void[(arm, cell)].append(f"realized broadcast loss {100 * c['b_loss']:.1f}% outside [15, 45]%")
        if cell == "be_small" and lb is not None and c["offered"] and not math.isnan(c["u_loss"]):
            be_loss = 1 - c["goodput"]
            tol = max(0.01, 0.3 * lb[0] / 100)
            if abs(be_loss - c["u_loss"]) > tol:
                void[(arm, cell)].append(f"be delivered loss {100 * be_loss:.2f}% vs realized {100 * c['u_loss']:.2f}%"
                                         f": the instruments disagree by more than {100 * tol:.1f} points")
        # Post hoc (2026-10-23): netem's slot ("packets 42") and its loss act per skb. Where the sender's skbs carry
        # several frames (UDP GSO: large samples in MTU units), the airtime model and the loss are not per frame, and
        # the arm is not the treatment it names - the base run's lossy large arms (MTU units, GSO) delivered 1 MiB in
        # ~5 ms at p50 against the control's 47 ms (IP-fragment runs, one frame an skb). Off-PC Wi-Fi sends every
        # frame on its own, so such an arm is VOID; TT_UDP_OFFLOAD=0 (NODE_ENV) makes every frame an skb.
        if not math.isnan(c.get("segs_per_skb", float("nan"))) and c["segs_per_skb"] > 1.05:
            void[(arm, cell)].append(f"{c['segs_per_skb']:.2f} frames per skb on the sender's unicast band: slot and "
                                     "loss act per skb, not per frame (run with NODE_ENV=TT_UDP_OFFLOAD=0)")
    return void, campaign_void


def rounds_needed(loss_pct, burst):
    f = (1 - 1 / burst) ** 2
    lp = loss_pct / 100
    k = 1
    while lp * f**k > P99_TARGET and k < 20:
        k += 1
    return k


def rules(cells, void, campaign_void, treat_void=()):
    out = []

    def cell_ok(arm, cell):
        return (arm, cell) in cells and not void.get((arm, cell)) and not campaign_void

    ctl = cells.get(("ctl", "rel_small"))
    if ctl is None or not ctl["owd"] or not ctl["rtt"]:
        out.append(("ALL", "VOID", "no ctl rel_small latencies: no base_rtt"))
        return out, None
    base_rtt = quantile(ctl["rtt"], 0.5) / 1000.0
    base_owd = quantile(ctl["owd"], 0.5) / 1000.0
    out.append(("BASE", "info", f"base_rtt {base_rtt:.2f} ms, base_owd {base_owd:.2f} ms (ctl rel_small p50)"))
    fails = {}

    def verdict(name, ok, text, arm):
        out.append((name, "PASS" if ok else "FAIL", text))
        if not ok:
            fails.setdefault(name, []).append(arm)

    for arm, (lp, b) in DECISION_LOSS.items():
        if not cell_ok(arm, "rel_small"):
            out.append(("R1/R2", "VOID", f"{arm} rel_small: {'; '.join(void.get((arm, 'rel_small'), ['missing']))}"))
            continue
        c = cells[(arm, "rel_small")]
        if len(c["owd"]) < POOLED_MIN:
            out.append(("R1/R2", "VOID", f"{arm} rel_small: {len(c['owd'])} latencies from reps the PC did not load, "
                        f"under {POOLED_MIN}"))
            fails.setdefault("VOID", []).append("R1/R2")
            continue
        verdict("R1", c["goodput"] >= 0.995, f"{arm} rel_small delivered {100 * c['goodput']:.2f}% (>= 99.5%)", arm)
        k = rounds_needed(lp, b)
        limit = base_owd + PERIOD_MS + k * ROUND_FACTOR * base_rtt
        p99 = quantile(c["owd"], 0.99) / 1000.0
        verdict("R2", p99 <= limit, f"{arm} rel_small one-way p99 {p99:.2f} ms (<= {limit:.2f} ms: base_owd + T + "
                f"{k} x 1.5 x base_rtt), {len(c['owd'])} latencies", arm)
        out.append(("R2-all", "info", f"{arm} rel_small one-way p99 over every rep, loaded ones included: "
                    f"{quantile(c['owd_all'], 0.99) / 1000.0:.2f} ms of {len(c['owd_all'])}"))
    ctl_l = cells.get(("ctl", "rel_large"))
    for arm in LARGE_ARMS:
        why = void.get((arm, "rel_large"), ["missing"]) if (arm, "rel_large") in cells else ["missing"]
        if ctl_l is None or not ctl_l["p50s"]:
            why = why + ["no ctl rel_large rep the PC did not load: no latency reference"]
        if not cell_ok(arm, "rel_large") or ctl_l is None or not ctl_l["p50s"] or not cells[(arm, "rel_large")]["p50s"]:
            out.append(("R3/R4", "VOID", f"{arm} rel_large: {'; '.join(why) or 'no unloaded rep'}"))
            fails.setdefault("VOID", []).append("R3/R4")
            continue
        c = cells[(arm, "rel_large")]
        verdict("R3", c["goodput"] >= 0.95, f"{arm} rel_large delivered {100 * c['goodput']:.2f}% (>= 95%)", arm)
        p50 = statistics.median(c["p50s"]) / 1000.0
        limit = statistics.median(ctl_l["p50s"]) / 1000.0 + 4 * base_rtt
        rev = 100 * c["rev"] / c["fwd"] if c["fwd"] else float("nan")
        verdict("R4", p50 <= limit, f"{arm} rel_large one-way p50 {p50:.2f} ms (<= {limit:.2f} ms); reverse bytes "
                f"{rev:.2f}% of forward", arm)
        c["rev_pct"] = rev
    if treat_void:
        # A run that did not apply the discovery its profile names says nothing about that profile's R5 or R6.
        out.append(("R5/R6", "VOID", "the discovery treatment: " + "; ".join(treat_void)))
        return out, (fails, cells)
    deaths = {k: c["false_deaths"] for k, c in cells.items() if c["false_deaths"]}
    verdict("R5", not deaths, "false peer deaths: " + (", ".join(f"{a}/{b}={n}" for (a, b), n in sorted(deaths.items()))
                                                      or "none"), "any")
    slow = {k: max(c["join"]) for k, c in cells.items() if c["join"] and quantile(c["join"], 0.99) > 2000}
    verdict("R6", not slow, "ping join p99 over 2 s: " + (", ".join(f"{a}/{b}={v:.0f}ms" for (a, b), v in
                                                                    sorted(slow.items())) or "none"), "any")
    tv = cells.get(("tv", "rel_small"))
    if tv is not None and tv["delivered"]:
        verdict("R7", tv["dups"] <= 0.01 * tv["delivered"], f"tv rel_small copies after delivery {tv['dups']} of "
                f"{tv['delivered']} delivered (<= 1%)", "tv")
    return out, (fails, cells)


def reading(fails_cells):
    if fails_cells is None:
        return ["READING: VOID - no base round trip"]
    fails, cells = fails_cells
    lines = []
    small = set(fails.get("R1", []) + fails.get("R2", []))
    if "ge5b2" in small:
        lines.append("R1/R2 fail at ge5b2 (short bursts): for this profile, local values are NOT enough where FEC acts "
                     "-> lift FEC if this is the tuned WIFI profile")
    elif small:
        lines.append(f"R1/R2 fail only at {sorted(small)} (long bursts): XOR FEC would lose its parity in the burst; "
                     "interleaving/RS is a separate user decision, not this lift")
    else:
        lines.append("R1/R2 pass: small-sample loss and latency within an ideal ARQ's budget -> no FEC lift")
    if "R1/R2" in fails.get("VOID", []):
        lines = ["R1/R2 VOID: the small decision cells cannot be read"]
    large = set(fails.get("R3", []) + fails.get("R4", []))
    for arm in sorted(large):
        rev = cells[(arm, "rel_large")].get("rev_pct", float("nan"))
        if rev > 2.0:
            lines.append(f"R3/R4 fail at {arm} with reverse bytes {rev:.2f}% > 2%: the ACKNACK is material -> lift range "
                         "NACK if this is the tuned WIFI profile")
        else:
            lines.append(f"R3/R4 fail at {arm} with reverse bytes {rev:.2f}% <= 2%: a timer/pacing problem -> a local "
                         "value, no wire lift")
    if "R3/R4" in fails.get("VOID", []):
        lines.append("R3/R4 VOID: the large decision cells cannot be read - neither a pass nor a lift")
    elif not large:
        lines.append("R3/R4 pass: large-sample repair within budget -> no range NACK lift")
    for name, what in (("R5", "false peer deaths -> discovery silence / unicast summary repeat (local values)"),
                       ("R6", "slow joins -> announce/summary repeat (local values)"),
                       ("R7", "spurious repairs after a delay step -> the RTT estimator (local value)")):
        if name in fails:
            lines.append(f"{name} fails: {what}")
    return ["READING: " + x for x in lines]


def main():
    if len(sys.argv) != 2 or not os.path.isdir(os.path.join(sys.argv[1], "runs")):
        print("usage: wifi_burst_read.py OUT_DIR (with OUT_DIR/runs/)", file=sys.stderr)
        return 2
    out = sys.argv[1]
    prov = kv(" ".join(read_lines(os.path.join(out, "provenance.txt"))))
    dur = int(prov.get("dur", "30"))
    print("provenance: " + " | ".join(read_lines(os.path.join(out, "provenance.txt"))))
    profile = prov.get("profile", "ethernet")
    runs = load_runs(out, profile)
    cells = pool(runs, dur)
    arm_order = ["ctl", "ge1b2", "ge1b8", "ge5b2", "ge5b4", "ge5b8", "ge10b4", "bc", "tv"]
    arms = [a for a in arm_order if any(k[0] == a for k in cells)]
    table(cells, ["be_small", "rel_small", "rel_large"], arms)
    void, campaign_void = validity(cells)
    for msg in campaign_void:
        print(f"VOID  campaign: {msg}")
    for (arm, cell), msgs in sorted(void.items()):
        for msg in msgs:
            print(f"VOID  {arm}/{cell}: {msg}")
    treat_void = [f"{a}/{b} {v}" for (a, b), c in sorted(cells.items()) for v in c["treat_void"]]
    for (arm, cell), c in sorted(cells.items()):
        print(f"treatment {profile} {arm}/{cell}: summary copies {c['copies']}, announce repeats {c['repeats']} "
              f"(all nodes, {c['reps']} reps)")
    results, fails_cells = rules(cells, void, campaign_void, treat_void)
    for name, verdict, text in results:
        print(f"{verdict:5} {name}: {text}")
    for line in reading(fails_cells):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
