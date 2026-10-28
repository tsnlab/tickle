#!/usr/bin/env python3
"""zenoh_fair_read.py <campaign output> - the four-framework table of zenoh_fair_queue.sh's campaign.

READING RULES, pre-registered 2026-10-28 before the first run (zenoh_fair_queue.sh's header gives the reasons):
  R1  A row counts only if campaign_sweep.sh gave it verdict ok. Its VOIDs are printed, never averaged in.
  R2  A cell (framework x row) needs >= 3 usable reps, else it prints VOID(n=<k>). Values are medians over reps.
  R3  Throughput goodput = the SERVER's complete samples inside the measured window per second (win_recv / win_s),
      delivered% = win_recv / the client's win_sent. A sample counts only if it arrived whole: every harness counts a
      sample in its receive callback, which no framework calls for a partial one.
  R4  zenohpico reliable_* rows run over TCP. A reliable throughput rep whose server got < 90% of what the client
      reports sent is LINK_DIED (TCP does not lose bytes; the session dropped its peer, see
      zenoh_tcp_disconnect_repro.sh). It is printed as "died after <received> samples" in how many reps, never as a
      goodput; if >= 1 of the reps died the cell prints DIED k/n beside the surviving reps' median.
  R5  Latency = the client's rtt_p50_ms / rtt_p99_ms over the measured window, median over reps; delivered% =
      measured / measured_sent (round trips that came back within the 500 ms give-up).
  R6  CPU per delivered sample (us) = (client utime+stime + server utime+stime) / server received (throughput) or
      / client recv (latency). Wire bytes per delivered sample = the client's eth0 bytes both ways / the same count.
  R7  zenohpico is a REFERENCE column (the user, 2026-09-29; re-affirmed 2026-10-28): no WIN/LOSE is computed for it
      or against it. TickLE-vs-DDS scoring is campaign_summary.py's, on the same file, unchanged.
  R8  Drift control: the queue's first row is repeated as its last. Per framework, if the two medians differ by more
      than the larger of the two rows' rep ranges, DRIFT is printed and every cross-framework difference smaller than
      that drift is not readable from this campaign.
"""
import re
import statistics as st
import sys
from collections import defaultdict

LINE = re.compile(r"^c(\d+)\s+(\S)\s+(p\d)\s+(Q\d)\s+(.+?)\s+(tickle|cyclonedds|fastdds|zenohpico)\s+rep(\d+)"
                  r"\s+\|\s+(\S+)\s+\|\s+(.*)$")
FWS = ("tickle", "cyclonedds", "fastdds", "zenohpico")


def roles(res):
    out = {}
    for chunk in re.split(r"(?=role=)", res):
        f = dict(kv.split("=", 1) for kv in chunk.split() if "=" in kv)
        if "role" in f:
            out[f["role"]] = f
    # the latency DDS servers print no line; a line with no role= is the client's (campaign_summary.py's rule)
    if "client" not in out and res and not res.startswith("role="):
        f = dict(kv.split("=", 1) for kv in res.split("role=")[0].split() if "=" in kv)
        if f:
            out["client"] = f
    return out


def num(f, k):
    try:
        return float(f[k])
    except (KeyError, ValueError, TypeError):
        return None


def cpu(f):
    u, s = num(f, "utime_s"), num(f, "stime_s")
    return None if u is None or s is None else u + s


def med(v):
    v = [x for x in v if x is not None]
    return st.median(v) if v else None


