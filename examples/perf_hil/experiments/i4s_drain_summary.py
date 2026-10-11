#!/usr/bin/env python3
"""i4s_drain_summary.py - the rig reading of the receive drain (DESIGN.md section 8, "Saturation"), written before the
run (2026-10-11).

Reads an rmw_keepall_rig.sh runs directory taken with KCOUNTERS=1 and arms tickle@pre (the tree before core asks for
the drain, with the "Node N rx:" line) and tickle@head (with it), beside fastdds and cyclonedds. Per-run figures come
from largemsg_rig_summary.load() (whole sample counts, one-clock latency, CPU per MB); this adds the counters that say
where a sample was lost and whether the drain ran.

  W0 TREATMENT  every tickle@head run's subscriber says drain=1 and drain_starts > 0, every tickle@pre run's says
                drain_starts 0 or nothing: else VOID. A head run with drain=-1 (the Pi's kernel refused io_uring or
                multishot recvmsg) is VOID as well - the rig then cannot test the change.
  W1 LOCATION   tickle@pre, cell I4s, every run: published - tail_abandoned = reassembled + abandoned (sender side
                accounted for), and the subscriber's socket_drops > 0 whenever abandoned > 0. With the kernel counters:
                NIC drops (ethtool deltas named *drop*/*miss*/*discard* on either Pi) = 0 and the publisher's qdisc
                dropped delta = 0. SOCKET when all hold; otherwise NIC, QDISC or UNEXPLAINED, said per run - and the
                drain, which acts only on the socket, is then not the whole answer.
  W2 FIX        tickle@head, I4s, the change's own claim: the subscriber completes what the wire carried -
                reassembled >= published - tail_abandoned - 2 and socket_drops = 0 - in at least 4 of 5 runs. PASS or
                FAIL.
  W2b DDS       the cell's L4 question, reported beside W2 and not the change's criterion: head's median complete
                samples against the best DDS's. In A4 s3a the sender put 1,693 samples on the wire a run (1,802 less
                109 superseded whole), CycloneDDS delivered 1,699-1,700: per-datagram headers cost TickLE's MTU-unit
                fragments ~0.4% of the saturated link. If head's wire count is itself below the best DDS's complete
                count, the remaining gap is that overhead - Dev's hybrid IP mode's question, not this change's.
  W3 LATENCY    tickle@head against tickle@pre, I4s and the controls: latency median within 2 SE of the difference
                (the drain is not expected to move it; I4s's queueing latency against DDS is its own item).
  W4 CPU        tickle@head against tickle@pre: subscriber and publisher CPU per MB not worse beyond 2 SE in the
                controls; I4s printed (head delivers more there).
  W5 CONTROLS   I4 (Array4m 15 Hz) and I1 (Array1m 30 Hz) best effort: tickle@head delivers 100% complete in every run.
  RSS           tickle@head's subscriber peak RSS minus tickle@pre's, printed (the drain's ~4.25 MB of buffers).

Usage: i4s_drain_summary.py RUNS_DIR
"""

import math
import re
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import largemsg_rig_summary as lrs  # noqa: E402

HEAD, PRE = "tickle@head", "tickle@pre"
VENDORS = ("fastdds", "cyclonedds")


def last_int(text, key):
    found = re.findall(rf"{key}=(-?\d+)", text)
    return int(found[-1]) if found else None


def snmp(text, table, key):
    lines = [line.split() for line in text.splitlines() if line.startswith(table + ":")]
    if len(lines) >= 2 and key in lines[0]:
        return int(lines[1][lines[0].index(key)])
    return None


def nic_drops(text):
    part = text.split("--- ethtool", 1)[-1].split("--- tc", 1)[0]
    return sum(int(v) for k, v in re.findall(r"^\s*(\S+):\s*(\d+)\s*$", part, re.M)
               if re.search(r"drop|miss|discard", k, re.I))


def qdisc_drops(text):
    part = text.split("--- tc", 1)[-1].split("--- softnet", 1)[0]
    return sum(int(v) for v in re.findall(r"dropped (\d+)", part))


def kdelta(runs, stem, role, fn):
    pre, post = runs / f"{stem}_{role}_kpre.txt", runs / f"{stem}_{role}_kpost.txt"
    if not pre.exists() or not post.exists():
        return None
    a, b = fn(pre.read_text(errors="replace")), fn(post.read_text(errors="replace"))
    return None if a is None or b is None else b - a


def counters(runs, r):
    stem = f"{r['arm']}_{r['topic']}_l{r['loss']}_r{r['rep']}"
    ptext = (runs / f"{stem}_pub.log").read_text(errors="replace")
    stext = (runs / f"{stem}_sub.log").read_text(errors="replace")
    c = dict(stem=stem)
    c["published"] = last_int(ptext, "large: published")
    c["tail_abandoned"] = last_int(ptext, "tail_abandoned")
    sub_large = re.findall(r"large: published=\d+ reassembled=(\d+) abandoned=(\d+)", stext)
    c["reassembled"], c["abandoned"] = (int(sub_large[-1][0]), int(sub_large[-1][1])) if sub_large else (None, None)
    c["socket_drops"] = last_int(stext, "rx: socket_drops")
    c["drain"] = last_int(stext, " drain")
    c["drain_starts"] = last_int(stext, "drain_starts")
    c["rcvbuf_errors"] = kdelta(runs, stem, "sub", lambda t: snmp(t, "Udp", "RcvbufErrors"))
    c["reasm_fails"] = kdelta(runs, stem, "sub", lambda t: snmp(t, "Ip", "ReasmFails"))
    nics = [kdelta(runs, stem, role, nic_drops) for role in ("pub", "sub")]
    c["nic_drops"] = None if None in nics else sum(nics)
    c["qdisc_drops"] = kdelta(runs, stem, "pub", qdisc_drops)
    return c


