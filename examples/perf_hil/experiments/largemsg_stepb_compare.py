#!/usr/bin/env python3
"""Step B's rig reading (docs/DESIGN.md section 8, "Step B pre-registration", B2), written before any rig run.

Input: the summary files rmw_keepall_rig.sh writes through largemsg_rig_summary.py (OUT.txt), one per QoS, each with
per-run lines "<topic> l<loss> <arm> r<n> sent .. recv .. (<pct>%) .. lat med <ms> .. cpu sub <ms/MB> pub <ms/MB>
rss <sub kB>/<pub kB> ..". Arms: --main (step A, default tickle@pre) and --stepb (default tickle@head); the vendor arms
are whatever else appears. A run whose row says VOID or lacks a field is not used.

The rules, as B2 states them:
  CONTROL  the --control topic (default Array16k: below 64 KB, no step B code on its path): stepb - main for latency
           median and sub + pub CPU per MB within 2 x SE of the difference of means. Otherwise the session is VOID.
  B2.1     BEST_EFFORT Array1m / Array4m: step B delivered >= 99.9% in every run.
  B2.2     RELIABLE: step B delivered 100% in every run.
  B2.3     RELIABLE cells: step B's sub + pub CPU per MB below main's beyond 2 x SE. Failing it, L3 drops step B.
  B2.4     latency median: step B not above main beyond 2 x SE.
  B2.5     against each vendor: delivered %, latency median, sub and pub CPU per MB, peak RSS - WIN / DRAW / LOSE by
           2 x SE (lower is better except delivered).
  B2.6     peak RSS (sub, pub): step B not above main by more than one sample's size (reported, not blocking).
Fewer than 3 usable runs of an arm in a cell: that cell is NO VERDICT. Exit status 0 when the control holds and
B2.1-B2.4 pass, 1 when any fails, 2 when the session is VOID or a cell is NO VERDICT.

--selftest checks the reader on a synthetic table built to give known answers (a passing step B, a failing one, a
moving control) and must print SELFTEST PASS before the reader is trusted with rig files.
"""
import argparse
import math
import re
import sys

ROW = re.compile(
    r"^(?P<topic>Array\w+)\s+l(?P<loss>\d+)\s+(?P<arm>\S+)\s+r(?P<rep>\d+)\s+sent\s+(?P<sent>\d+)\s+recv\s+(?P<recv>\d+)"
    r".*?\(\s*(?P<pct>[0-9.]+)%\).*?lat med\s+(?P<lat>[0-9.]+).*?cpu sub\s+(?P<sub>[0-9.]+)\s+pub\s+(?P<pub>[0-9.]+)"
    r"\s+rss\s+(?P<sub_rss>\d+)/\s*(?P<pub_rss>\d+)")
SIZES_KB = {"Array16k": 16, "Array1m": 1024, "Array4m": 4096}


def parse(path, qos):
    runs = []
    for line in open(path):
        if "VOID" in line:
            continue
        m = ROW.match(line)
        if m:
            d = m.groupdict()
            runs.append(dict(qos=qos, topic=d["topic"], arm=d["arm"], pct=float(d["pct"]), lat=float(d["lat"]),
                             cpu=float(d["sub"]) + float(d["pub"]), sub_cpu=float(d["sub"]), pub_cpu=float(d["pub"]),
                             sub_rss=float(d["sub_rss"]), pub_rss=float(d["pub_rss"])))
    return runs


def mean_se(xs):
    m = sum(xs) / len(xs)
    if len(xs) < 2:
        return m, float("inf")
    return m, math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1) / len(xs))


def diff(a, b):
    """(mean(b) - mean(a), 2 x SE of that difference)."""
    (ma, sa), (mb, sb) = mean_se(a), mean_se(b)
    return mb - ma, 2 * math.sqrt(sa * sa + sb * sb)


def versus(ours, theirs, lower_is_better=True):
    d, bound = diff(theirs, ours)
    if abs(d) <= bound:
        return "DRAW"
    return "WIN" if (d < 0) == lower_is_better else "LOSE"