def main(path):
    rows = defaultdict(lambda: defaultdict(list))  # (num, shape, payload, qos, net) -> fw -> [metrics]
    voids = []
    for line in open(path, errors="replace"):
        m = LINE.match(line.rstrip("\n"))
        if not m:
            continue
        n, shape, size, qos, net, fw, rep, verdict, res = m.groups()
        key = (int(n), shape, size, qos, net.strip())
        if verdict != "ok":
            voids.append("c%s %s %s rep%s %s" % (n, size, fw, rep, verdict))
            continue
        r = roles(res)
        c, s = r.get("client", {}), r.get("server", {})
        met = {}
        if shape == "T":
            sent = num(c, "sent")
            recv = num(s, "received") if num(s, "received") is not None else num(s, "recv")  # DDS servers: recv=
            ws, wr, wsec = num(c, "win_sent"), num(s, "win_recv"), num(s, "win_s")
            if fw == "zenohpico" and qos != "Q1" and sent and recv is not None and recv < 0.9 * sent:
                met["died"] = recv
            if wr is not None and wsec:
                met["goodput"] = wr / wsec
            if wr is not None and ws:
                met["delivered"] = 100.0 * wr / ws
            denom = recv
        else:
            met["p50"], met["p99"] = num(c, "rtt_p50_ms"), num(c, "rtt_p99_ms")
            ms, mr = num(c, "measured_sent"), num(c, "measured")
            if ms:
                met["delivered"] = 100.0 * (mr or 0) / ms
            denom = num(c, "recv")
        tot = [x for x in (cpu(c), cpu(s)) if x is not None]
        if denom:
            met["cpu_us"] = 1e6 * sum(tot) / denom if tot else None
            wb = num(c, "wire_bytes_total")
            met["wire_b"] = wb / denom if wb is not None else None
        rows[key][fw].append(met)

    print("zenoh_fair_read: %s  (zenohpico = REFERENCE, R7; zenohpico reliable = TCP, R4)" % path)
    for key in sorted(rows):
        n, shape, size, qos, net = key
        print("\nc%d %s %s %s %s" % key)
        for fw in FWS:
            reps = rows[key].get(fw, [])
            if not reps:
                continue
            died = [r["died"] for r in reps if "died" in r]
            alive = [r for r in reps if "died" not in r]
            tag = "%-10s" % fw + (" (TCP)" if fw == "zenohpico" and qos != "Q1" else "      ")
            if len(reps) < 3:
                print("  %s VOID(n=%d)" % (tag, len(reps)))
                continue
            extra = ""
            if died:
                extra = "  DIED %d/%d (died after %s samples)" % (len(died), len(reps),
                                                                   ",".join("%d" % d for d in died))
            if shape == "T":
                print("  %s goodput %10s/s  delivered %6s%%  cpu %8s us/sample  wire %8s B/sample  n=%d%s" % (
                    tag, fmt(med([r.get("goodput") for r in alive]), "%.0f"),
                    fmt(med([r.get("delivered") for r in alive]), "%.2f"),
                    fmt(med([r.get("cpu_us") for r in alive]), "%.2f"),
                    fmt(med([r.get("wire_b") for r in alive]), "%.1f"), len(alive), extra))
            else:
                print("  %s p50 %8s ms  p99 %8s ms  delivered %6s%%  cpu %8s us/rt  wire %8s B/rt  n=%d" % (
                    tag, fmt(med([r.get("p50") for r in reps]), "%.3f"), fmt(med([r.get("p99") for r in reps]), "%.3f"),
                    fmt(med([r.get("delivered") for r in reps]), "%.2f"), fmt(med([r.get("cpu_us") for r in reps]), "%.1f"),
                    fmt(med([r.get("wire_b") for r in reps]), "%.1f"), len(reps)))
    # R8: the first and last rows are the same cell
    keys = sorted(rows)
    if len(keys) >= 2 and keys[0][1:] == keys[-1][1:]:
        print("\nR8 drift control (c%d vs c%d):" % (keys[0][0], keys[-1][0]))
        metric = "goodput" if keys[0][1] == "T" else "p50"
        for fw in FWS:
            a = [r.get(metric) for r in rows[keys[0]].get(fw, []) if r.get(metric) is not None]
            b = [r.get(metric) for r in rows[keys[-1]].get(fw, []) if r.get(metric) is not None]
            if len(a) < 3 or len(b) < 3:
                print("  %-10s VOID (n=%d/%d)" % (fw, len(a), len(b)))
                continue
            drift = abs(st.median(b) - st.median(a))
            spread = max(max(a) - min(a), max(b) - min(b))
            print("  %-10s first %.0f last %.0f drift %.0f spread %.0f -> %s" % (
                fw, st.median(a), st.median(b), drift, spread, "DRIFT" if drift > spread else "ok"))
    if voids:
        print("\nVOID rows (%d), not in any median:" % len(voids))
        for v in voids:
            print("  " + v)


def fmt(v, f):
    return "-" if v is None else f % v


if __name__ == "__main__":
    main(sys.argv[1])
