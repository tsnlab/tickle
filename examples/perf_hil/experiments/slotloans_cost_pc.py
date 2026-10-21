#!/usr/bin/env python3
"""slotloans_cost_pc.py - the reading of slotloans_cost_pc.sh (its header is the pre-registration).

Usage: slotloans_cost_pc.py RUNS_DIR LABEL... (the first label is the reference arm). Exit 0 always; the verdict is
printed: per arm against the first, WORSE / BETTER / SAME per metric, VOID with the reason.
"""
import math
import os
import re
import sys
from collections import defaultdict
from statistics import mean, stdev

from scipy import stats

ALPHA = 0.05


def perf(path):
    """instructions and cycles (user + kernel) from a perf stat -x, file, or None."""
    insn = cyc = 0
    seen = set()
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                p = line.strip().split(",")
                if len(p) < 3 or not p[0].replace(".", "").isdigit():
                    continue
                ev = p[2]
                if ev.startswith("instructions"):
                    insn += float(p[0])
                    seen.add(ev)
                elif ev.startswith("cycles"):
                    cyc += float(p[0])
                    seen.add(ev)
    except OSError:
        return None
    return (insn, cyc) if len(seen) == 4 else None


def text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def counters(t, *names):
    out = {}
    for n in names:
        vals = [int(v) for v in re.findall(r"\b" + n + r"=(\d+)", t)]
        out[n] = max(vals) if vals else 0
    return out


def perf_test_rows(t, col):
    total = 0
    for line in t.splitlines():
        p = [x.strip() for x in line.split(",") if x.strip() != ""]
        if len(p) > 10 and re.match(r"^\d+\.\d+$", p[0]):
            try:
                total += int(float(p[col]))
            except ValueError:
                pass
    return total


def read_run(d, cell):
    """(metrics dict role -> (insn/sample, cyc/sample), info dict) or (None, why)."""
    inst = text(os.path.join(d, "install.txt")).strip()
    maps = text(os.path.join(d, "pong.maps" if cell.startswith("rtt") else "sub.maps")).split()
    if not inst or maps != [inst + "/rmw_tickle/lib/librmw_tickle.so"]:
        return None, "identity: maps %s, arm install %s" % (maps, inst)
    if cell.startswith("rtt"):
        ping, pong = text(os.path.join(d, "ping.log")), text(os.path.join(d, "pong.log"))
        m = re.search(r"^RESULT: .*?sent=(\d+) recv=(\d+).*?rtt_p50_ms=([\d.]+)", ping, re.M)
        if not m:
            return None, "no RESULT"
        n = int(m.group(1))
        info = {"rts": n, "recv": int(m.group(2)), "p50_us": float(m.group(3)) * 1000}
        both = ping + pong
        info.update(counters(both, "loans_in_slot", "slot_loans_copied", "shm_held_passed", "shm_refs_read",
                             "shm_claims_published", "shm_claims_in_place"))
        info["in_slot_sum"] = sum(int(v) for v in re.findall(r"loans_in_slot=(\d+)", both))
        if info["in_slot_sum"] == 0:
            return None, "treatment absent: loans_in_slot 0"
        res = {}
        for role in ("ping", "pong"):
            p = perf(os.path.join(d, role + ".perf"))
            if p is None:
                return None, "no perf counts for " + role
            res[role] = (p[0] / n, p[1] / n)
        return res, info
    pub, sub = text(os.path.join(d, "pub.log")), text(os.path.join(d, "sub.log"))
    sent, recv = perf_test_rows(pub, 3), perf_test_rows(sub, 2)
    if sent == 0 or recv == 0:
        return None, "no samples (sent %d recv %d)" % (sent, recv)
    info = {"sent": sent, "recv": recv}
    info.update(counters(sub, "rx_shm", "missed_samples", "loans_copied", "rx_shm_skipped_superseded"))
    if info["rx_shm"] == 0:
        return None, "no traffic line from the sub (rx_shm 0)"
    res = {}
    for role, n in (("pub", sent), ("sub", info["rx_shm"])):
        p = perf(os.path.join(d, role + ".perf"))
        if p is None:
            return None, "no perf counts for " + role
        res[role] = (p[0] / n, p[1] / n)
    return res, info


