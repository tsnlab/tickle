#!/usr/bin/env python3
"""r4_trace_read.py - where a 1 MiB sample's repair latency goes (R4 investigation, 2026-10-25).

Reads one or more rel_large run directories of wifi_burst_loss_netns.sh run with LARGE_TRACE=1: the writer's and
reader's core trace (pub.trace, sub.trace; src/tickle.c's R4_TRACE events, CLOCK_MONOTONIC ns, one clock for every
namespace) and the reader's LAT lines (echo.log: publish time and callback time of each sample).

Events: writer C/E (a send call of a sample's fresh units starts / ends; E b=1 the sample's last unit handed to the
socket), N (ACKNACK received: base, bits requested), S (a unit resent: seq, resend number), H (a request for a unit held
by the resend order), Q (a request for a unit the send has not reached), F (a resend the socket refused); reader U
(units recorded: first seq, count), D/X (duplicate / not recorded), A/a (ACKNACK sent: first named, bits; base, reason
T timer, M immediate, G new gap), I (the retry timer fired: interval us, retry count).

Per sample, the CRITICAL unit is the unit of that sample whose recording came last (it decides the delivery). The
sample's publish-to-callback latency is split, on that unit's own timeline, into:
  drain      publish -> the writer's last fresh unit of the sample handed to the socket ... -> the reader's last FRESH
             unit of the sample recorded (what the control has too: the sample's serialization through netem's slots);
  to_repair  -> the critical unit's first resend at the writer (detection + request transit + any hold);
  rerepair   first resend -> last resend (the rounds lost repairs cost); split per round into the reader's wait before it
             asks again (its retry timer: first request the writer saw after the previous resend, minus that resend)
             and the writer's hold of a request (H events: request seen -> resend);
  transit    last resend -> recorded at the reader (netem's 2 ms delay, its slot, and its queue: anything ahead of the
             repair on the writer's egress);
  callback   recorded -> the callback.
Every repair's transit (resend -> recorded) is also listed against whether the writer still had fresh units of a
sample in the socket's way at the time (any C/E of a fresh send within the 45 ms before), which separates queueing
behind fresh data from the egress's own delay.
"""
import bisect
import collections
import os
import re
import statistics
import sys


def q(v, p):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))]


def load(path):
    ev = []
    with open(path) as f:
        for line in f:
            k, t, a, b = line.split()
            ev.append((k, int(t), int(a), int(b)))
    return ev


