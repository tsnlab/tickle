#!/usr/bin/env python3
"""Reads fastdds_keepall_arms.sh's runs and applies the reading rules below, written before any run.

Usage: fastdds_keepall_arms_summary.py <OUT.runs dir> <DUR>

The question (docs/ROADMAP.md, "Fast DDS under KEEP_ALL, from its side"): Fast DDS's KEEP_ALL write times out at
5% loss on the rig (RESULTS rows 73 and 75). The explanation on the table: the 5,000-sample history fills, a repair
lost together with its piggybacked heartbeat waits for the 3 s periodic heartbeat, and write() gives up after its
100 ms max_blocking_time. Arms (fastdds/*.xml, one changed parameter set each; CycloneDDS is the control):
  F0  fastdds_eth0_only.xml unchanged (what every published Fast DDS figure used)
  F1  F0 + max_blocking_time 5 s
  F2  F1 + heartbeatPeriod 50 ms
  F3  F0 + max_samples 50,000

HOW TO READ IT (enforced below; every verdict line prints the realized inputs it was read from):
  VOID run: perf_test's /proc/PID/maps on either Pi does not show the arm's rmw library (a publisher that ended
    before its maps were read may be identified by its log's "RMW Implementation:" line), the treatment record of
    either side does not show FASTRTPS_DEFAULT_PROFILES_FILE=/tmp/ka_fdds_<F>.xml with the hash this checkout's
    profile had (or shows RMW_FASTRTPS_USE_QOS_FROM_XML / RMW_FASTRTPS_PUBLICATION_MODE set), either log carries a
    Fast DDS XMLPARSER error, or the subscriber received nothing. A void publisher still counts as REFUSED evidence
    when only the subscriber side failed.
  REFUSED run: the publisher log holds "failed to publish" (the KEEP_ALL write timed out; rclcpp throws).
  STALL: an interior per-second row of the subscriber (strictly between its first and last delivering second) that
    delivered under 10% of the run's median delivering second. A stall the 3 s heartbeat explains spans at most 4
    consecutive rows.
  F0 at 5% loss, per topic: REPRODUCED if at least 2 identified F0 runs are REFUSED. Otherwise F1 and F2 at that
    topic test nothing and are VOID.
  F1 at 5% loss (prediction: no timeout, ~3 s stalls): FALSIFIED if any F1 run is REFUSED, if no F1 run has a
    stall, or if 2+ runs have a stall longer than 4 rows; SUPPORTED if 2+ runs have a stall and none of those; else
    INCONCLUSIVE. Control: F1 at 0% loss must not have stalls in 2+ runs, or the stall reading is VOID (stalls that
    occur without loss are not the repair waiting for a heartbeat).
  F2 at 5% loss (prediction: stalls shrink to tens of ms, rate well above F1): FALSIFIED if any F2 run is REFUSED,
    if 2+ F2 runs still have a stall, or if F2's rate is not BETTER than F1's by at least 2x; SUPPORTED otherwise.
    BETTER = mean difference over 2 x its standard error AND a ratio beyond CycloneDDS's own max/min rep ratio in
    the same cell (the rig's drift).
  F3 at Array1k 0% loss vs F0: rate BETTER -> the writer history was the limit; rate HELD and mean latency at least
    3x F0's -> a subscriber-side bottleneck (the history is a standing queue in front of a slower consumer: 10x the
    samples, ~10x the latency); rate HELD and latency under 3x -> NEITHER, unexplained; rate WORSE -> reported as
    unexpected, not read. Latency is compared F3 to F0 only: the Pis' clock offset is the same in both and far
    below the ~300 ms F0 shows.
  Every comparison needs 2 usable (not REFUSED) reps of each side and of CycloneDDS, or it prints NO VERDICT.
"""
import math
import re
import statistics
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rmw_keepall_rig_summary import (ERROR_MARKERS, INDEX_RE, loaded, loss_split, mean_se, rows,  # noqa: E402
                                     sample_bytes, whole_counts)

STALL_FRACTION = 0.10
HB_STALL_MAX_ROWS = 4
F0_REPRODUCE_MIN = 2
MIN_REPS = 2
F2_MIN_RATIO = 2.0
F3_LATENCY_RATIO = 3.0
NS = {"p": "http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles"}