def welch(a, b):
    if len(a) < 2 or len(b) < 2:
        return float("nan"), float("nan")
    va, vb = stdev(a) ** 2 / len(a), stdev(b) ** 2 / len(b)
    se = math.sqrt(va + vb)
    if se == 0:
        return float("inf") if mean(b) != mean(a) else 0.0, 1.0
    t = (mean(b) - mean(a)) / se
    df = (va + vb) ** 2 / ((va ** 2) / (len(a) - 1) + (vb ** 2) / (len(b) - 1))
    return t, df


def main():
    runs, labels = sys.argv[1], sys.argv[2:]
    data = defaultdict(lambda: defaultdict(list))  # (cell, role, kind) -> label -> values
    infos = defaultdict(list)
    void = []
    for name in sorted(os.listdir(runs)):
        m = re.match(r"^(rtt_be|rtt_rel|tput_l|tput_d)_(\w+?)_r\d+_\d+$", name)
        if not m:
            continue
        cell, label = m.group(1), m.group(2)
        res, info = read_run(os.path.join(runs, name), cell)
        if res is None:
            void.append("%s: %s" % (name, info))
            continue
        infos[(cell, label)].append(info)
        for role, (i, c) in res.items():
            data[(cell, role, "insn")][label].append(i)
            data[(cell, role, "cyc")][label].append(c)
    print("SAMPLE COUNTS (per run):")
    for (cell, label), lst in sorted(infos.items()):
        print("  %-7s %-4s runs %d: %s" % (cell, label, len(lst), "; ".join(
            " ".join("%s=%s" % (k, ("%.1f" % v) if isinstance(v, float) else v) for k, v in i.items()) for i in lst)))
    for v in void:
        print("  VOID run " + v)
    ref = labels[0]
    insn_keys = [k for k in sorted(data) if k[2] == "insn"]
    nmetrics = max(1, len(insn_keys) * max(1, len(labels) - 1))
    print("\nPER SAMPLE (mean over runs; Welch t vs %s; |t| threshold Bonferroni over %d):" % (ref, nmetrics))
    verdict = {}
    control_void = []
    for key in sorted(data):
        cell, role, kind = key
        a = data[key].get(ref, [])
        line = "  %-7s %-4s %-4s %s %.0f (n %d)" % (cell, role, kind, ref, mean(a) if a else float("nan"), len(a))
        for lab in labels[1:]:
            b = data[key].get(lab, [])
            if len(a) < 2 or len(b) < 2:
                line += " | %s VOID (n %d)" % (lab, len(b))
                verdict.setdefault(lab, []).append("VOID")
                continue
            t, df = welch(a, b)
            thr = stats.t.ppf(1 - ALPHA / (2 * nmetrics), df) if df == df else float("inf")
            d = mean(b) - mean(a)
            word = ""
            if kind == "insn":
                word = "WORSE" if d > 0 and abs(t) > thr else "BETTER" if d < 0 and abs(t) > thr else "SAME"
                if cell == "tput_d" and word != "SAME":
                    control_void.append("%s %s %s" % (cell, role, lab))
                elif cell != "tput_d":
                    verdict.setdefault(lab, []).append(word)
            line += " | %s %.0f (n %d) %+.0f (%+.2f%%) t %+.2f thr %.2f %s" % (lab, mean(b), len(b), d, 100 * d / mean(a),
                                                                               t, thr, word)
        print(line)
    print("\nrtt p50 (us, informative):")
    for (cell, label), lst in sorted(infos.items()):
        if cell.startswith("rtt"):
            print("  %-7s %-4s %s" % (cell, label, " ".join("%.1f" % i["p50_us"] for i in lst)))
    for lab in labels[1:]:
        if control_void:
            print("VERDICT %s vs %s: VOID (control moved: %s)" % (lab, ref, ", ".join(control_void)))
        else:
            v = verdict.get(lab, [])
            word = "VOID" if "VOID" in v else "WORSE" if "WORSE" in v else "BETTER" if "BETTER" in v else "SAME"
            print("VERDICT %s vs %s: %s (%s)" % (lab, ref, word, " ".join(v)))


if __name__ == "__main__":
    main()