def one_run(d):
    pub = load(os.path.join(d, "pub.trace"))
    sub = load(os.path.join(d, "sub.trace"))
    lat = {}
    for line in open(os.path.join(d, "echo.log")):
        m = re.match(r"LAT index=(\d+) sent_ns=(\d+) arrived_ns=(\d+)", line)
        if m:
            lat[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
    # samples in the writer's order: record seq -> count, last fresh unit handed over
    samples = []
    seen = {}
    for k, t, a, b in pub:
        if k == "C":
            # a is record seq + next index; a sample's first call starts at its record seq
            if not samples or a >= samples[-1][0] + samples[-1][1]:
                samples.append([a, b, None, t])
        elif k == "E" and b == 1 and samples:
            samples[-1][2] = t
    resends = collections.defaultdict(list)  # unit -> [t]
    holds = collections.defaultdict(list)
    for k, t, a, b in pub:
        if k == "S":
            resends[a].append(t)
        elif k == "H":
            holds[a].append(t)
    acks_rx = [t for k, t, a, b in pub if k == "N"]
    fresh_calls = [t for k, t, a, b in pub if k in "CE"]
    rec = {}  # unit -> first recorded time
    for k, t, a, b in sub:
        if k == "U":
            for u in range(a, a + b):
                rec.setdefault(u, t)
    timers = [(t, a) for k, t, a, b in sub if k == "I"]
    sent_acks = [(t, chr(b & 0xFF)) for k, t, a, b in sub if k == "a"]
    ack_times = [t for t, _ in sent_acks]
    acks_rx_sorted = acks_rx
    reasons = collections.Counter(chr(b & 0xFF) for k, t, a, b in sub if k == "a")
    rows = []
    for idx, (s0, cnt, e_t, c_t) in enumerate(samples):
        if idx not in lat:
            continue
        sent, arrived = lat[idx]
        if arrived < sent or sent < c_t - 50_000_000 or sent > c_t + 50_000_000:
            continue  # the LAT row is not this record's (index and record order disagree)
        units = range(s0, s0 + cnt)
        if any(u not in rec for u in units):
            continue
        crit = max(units, key=lambda u: rec[u])
        fresh = [rec[u] for u in units if u not in resends]
        last_fresh = max(fresh) if fresh else sent
        r = {"idx": idx, "lat": arrived - sent, "lost": sum(1 for u in units if u in resends),
             "drain": last_fresh - sent, "callback": arrived - rec[crit], "rounds": len(resends.get(crit, []))}
        rs = resends.get(crit, [])
        if rs:
            r["to_repair"] = rs[0] - last_fresh
            r["first_resend_after_pub"] = rs[0] - sent
            r["rerepair"] = rs[-1] - rs[0]
            r["transit"] = rec[crit] - rs[-1]
            if len(rs) >= 2:
                # the second resend's wait past the later of the drain's end and the first resend (K2's P5 figure)
                r["after_drain"] = rs[1] - max(last_fresh, rs[0])
            wait = hold = 0
            r["reask_by"] = []
            for j in range(1, len(rs)):
                hs = [h for h in holds.get(crit, []) if rs[j - 1] < h < rs[j]]
                first_req = hs[0] if hs else rs[j]
                wait += first_req - rs[j - 1]
                hold += rs[j] - first_req
                # the reader's ACKNACK that asked: its latest one sent before the writer saw the request
                i = bisect.bisect_left(ack_times, first_req) - 1
                r["reask_by"].append(sent_acks[i][1] if i >= 0 else "?")
            r["reask_wait"] = wait
            r["writer_hold"] = hold
        else:
            r["to_repair"] = 0
            r["transit"] = 0
            r["rerepair"] = 0
            r["reask_wait"] = r["writer_hold"] = 0
        rows.append(r)
    # every repair's transit, split by fresh data in the way
    tr_busy, tr_idle = [], []
    for u, ts in resends.items():
        if u not in rec:
            continue
        t_last = ts[-1]
        transit = rec[u] - t_last
        if transit < 0:
            continue  # recorded before its last resend: a copy, not this resend
        busy = any(t_last - 45_000_000 <= c <= t_last for c in fresh_calls)
        (tr_busy if busy else tr_idle).append(transit)
    return rows, tr_busy, tr_idle, timers, reasons, len(acks_rx), sum(len(v) for v in resends.values()), \
        sum(len(v) for v in holds.values())


def main(dirs):
    allrows, busy, idle, timers = [], [], [], []
    reasons = collections.Counter()
    nacks = nres = nhold = 0
    for d in dirs:
        rows, b, i, t, r, na, nr, nh = one_run(d)
        allrows += rows
        busy += b
        idle += i
        timers += t
        reasons += r
        nacks += na
        nres += nr
        nhold += nh
        ad = [x["after_drain"] for x in rows if "after_drain" in x]
        print(f"{d}: {len(rows)} samples traced; second resend past max(drain end, first resend) p50 "
              f"{q(ad, .5) / 1e6:.1f} ms (n {len(ad)})")
    ms = 1e6
    print(f"samples {len(allrows)}; ACKNACKs at the writer {nacks}, resends {nres}, held requests {nhold}; reader "
          f"ACKNACK reasons {dict(reasons)}")
    if not allrows:
        return
    for key in ["lat", "drain", "to_repair", "reask_wait", "writer_hold", "rerepair", "transit", "callback"]:
        v = [r[key] for r in allrows if key in r]
        print(f"  {key:12s} p25 {q(v, .25) / ms:7.2f}  p50 {q(v, .5) / ms:7.2f}  p75 {q(v, .75) / ms:7.2f} ms  "
              f"mean {statistics.mean(v) / ms:7.2f}  (n {len(v)})")
    by = collections.Counter(x for r in allrows for x in r.get("reask_by", []))
    print(f"  critical unit's re-requests, by the reader ACKNACK that carried them (T timer, G new gap, M immediate): "
          f"{dict(by)}")
    rd = collections.Counter(r["rounds"] for r in allrows)
    print(f"  critical unit's resends (rounds): {dict(sorted(rd.items()))}; lost units a sample p50 "
          f"{q([r['lost'] for r in allrows], .5)}")
    # the median sample's breakdown: samples within +-10% of the p50 latency
    p50 = q([r["lat"] for r in allrows], .5)
    near = [r for r in allrows if abs(r["lat"] - p50) <= 0.1 * p50]
    print(f"  samples near the p50 ({p50 / ms:.2f} ms, n {len(near)}), mean terms:")
    for key in ["drain", "to_repair", "reask_wait", "writer_hold", "transit", "callback"]:
        print(f"    {key:12s} {statistics.mean(r[key] for r in near) / ms:7.2f} ms")
    print(f"  repair transit (last resend -> recorded): fresh send in the 45 ms before: n {len(busy)} p50 "
          f"{q(busy, .5) / ms:.2f} p90 {q(busy, .9) / ms:.2f} ms; none: n {len(idle)} p50 {q(idle, .5) / ms:.2f} p90 "
          f"{q(idle, .9) / ms:.2f} ms")
    if timers:
        iv = [a for t, a in timers]
        print(f"  reader retry timer fired {len(timers)} times; interval us p10 {q(iv, .1)} p50 {q(iv, .5)} p90 "
              f"{q(iv, .9)}")


if __name__ == "__main__":
    main(sys.argv[1:])