def read(runs, main, stepb, control, out):
    cells = {}
    for r in runs:
        cells.setdefault((r["qos"], r["topic"]), {}).setdefault(r["arm"], []).append(r)
    status = 0
    session_void = False
    for (qos, topic), arms in sorted(cells.items()):
        a, b = arms.get(main, []), arms.get(stepb, [])
        if len(a) < 3 or len(b) < 3:
            print(f"{qos} {topic}: NO VERDICT - {len(a)} usable {main}, {len(b)} usable {stepb} runs", file=out)
            status = max(status, 2)
            if topic == control:
                session_void = True
            continue
        get = lambda rs, k: [r[k] for r in rs]
        if topic == control:
            ok = True
            for key in ("lat", "cpu"):
                d, bound = diff(get(a, key), get(b, key))
                held = abs(d) <= bound
                ok = ok and held
                print(f"CONTROL {qos} {topic} {key}: {stepb} - {main} = {d:+.3f} (2 x SE {bound:.3f}) -> "
                      f"{'HELD' if held else 'MOVED'}", file=out)
            if not ok:
                session_void = True
            continue
        reliable = qos.lower().startswith("rel")
        floor = 100.0 if reliable else 99.9
        delivered_ok = all(r["pct"] >= floor for r in b)
        tag = "B2.2" if reliable else "B2.1"
        print(f"{tag} {qos} {topic}: {stepb} delivered min {min(get(b, 'pct')):.2f}% (needs >= {floor}%) -> "
              f"{'PASS' if delivered_ok else 'FAIL'}", file=out)
        status = max(status, 0 if delivered_ok else 1)
        d, bound = diff(get(a, "cpu"), get(b, "cpu"))
        if reliable:
            lower = d < -bound
            print(f"B2.3 {qos} {topic}: sub+pub CPU/MB {mean_se(get(a, 'cpu'))[0]:.3f} -> "
                  f"{mean_se(get(b, 'cpu'))[0]:.3f} ({d:+.3f}, 2 x SE {bound:.3f}) -> "
                  f"{'PASS' if lower else 'FAIL: L3 drops step B'}", file=out)
            status = max(status, 0 if lower else 1)
        else:
            print(f"     {qos} {topic}: sub+pub CPU/MB {mean_se(get(a, 'cpu'))[0]:.3f} -> "
                  f"{mean_se(get(b, 'cpu'))[0]:.3f} ({d:+.3f}, 2 x SE {bound:.3f}) (main delivered "
                  f"{mean_se(get(a, 'pct'))[0]:.2f}%: per delivered MB, not a like-for-like)", file=out)
        d, bound = diff(get(a, "lat"), get(b, "lat"))
        worse = d > bound
        print(f"B2.4 {qos} {topic}: latency median {mean_se(get(a, 'lat'))[0]:.2f} -> {mean_se(get(b, 'lat'))[0]:.2f}"
              f" ms ({d:+.2f}, 2 x SE {bound:.2f}) -> {'FAIL' if worse else 'PASS'}", file=out)
        status = max(status, 1 if worse else 0)
        size_kb = SIZES_KB.get(topic, 0)
        for key in ("sub_rss", "pub_rss"):
            rise = max(get(b, key)) - max(get(a, key))
            print(f"B2.6 {qos} {topic} {key}: peak {max(get(a, key)):.0f} -> {max(get(b, key)):.0f} kB ({rise:+.0f}; "
                  f"one sample {size_kb} kB) -> {'LOSE' if rise > size_kb else 'ok'}", file=out)
        for vendor in sorted(v for v in arms if v not in (main, stepb)):
            v = arms[vendor]
            if len(v) < 3:
                print(f"B2.5 {qos} {topic} vs {vendor}: NO VERDICT ({len(v)} runs)", file=out)
                continue
            verdicts = [f"delivered {versus(get(b, 'pct'), get(v, 'pct'), lower_is_better=False)}"]
            for key, name in (("lat", "latency"), ("sub_cpu", "sub CPU/MB"), ("pub_cpu", "pub CPU/MB"),
                              ("sub_rss", "sub RSS"), ("pub_rss", "pub RSS")):
                verdicts.append(f"{name} {versus(get(b, key), get(v, key))}")
            print(f"B2.5 {qos} {topic} vs {vendor}: " + ", ".join(verdicts), file=out)
    if session_void:
        print("SESSION: VOID - the control moved or could not be read", file=out)
        return 2
    print("SESSION:", {0: "PASS", 1: "FAIL", 2: "INCOMPLETE"}[status], file=out)
    return status


def synthetic(cpu_b, lat_b, ctrl_shift, pct_b=100.0):
    lines = []
    for topic, cpu, lat in (("Array16k", 3.0, 0.1), ("Array4m", 2.5, 7.7)):
        for arm in ("tickle@pre", "tickle@head", "fastdds"):
            for rep in range(1, 6):
                jitter = 0.01 * ((rep * 7) % 5 - 2)
                c, l, p = cpu + jitter, lat + jitter, 100.0
                if arm == "tickle@head":
                    if topic == "Array4m":
                        c, l, p = cpu_b + jitter, lat_b + jitter, pct_b
                    else:
                        c, l = c + ctrl_shift, l + ctrl_shift
                lines.append(f"{topic}  l0  {arm:16s} r{rep} sent  1742 recv  1742 lost    0 end   0 ({p:6.2f}%)  "
                             f"29.5 Hz  lat med {l:8.2f} p99>= 1.0 max 2.0  excess p99>= 0.5 mean 0.2  cpu sub "
                             f"{c / 2:6.2f} pub {c / 2:6.2f}  rss 20000/ 18000  [one host]")
    return lines


def selftest():
    import io
    import os
    import tempfile
    cases = [("a passing step B", (1.2, 3.0, 0.0), 0, "SESSION: PASS"),
             ("step B paying nothing", (2.5, 7.7, 0.0), 1, "B2.3 reliable Array4m"),
             ("a moving control", (1.2, 3.0, 0.5), 2, "SESSION: VOID"),
             ("an incomplete RELIABLE delivery", (1.2, 3.0, 0.0, 99.0), 1, "B2.2 reliable Array4m")]
    ok = True
    for name, args, want_rc, want_text in cases:
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write("\n".join(synthetic(*args)) + "\n")
        out = io.StringIO()
        rc = read(parse(f.name, "reliable"), "tickle@pre", "tickle@head", "Array16k", out)
        os.unlink(f.name)
        text = out.getvalue()
        passed = rc == want_rc and want_text in text and (want_rc != 1 or "FAIL" in text)
        ok = ok and passed
        print(f"selftest {name}: rc {rc} (want {want_rc}) -> {'ok' if passed else 'WRONG'}")
        if not passed:
            print(text)
    print("SELFTEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="*", help="QOS=summary.txt, QOS 'reliable' or 'best_effort'")
    ap.add_argument("--main", default="tickle@pre")
    ap.add_argument("--stepb", default="tickle@head")
    ap.add_argument("--control", default="Array16k")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    runs = []
    for spec in args.files:
        qos, path = spec.split("=", 1)
        runs += parse(path, qos)
    if not runs:
        print("no per-run rows found: NO VERDICT")
        return 2
    return read(runs, args.main, args.stepb, args.control, sys.stdout)


if __name__ == "__main__":
    sys.exit(main())
