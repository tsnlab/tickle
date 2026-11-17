#!/usr/bin/env python3
"""Readout of the capacity curve (WIFI_DIAG_PLAN.md section 2). Usage: t1pi_capread.py DIR [--md]
DIR holds <arm>_rep<k>_g<j>.jsonl (+ .rc sampler logs) written by t1pi_cap.sh. One row per (arm, B, R, size): median over reps of
  steady  datagrams/s in the middle arrival bins (first and last bin dropped), the plateau the receiver sees
  del/s   got / sent-time (tx took)       loss  1 - got/sent       took/dur  how long the sender took relative to the plan (>1: backpressure on the socket)
  frac    mean fraction of a burst that arrived (bursts with at least one arrival)       wm%  wmediumd CPU (percent of one core) during the cell, from the sampler
and the spread (min..max) over reps. Also prints, per topology, the STA's minstrel best rate at start and end."""
import glob, json, os, re, statistics, sys
from collections import defaultdict


def med(v):
    v = [x for x in v if x is not None]
    return statistics.median(v) if v else None


def rng(v):
    v = [x for x in v if x is not None]
    return (min(v), max(v)) if v else (None, None)


def load(d):
    rows = []
    for p in sorted(glob.glob(os.path.join(d, "*.jsonl"))):
        m = re.match(r"(\w+)_rep(\d+)_g(\d+)\.jsonl", os.path.basename(p))
        if not m:
            continue
        arm, rep, g = m.group(1), int(m.group(2)), int(m.group(3))
        rc = []
        try:
            rc = [json.loads(l) for l in open(p[:-6] + ".rc")]
        except (OSError, ValueError):
            pass
        for l in open(p):
            try:
                c = json.loads(l)
            except ValueError:
                continue
            tx, rx = c.get("tx"), c.get("rx")
            state = "?"
            if rc:
                b0 = next((y[0] for y in rc[0]["sta"] if "A" in y[1]), None)
                state = "slow" if b0 == "1.0M" else "fast"
            if not rx:
                rows.append(dict(arm=arm, rep=rep, g=g, state=state, burst=c["burst"], rate=c["rate"], size=c["size"], bad=True))
                continue
            if not tx:  # sender killed by its timeout (blocked on a full socket buffer): rx side only
                bins = rx["bins"]
                mid = bins[1:-1] if len(bins) > 2 else bins
                rows.append(dict(arm=arm, rep=rep, g=g, state=state, burst=c["burst"], rate=c["rate"], size=c["size"], got=rx["got"], steady=(statistics.median(mid) if mid else None),
                                 frac=rx["burst_mean_frac"], notx=True, loss=None, dps=None, bp=None))
                continue
            bins = rx["bins"]
            mid = bins[1:-1] if len(bins) > 2 else bins
            r = dict(arm=arm, rep=rep, g=g, state=state, burst=c["burst"], rate=c["rate"], size=c["size"], sent=tx["sent"], got=rx["got"], took=tx["took"],
                     steady=(statistics.median(mid) if mid else None), frac=rx["burst_mean_frac"], foreign=rx["foreign"])
            r["loss"] = 1 - rx["got"] / tx["sent"] if tx["sent"] else None
            r["dps"] = rx["got"] / tx["took"] if tx["took"] else None
            r["bp"] = tx["took"] / (tx["sent"] / c["rate"]) if c["rate"] else None
            ws = [x for x in rc if tx["t_epoch0"] - 0.5 <= x["t"] <= tx["t_epoch1"] + 0.5 and x.get("wm_ticks") is not None]
            r["wm"] = (ws[-1]["wm_ticks"] - ws[0]["wm_ticks"]) / (ws[-1]["t"] - ws[0]["t"]) if len(ws) > 1 else None
            if ws:
                core = [(b[0] - a[0]) / max(1, b[1] - a[1]) * 100 for a, b in zip(ws[0]["cpu"], ws[-1]["cpu"])]
                r["cpu_max_core"] = max(core)
            sta = [x for x in rc if x["t"] <= tx["t_epoch0"]]
            r["sta_rate_start"] = next((y[0] for y in sta[-1]["sta"] if "A" in y[1]), None) if sta else None
            sta = [x for x in rc if x["t"] >= tx["t_epoch1"]]
            r["sta_rate_end"] = next((y[0] for y in sta[0]["sta"] if "A" in y[1]), None) if sta else None
            rows.append(r)
    return rows


def main():
    d = sys.argv[1]
    rows = load(d)
    g = defaultdict(list)
    for r in rows:
        if r.get("bad"):
            continue
        g[(r["arm"], r["state"], r["size"], r["burst"], r["rate"])].append(r)
    print("arm state size B R n (n_notx) | steady/s [min..max] | del/s | loss | took/dur | frac | wm% | maxcore% | sta rate start->end")
    for k in sorted(g, key=lambda k: (k[0], k[1], -k[2], k[3], k[4])):
        v = g[k]
        s0, s1 = rng([x.get("steady") for x in v])
        print("%-4s %-4s %4d %3d %4d %d(%d) | %6.1f [%5.1f..%5.1f] | %6.1f | %5.2f | %5.2f | %4.2f | %s | %s | %s" % (
            k[0], k[1], k[2], k[3], k[4], len(v), sum(1 for x in v if x.get("notx")), med([x.get("steady") for x in v]) or 0, s0 or 0, s1 or 0, med([x.get("dps") for x in v]) or 0, med([x.get("loss") for x in v]) or 0,
            med([x.get("bp") for x in v]) or 0, med([x["frac"] for x in v]) or 0, "%.0f" % med([x.get("wm") for x in v]) if med([x.get("wm") for x in v]) is not None else "-",
            "%.0f" % med([x.get("cpu_max_core") for x in v]) if med([x.get("cpu_max_core") for x in v]) is not None else "-",
            "/".join(sorted(set("%s>%s" % (x.get("sta_rate_start"), x.get("sta_rate_end")) for x in v)))))
    bad = [r for r in rows if r.get("bad")]
    print("cells without a result:", len(bad))


if __name__ == "__main__":
    main()