def mean_se(xs):
    xs = lrs.finite(xs)
    if len(xs) < 2:
        return (xs[0] if xs else float("nan")), float("nan")
    return st.mean(xs), st.stdev(xs) / math.sqrt(len(xs))


def within(head, pre):
    (mh, sh), (mp, sp) = mean_se(head), mean_se(pre)
    if any(math.isnan(x) for x in (mh, mp, sh, sp)):
        return "NO READING", mh, mp
    se = math.hypot(sh, sp)
    return ("HELD" if mh <= mp + 2 * se else "WORSE"), mh, mp


def main():
    runs = Path(sys.argv[1])
    meta, samehost, recs = lrs.load(runs)
    recs = [r for r in recs if not r["void"]]
    for r in recs:
        r["name"], r["cell"], _q = lrs.cell_name(r["topic"], r["loss"], meta, samehost)
        r.update(counters(runs, r) if r["arm"].startswith("tickle@") else {})
    verdict = {}

    print("=== per run: sender accounting, where the rest went, the drain ===")
    for r in sorted(recs, key=lambda r: (r["cell"], r["arm"], r["rep"])):
        if not r["arm"].startswith("tickle@"):
            continue
        print(f"{r['cell']:44} {r['arm']:12} r{r['rep']} recv {r['recv']:5} published {r['published']} tail_abandoned "
              f"{r['tail_abandoned']} reassembled {r['reassembled']} abandoned {r['abandoned']} socket_drops "
              f"{r['socket_drops']} rcvbuf_errors {r['rcvbuf_errors']} reasm_fails {r['reasm_fails']} nic_drops "
              f"{r['nic_drops']} qdisc_drops {r['qdisc_drops']} drain={r['drain']} starts {r['drain_starts']}")

    bad = [r for r in recs if r["arm"] == HEAD and not (r["drain"] == 1 and (r["drain_starts"] or 0) > 0)]
    bad += [r for r in recs if r["arm"] == PRE and (r["drain_starts"] or 0) != 0]
    verdict["W0 TREATMENT"] = "PASS" if not bad else "VOID: " + ", ".join(r["stem"] for r in bad)

    loc = []
    for r in [r for r in recs if r["arm"] == PRE and r["name"] == "I4s"]:
        why = []
        if None in (r["published"], r["tail_abandoned"], r["reassembled"], r["abandoned"]):
            why.append("no large counters")
        elif abs(r["published"] - r["tail_abandoned"] - r["reassembled"] - r["abandoned"]) > 1:
            why.append("sender not accounted")
        if (r["abandoned"] or 0) > 0 and not (r["socket_drops"] or 0) > 0:
            why.append("abandoned without socket drops")
        if r["nic_drops"] is None or r["qdisc_drops"] is None:
            where = "NO KERNEL COUNTERS"
        elif r["nic_drops"] > 0:
            where = "NIC"
        elif r["qdisc_drops"] > 0:
            where = "QDISC"
        else:
            where = "SOCKET" if not why else "UNEXPLAINED"
        loc.append(f"r{r['rep']} {where}{(' (' + '; '.join(why) + ')') if why else ''}")
    verdict["W1 LOCATION"] = "; ".join(loc) if loc else "NO READING"

    i4s = [r for r in recs if r["name"] == "I4s"]
    head = [r for r in i4s if r["arm"] == HEAD]
    best = max((lrs.med([r["recv"] for r in i4s if r["arm"] == v]) for v in VENDORS
                if any(r["arm"] == v for r in i4s)), default=float("nan"))
    if head:
        whole = [r for r in head if None not in (r["published"], r["tail_abandoned"], r["reassembled"])
                 and r["reassembled"] >= r["published"] - r["tail_abandoned"] - 2 and r["socket_drops"] == 0]
        verdict["W2 FIX"] = (f"{'PASS' if len(whole) >= 4 else 'FAIL'}: {len(whole)} of {len(head)} runs completed what "
                             f"the wire carried, with no socket drop")
        wire = lrs.med([r["published"] - r["tail_abandoned"] for r in head if r["published"] is not None])
        med = lrs.med([r["recv"] for r in head])
        verdict["W2b DDS"] = (f"head median {med:.0f} complete samples ({wire:.0f} on the wire) against the best DDS "
                              f"{best:.0f}{'; the wire count is below it - overhead, not loss' if wire < best else ''}")
    else:
        verdict["W2 FIX"] = "NO READING"

    for name in ("I4s", "I4", "I1"):
        cell = [r for r in recs if r["name"] == name]
        h, p = [r for r in cell if r["arm"] == HEAD], [r for r in cell if r["arm"] == PRE]
        if not h or not p:
            continue
        for key, label in (("lat_med", "W3 latency median"), ("cpu_sub", "W4 sub CPU/MB"), ("cpu_pub", "W4 pub CPU/MB"),
                           ("rss_sub", "RSS sub kB")):
            word, mh, mp = within([r[key] for r in h], [r[key] for r in p])
            if key == "rss_sub":
                word = f"head - pre {mh - mp:+.0f}"
            verdict[f"{label} {name}"] = f"{word} (head {mh:.2f}, pre {mp:.2f})"
        if name != "I4s":
            full = all(r["recv"] >= r["sent"] - 1 and (r["abandoned"] or 0) == 0 for r in h)
            verdict[f"W5 CONTROL {name}"] = "PASS" if full else "FAIL: " + ", ".join(
                f"r{r['rep']} recv {r['recv']} sent {r['sent']} abandoned {r['abandoned']}" for r in h)
    for k in sorted(verdict):
        print(f"VERDICT {k}: {verdict[k]}")


if __name__ == "__main__":
    main()