def duration(node):
    if node is None:
        return None
    sec = node.findtext("p:sec", "0", NS)
    nsec = node.findtext("p:nanosec", "0", NS)
    return float(sec) + float(nsec) / 1e9


def realized_profile(path):
    """The three parameters under test as the profile file sets them, defaults spelled out."""
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as exc:
        return f"profile unreadable ({exc})"
    w = root.find("p:data_writer[@is_default_profile='true']", NS)
    mbt = duration(w.find("p:qos/p:reliability/p:max_blocking_time", NS)) if w is not None else None
    hb = duration(w.find("p:times/p:heartbeatPeriod", NS)) if w is not None else None
    ms = w.findtext("p:topic/p:resourceLimitsQos/p:max_samples", None, NS) if w is not None else None
    return (f"max_blocking_time {f'{mbt:g} s' if mbt is not None else 'default 0.1 s'}, heartbeatPeriod "
            f"{f'{hb:g} s' if hb is not None else 'default 3 s'}, max_samples {ms or 'default 5000'}")


def treatment_problems(arm, treat_text, profiles):
    """Why this side's recorded environment is not the arm's treatment ([] when it is)."""
    if not treat_text.strip():
        return ["no treatment record"]
    problems = []
    for section in re.split(r"(?m)^(?=\[)", treat_text):
        if not section.strip() or "unreadable" in section.splitlines()[0]:
            continue
        env = dict(m.groups() for m in re.finditer(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$", section, re.M))
        name = section.splitlines()[0]
        for bad in ("RMW_FASTRTPS_USE_QOS_FROM_XML", "RMW_FASTRTPS_PUBLICATION_MODE"):
            if bad in env:
                problems.append(f"{name} {bad}={env[bad]}")
        if arm.startswith("fastdds@"):
            f = arm.split("@", 1)[1]
            want = f"/tmp/ka_fdds_{f}.xml"
            if env.get("RMW_IMPLEMENTATION") != "rmw_fastrtps_cpp":
                problems.append(f"{name} RMW_IMPLEMENTATION={env.get('RMW_IMPLEMENTATION')}")
            if env.get("FASTRTPS_DEFAULT_PROFILES_FILE") != want:
                problems.append(f"{name} FASTRTPS_DEFAULT_PROFILES_FILE={env.get('FASTRTPS_DEFAULT_PROFILES_FILE')}")
            m = re.search(r"^profile_sha256=(\w+) (\S+)$", section, re.M)
            if not m or m.group(1) != profiles.get(f) or m.group(2) != want:
                problems.append(f"{name} profile hash {m.group(1)[:12] if m else 'missing'} != "
                                f"{(profiles.get(f) or 'unknown')[:12]}")
        elif arm == "cyclonedds":
            if env.get("RMW_IMPLEMENTATION") != "rmw_cyclonedds_cpp" or "CYCLONEDDS_URI" not in env:
                problems.append(f"{name} not the cyclonedds environment")
    return problems


def stalls(sub):
    """(interior stall row count, longest consecutive stall run) of the subscriber's per-second rows."""
    live_idx = [i for i, r in enumerate(sub) if r.get("received", 0) > 0]
    if len(live_idx) < 3:
        return 0, 0
    first, last = live_idx[0], live_idx[-1]
    med = statistics.median(sub[i]["received"] for i in live_idx)
    count = run = longest = 0
    for r in sub[first + 1:last]:
        if r.get("received", 0) < STALL_FRACTION * med:
            count += 1
            run += 1
            longest = max(longest, run)
        else:
            run = 0
    return count, longest


def compare(a, b, floor):
    """(verdict, mean a, mean b, ratio b/a) with the 2 x SE and drift-floor rule."""
    ma, sa = mean_se(a)
    mb, sb = mean_se(b)
    diff, se = mb - ma, math.sqrt(sa ** 2 + sb ** 2)
    ratio = mb / ma if ma else float("inf")
    if diff > 2 * se and ratio > floor:
        return "BETTER", ma, mb, ratio
    if -diff > 2 * se and (1 / ratio if ratio else float("inf")) > floor:
        return "WORSE", ma, mb, ratio
    return "HELD", ma, mb, ratio


def load(runs):
    profiles = {}
    if (runs / "profiles.txt").exists():
        for line in (runs / "profiles.txt").read_text().splitlines():
            m = re.match(r"PROFILE (\S+) sha256=(\w+)", line)
            if m:
                profiles[m.group(1)] = m.group(2)
    index = {}
    for line in (runs / "index.txt").read_text().splitlines():
        m = re.match(INDEX_RE, line)
        if m:
            index[m.group(1)] = m.groups()[1:]
    out, voids = [], []
    for stem, (pmaps, smaps, pkilled, _skilled, want) in sorted(index.items()):
        arm, topic, loss, rep = re.match(r"(.+?)_(Array\w+|Struct\w+)_l(\d+)_r(\d+)$", stem).groups()
        sub, stext = rows(runs / f"{stem}_sub.log")
        pub, ptext = rows(runs / f"{stem}_pub.log")
        ptreat = (runs / f"{stem}_pub.treat").read_text() if (runs / f"{stem}_pub.treat").exists() else ""
        streat = (runs / f"{stem}_sub.treat").read_text() if (runs / f"{stem}_sub.treat").exists() else ""
        header = re.search(r"RMW Implementation: (\S+)", ptext)
        pub_id = loaded(want, pmaps) or bool(header and want.startswith("lib" + header.group(1)))
        pub_why, sub_why = [], []
        if not pub_id:
            pub_why.append(f"publisher did not load {want}")
        pub_why += ["pub " + p for p in treatment_problems(arm, ptreat, profiles)]
        if "XMLPARSER" in ptext:
            pub_why.append("publisher log has an XMLPARSER error")
        if not loaded(want, smaps):
            sub_why.append(f"subscriber did not load {want}")
        sub_why += ["sub " + p for p in treatment_problems(arm, streat, profiles)]
        if "XMLPARSER" in stext:
            sub_why.append("subscriber log has an XMLPARSER error")
        live = [r for r in sub if r.get("received", 0) > 0]
        if not live:
            sub_why.append("subscriber received nothing")
        refused = "failed to publish" in ptext
        errors = [mk for mk in ERROR_MARKERS if mk in ptext]
        errors += ["publisher killed at deadline"] if pkilled != "no" else []
        steady = live[2:-1] if len(live) > 4 else live
        # Whole counts (rmw_keepall_rig_summary.py, COUNTS): perf_test's received/sent/lost columns are per-second rates.
        lost_all, prematch, lost = loss_split(sub, sample_bytes(topic))
        whole_counts(pub)
        n_stall, longest = stalls(sub)
        rec = dict(stem=stem, arm=arm, topic=topic, loss=int(loss), rep=int(rep), refused=refused, errors=errors,
                   pub_ok=not pub_why, ok=not pub_why and not sub_why and not (errors and not refused),
                   rate=statistics.mean(r["received"] for r in steady) if steady else float("nan"),
                   lat=statistics.mean(r["latency_mean (ms)"] for r in steady) if steady else float("nan"),
                   lost=lost, prematch=prematch, lost_all=lost_all, stall_rows=n_stall, longest_stall=longest,
                   seconds=len(live), sent=sum(r["n_sent"] for r in pub))
        if pub_why or sub_why:
            voids.append(f"{stem}: " + "; ".join(pub_why + sub_why))
        out.append(rec)
    return out, voids, profiles


def pick(recs, arm, topic, loss):
    return [r for r in recs if r["arm"] == arm and r["topic"] == topic and r["loss"] == loss]


def usable(rs):
    return [r for r in rs if r["ok"] and not r["refused"]]


def describe(rs):
    """The realized inputs of a set of runs: how many, how many refused, rates, stalls."""
    u = usable(rs)
    rates = ", ".join(f"{r['rate']:.0f}" for r in u) or "-"
    st = ", ".join(f"r{r['rep']}:{r['stall_rows']}/{r['longest_stall']}" for r in rs if r["ok"]) or "-"
    refused = sum(r["refused"] for r in rs if r["pub_ok"])
    return f"{len(rs)} runs, {refused} refused, {len(u)} usable; rates/s [{rates}]; stall rows/longest [{st}]"


def main():
    runs, dur = Path(sys.argv[1]), int(sys.argv[2])
    recs, voids, profiles = load(runs)
    arms = sorted({r["arm"] for r in recs})

    print(f"\n=== realized treatments ({runs}) ===")
    for f in sorted(profiles):
        print(f"  fastdds@{f}: sha256 {profiles[f][:16]}  {realized_profile(runs / f'profile_{f}.xml')}")
    if not profiles:
        print("  NO VERDICT on any Fast DDS arm: profiles.txt is missing - which profile each arm ran is unknown")

    print(f"\n=== per run, {dur} s publisher (delivered msgs/s over steady seconds; lost = after the first delivered "
          "second; stalls = interior rows under 10% of the median) ===")
    for r in sorted(recs, key=lambda r: (r["topic"], r["loss"], r["arm"], r["rep"])):
        print(f"{r['topic']:8} l{r['loss']:<2} {r['arm']:11} r{r['rep']} rate {r['rate']:9.1f}/s  "
              f"lat {r['lat']:8.2f} ms  lost {r['lost']:6} (pre-match {r['prematch']:7}, old all-rows "
              f"{r['lost_all']:7})  stall rows {r['stall_rows']:2} longest {r['longest_stall']}  "
              f"{'REFUSED after ' + str(r['seconds']) + ' s ' if r['refused'] else ''}"
              f"{'ERR ' + ','.join(r['errors']) + ' ' if r['errors'] else ''}{'' if r['ok'] else 'VOID'}")
    if voids:
        print("\nVOID runs:")
        for v in voids:
            print("  " + v)

    def floor_for(topic, loss):
        c = [r["rate"] for r in usable(pick(recs, "cyclonedds", topic, loss))]
        return (max(c) / min(c), c) if len(c) >= MIN_REPS else (None, c)

    print("\n=== verdicts ===")
    lossy_topics = sorted({r["topic"] for r in recs if r["loss"] > 0})
    if not lossy_topics:
        print("NO VERDICT: no lossy cell was run")
    for topic in lossy_topics:
        loss = max(r["loss"] for r in recs if r["topic"] == topic)
        f0 = [r for r in pick(recs, "fastdds@F0", topic, loss) if r["pub_ok"]]
        n_ref = sum(r["refused"] for r in f0)
        tag = f"{topic} l{loss}"
        if not f0:
            print(f"{tag} F0: NO VERDICT - no F0 publisher identified with its verified profile (VOID runs); "
                  "F1/F2 unreadable")
            continue
        if n_ref < F0_REPRODUCE_MIN:
            print(f"{tag} F0: VOID - the timeout did not reproduce ({n_ref} of {len(f0)} identified runs refused, "
                  f"need {F0_REPRODUCE_MIN}); F1 and F2 at {tag} test nothing")
            continue
        print(f"{tag} F0: REPRODUCED - {n_ref} of {len(f0)} identified runs refused  [{describe(f0)}]")

        f1_all = pick(recs, "fastdds@F1", topic, loss)
        f1 = [r for r in f1_all if r["pub_ok"]]
        f1_ok = [r for r in f1 if r["ok"]]
        ctl0 = [r for r in pick(recs, "fastdds@F1", topic, 0) if r["ok"]]
        ctl0_stalls = sum(r["stall_rows"] > 0 for r in ctl0)
        ctl_txt = (f"0% control: {ctl0_stalls} of {len(ctl0)} F1 runs stall" if ctl0
                   else "0% control: not run for this topic")
        if not f1:
            print(f"{tag} F1: NO VERDICT - no F1 publisher identified with its verified profile (VOID runs)")
        elif any(r["refused"] for r in f1):
            print(f"{tag} F1: FALSIFIED - F1 still timed out with max_blocking_time 5 s  [{describe(f1_all)}]")
        elif len(f1_ok) < MIN_REPS:
            print(f"{tag} F1: NO VERDICT - {len(f1_ok)} usable runs  [{describe(f1_all)}]")
        elif ctl0_stalls >= MIN_REPS:
            print(f"{tag} F1: VOID stall reading - F1 stalls without loss too ({ctl_txt})  [{describe(f1_all)}]")
        else:
            with_stall = sum(r["stall_rows"] > 0 for r in f1_ok)
            too_long = sum(r["longest_stall"] > HB_STALL_MAX_ROWS for r in f1_ok)
            if with_stall == 0:
                v = "FALSIFIED - no timeout, but no stall either: the 3 s heartbeat is not what the writer waited on"
            elif too_long >= MIN_REPS:
                v = f"FALSIFIED - {too_long} runs stall longer than {HB_STALL_MAX_ROWS} s, more than a 3 s heartbeat"
            elif with_stall >= MIN_REPS:
                v = f"SUPPORTED - no timeout, {with_stall} of {len(f1_ok)} runs stall"
            else:
                v = f"INCONCLUSIVE - no timeout, {with_stall} of {len(f1_ok)} runs stall"
            print(f"{tag} F1: {v}  [{describe(f1_all)}; {ctl_txt}]")

        f2_all = pick(recs, "fastdds@F2", topic, loss)
        f2 = [r for r in f2_all if r["pub_ok"]]
        f2_ok = [r for r in f2 if r["ok"]]
        floor, crates = floor_for(topic, loss)
        if not f2:
            print(f"{tag} F2: NO VERDICT - no F2 publisher identified with its verified profile (VOID runs)")
        elif any(r["refused"] for r in f2):
            print(f"{tag} F2: FALSIFIED - F2 timed out  [{describe(f2_all)}]")
        elif len(f2_ok) < MIN_REPS or len(usable(f1)) < MIN_REPS or floor is None:
            print(f"{tag} F2: NO VERDICT - usable runs F2 {len(f2_ok)}, F1 {len(usable(f1))}, cyclonedds "
                  f"{len(crates)} (need {MIN_REPS} each)  [{describe(f2_all)}]")
        else:
            v, m1, m2, ratio = compare([r["rate"] for r in usable(f1)], [r["rate"] for r in f2_ok], floor)
            still = sum(r["stall_rows"] > 0 for r in f2_ok)
            why = []
            if still >= MIN_REPS:
                why.append(f"{still} runs still stall")
            if v != "BETTER" or ratio < F2_MIN_RATIO:
                why.append(f"rate {v} at {ratio:.2f}x, needs BETTER and >= {F2_MIN_RATIO:g}x")
            verdict = ("FALSIFIED - " + "; ".join(why)) if why else f"SUPPORTED - {ratio:.2f}x F1 and no stalls"
            print(f"{tag} F2: {verdict}  [F2 {m2:.0f}/s vs F1 {m1:.0f}/s, drift floor {floor:.3f}x; "
                  f"{describe(f2_all)}]")

    f0 = pick(recs, "fastdds@F0", "Array1k", 0)
    f3 = pick(recs, "fastdds@F3", "Array1k", 0)
    floor, crates = floor_for("Array1k", 0)
    if len(usable(f0)) < MIN_REPS or len(usable(f3)) < MIN_REPS or floor is None:
        print(f"Array1k l0 F3: NO VERDICT - usable runs F0 {len(usable(f0))}, F3 {len(usable(f3))}, cyclonedds "
              f"{len(crates)} (need {MIN_REPS} each)")
    else:
        v, m0, m3, ratio = compare([r["rate"] for r in usable(f0)], [r["rate"] for r in usable(f3)], floor)
        l0 = statistics.mean(r["lat"] for r in usable(f0))
        l3 = statistics.mean(r["lat"] for r in usable(f3))
        lr = l3 / l0 if l0 else float("inf")
        if v == "BETTER":
            verdict = "WRITER HISTORY WAS THE LIMIT - a larger history raised the delivered rate"
        elif v == "WORSE":
            verdict = "UNEXPECTED - F3 is slower than F0; reported, not read"
        elif lr >= F3_LATENCY_RATIO:
            verdict = "SUBSCRIBER-SIDE BOTTLENECK - same rate, latency grew with the history (a standing queue)"
        else:
            verdict = "NEITHER - same rate and latency did not grow with the history; unexplained"
        print(f"Array1k l0 F3: {verdict}  [F3 {m3:.0f}/s vs F0 {m0:.0f}/s = {ratio:.3f}x {v}, drift floor "
              f"{floor:.3f}x; latency F3 {l3:.1f} ms vs F0 {l0:.1f} ms = {lr:.1f}x; F0 {describe(f0)}; "
              f"F3 {describe(f3)}]")

    print("\nnot judged, for the record:")
    for a in arms:
        for topic, loss in sorted({(r["topic"], r["loss"]) for r in recs}):
            rs = pick(recs, a, topic, loss)
            if rs:
                print(f"  {a:11} {topic} l{loss}: {describe(rs)}")


if __name__ == "__main__":
    main()
