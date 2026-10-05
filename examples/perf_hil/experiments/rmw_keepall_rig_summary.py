#!/usr/bin/env python3
"""Reads rmw_keepall_rig.sh's runs and applies the reading rules written in its header.

Usage: rmw_keepall_rig_summary.py <OUT.runs dir> <DUR>
"""
import math
import re
import statistics
import sys
from pathlib import Path

ERROR_MARKERS = ("terminate", "exception", "what():", "timeout", "Segmentation", "Aborted")


def rows(path):
    """perf_test's per-second data rows as dicts, or [] when the log has none."""
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


def loaded(want, maps):
    """A tickle arm names its library's exact path; a vendor arm names the file, found anywhere on the path."""
    paths = maps.split()
    if want.startswith("/"):
        return want in paths
    return any(p.endswith("/" + want) for p in paths)


def mean_se(xs):
    if len(xs) < 2:
        return (xs[0] if xs else float("nan")), float("inf")
    return statistics.mean(xs), statistics.stdev(xs) / math.sqrt(len(xs))


def main():
    runs, dur = Path(sys.argv[1]), int(sys.argv[2])
    index = {}
    for line in (runs / "index.txt").read_text().splitlines():
        m = re.match(r"RUN (\S+) pub_maps=\[(.*?)\] sub_maps=\[(.*?)\] pub_killed=(\S+) sub_killed=(\S+) want=(\S+)", line)
        if m:
            index[m.group(1)] = m.groups()[1:]

    cells = {}  # (topic, loss) -> arm -> list of per-rep dicts
    voids, failures = [], []
    for stem, (pmaps, smaps, pkilled, skilled, want) in sorted(index.items()):
        arm, topic, loss, rep = re.match(r"(.+?)_(Array\w+|Struct\w+)_l(\d+)_r(\d+)$", stem).groups()
        sub, _ = rows(runs / f"{stem}_sub.log")
        pub, ptext = rows(runs / f"{stem}_pub.log")
        refused = "failed to publish" in ptext
        why = []
        header_rmw = re.search(r"RMW Implementation: (\S+)", ptext)
        vendor_by_log = not want.startswith("/") and header_rmw and want.startswith("lib" + header_rmw.group(1))
        if not loaded(want, pmaps) and not vendor_by_log:
            why.append(f"publisher did not load {want} (maps: {pmaps.strip() or 'none'})")
        if not loaded(want, smaps):
            why.append(f"subscriber did not load {want} (maps: {smaps.strip() or 'none'})")
        live = [r for r in sub if r.get("received", 0) > 0]
        if not live:
            why.append("subscriber received nothing")
        if why:
            voids.append(f"{stem}: " + "; ".join(why))
            continue
        steady = live[2:-1] if len(live) > 4 else live
        rate = statistics.mean(r["received"] for r in steady)
        lost = int(sum(r.get("lost", 0) for r in sub))
        sent = int(sum(r.get("sent", 0) for r in pub))
        recv = int(sum(r.get("received", 0) for r in sub))
        errors = [mk for mk in ERROR_MARKERS if mk in ptext] + (["publisher killed at deadline"] if pkilled != "no" else [])
        cpu_pub = (pub[-1]["ru_utime"] + pub[-1]["ru_stime"]) / sent * 1e6 if pub and sent else float("nan")
        cpu_sub = (sub[-1]["ru_utime"] + sub[-1]["ru_stime"]) / recv * 1e6 if recv else float("nan")
        lat = statistics.mean(r["latency_mean (ms)"] for r in steady) if steady else float("nan")
        rec = dict(rep=int(rep), refused=refused, rate=rate, lost=lost, sent=sent, recv=recv, errors=errors,
                   cpu_pub=cpu_pub, cpu_sub=cpu_sub, lat=lat, seconds=len(live))
        cells.setdefault((topic, int(loss)), {}).setdefault(arm, []).append(rec)
        if arm in ("tickle@ack", "tickle@head") and (lost or errors):
            failures.append(f"{stem}: lost={lost} errors={errors}")

    print()
    print(f"=== per run, {dur} s publisher runtime (delivered msgs/s over steady seconds; CPU us per sample; latency not judged) ===")
    for (topic, loss), arms in sorted(cells.items()):
        for arm, recs in sorted(arms.items()):
            for r in sorted(recs, key=lambda r: r["rep"]):
                print(f"{topic:8} l{loss:<2} {arm:12} r{r['rep']} rate {r['rate']:9.1f}/s  sent {r['sent']:8}  recv "
                      f"{r['recv']:8}  lost {r['lost']:6}  cpu pub {r['cpu_pub']:6.2f} sub {r['cpu_sub']:6.2f}  "
                      f"lat {r['lat']:.3f} ms  {'REFUSED after ' + str(r['seconds']) + ' s ' if r['refused'] else ''}"
                      f"{('ERR ' + ','.join(r['errors'])) if r['errors'] else ''}")

    if voids:
        print("\nVOID runs:")
        for v in voids:
            print("  " + v)

    print("\n=== CORRECTNESS (tickle@ack, tickle@head: lost == 0, clean publisher) ===")
    judged = sum(len(arms.get(a, [])) for arms in cells.values() for a in ("tickle@ack", "tickle@head"))
    if failures:
        print("FAIL:\n  " + "\n  ".join(failures))
    elif judged == 0:
        print("NO VERDICT: no usable tickle@ack or tickle@head run - nothing was checked")
    else:
        print(f"PASS - no loss and no publisher error in any of {judged} ack/head runs")

    ack_better_under_loss = []
    print("\n=== RATE verdicts (2 x SE and beyond the controls' own rep-to-rep spread) ===")
    for (topic, loss), arms in sorted(cells.items()):
        floors, unusable = [], []
        for ctl in ("fastdds", "cyclonedds"):
            rates = [r["rate"] for r in arms.get(ctl, []) if not r["refused"]]
            if len(rates) < 2:
                unusable.append(f"{ctl} ({len(rates)} usable, {sum(r['refused'] for r in arms.get(ctl, []))} refused)")
                continue
            floors.append(max(rates) / min(rates))
        if not floors:
            print(f"{topic} l{loss}: VOID - no control has two usable reps: " + ", ".join(unusable))
            continue
        floor = max(floors)
        line = [f"{topic} l{loss}: drift floor {floor:.3f}x" + (f" (without {', '.join(unusable)})" if unusable else "")]
        for a, b in (("tickle@pre", "tickle@ack"), ("tickle@ack", "tickle@head")):
            ra = [r["rate"] for r in arms.get(a, []) if not r["refused"]]
            rb = [r["rate"] for r in arms.get(b, []) if not r["refused"]]
            if len(ra) < 2 or len(rb) < 2:
                line.append(f"{b} vs {a}: too few reps")
                continue
            ma, sa = mean_se(ra)
            mb, sb = mean_se(rb)
            diff, se = mb - ma, math.sqrt(sa ** 2 + sb ** 2)
            ratio = mb / ma if ma else float("inf")
            if diff > 2 * se and ratio > floor:
                v = "BETTER"
            elif -diff > 2 * se and (1 / ratio if ratio else float("inf")) > floor:
                v = "WORSE"
            else:
                v = "HELD"
            line.append(f"{b} vs {a}: {mb:.0f} vs {ma:.0f}/s = {ratio:.3f}x {v}")
        ctl_txt = ", ".join(f"{c} {statistics.mean(r['rate'] for r in arms[c]):.0f}/s" for c in ("fastdds", "cyclonedds")
                            if arms.get(c))
        print("  ".join(line) + f"  [{ctl_txt}]")
        if loss > 0 and any("tickle@ack vs tickle@pre" in x and x.endswith("BETTER") for x in line):
            ack_better_under_loss.append(f"{topic} l{loss}")

    print()
    decidable = [c for c, arms in cells.items() if c[1] > 0
                 and sum(not r["refused"] for r in arms.get("tickle@pre", [])) >= 2
                 and sum(not r["refused"] for r in arms.get("tickle@ack", [])) >= 2]
    if not decidable:
        print("NO VERDICT: no lossy cell has two usable reps of both tickle@pre and tickle@ack - this run cannot "
              "say whether the fix reaches the rmw layer")
    elif ack_better_under_loss:
        print("FIX REACHES THE RMW LAYER: ack is BETTER than pre in " + ", ".join(ack_better_under_loss))
    else:
        print("FALSIFIED for this rig and rate: ack is not BETTER than pre in any lossy cell - the fix does not reach "
              "the rmw layer at max rate here")


if __name__ == "__main__":
    main()
