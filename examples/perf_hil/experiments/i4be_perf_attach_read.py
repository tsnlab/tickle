#!/usr/bin/env python3
"""Read i4be_cpu_gap.sh's runs: where rmw_tickle's extra kernel time per sample goes in I4 best effort.

Input: <MAIN>.runs (tickle@head, tickle@aa, fastdds; rmw_keepall_rig.sh PERF_ATTACH), optionally --pin <PIN>.runs
(tickle@head, fastdds under PIN_CPUS) and --baseline <runs> (an uninstrumented I4 best-effort run set, the L4 one).

Per run and side (pub, sub) over the attach window: perf stat --per-thread counts summed over perf_test's threads,
divided by the samples in the window (the subscriber's steady delivered rate x the window), and the thread table.

HOW TO READ IT - written before the first run, enforced below:
  VOID run      a .pstat missing, empty, missing an event, or with an event "<not supported>" on either side (a thread
                that did not run in the window reads "<not counted>", which is zero); or the
                subscriber's steady rate off 15 Hz by more than 1%; or the .psnap begin/end missing.
  INSTRUMENT    (a) per run and side, perf's task-clock over the window and the process's own utime+stime over the same
                window (.psnap, tick-sampled) agree within 10% + 2 ticks per thread; a run that fails is VOID (perf did
                not see the whole process, e.g. a thread it did not attach to).
                (b) the rusage gap tickle@head - fastdds in ms/MB (steady window, as largemsg_rig_summary.py) under the
                instrument keeps the sign of the uninstrumented --baseline gap and lies within 50% of it + 0.03 ms/MB,
                per side; otherwise the instrument changed what is being explained and every attribution on that side
                prints VOID.
  A/A FLOOR     tickle@aa is tickle@head's binary: for each metric, floor = |median(aa) - median(head)|. A difference
                head - fastdds is REAL only when it exceeds both 2 x floor and 2 x its standard error (reps); else
                "within floor".
  HYPOTHESES    (falsifiable; each prints SUPPORTED, FALSIFIED or UNDECIDED)
    H1 syscalls   rmw_tickle makes more syscalls per sample: exc_svc diff REAL and > 0. FALSIFIED if REAL and <= 0
                  or within floor while the kernel-cycle gap is REAL.
    H2 interrupts rmw_tickle's threads take more of the host's IRQs (and their softirq time) per sample: exc_irq diff
                  REAL and > 0 on that side. Causal half, needs --pin: the task-clock gap per sample shrinks by at
                  least half when every perf_test runs off CPU 0 (eth0's IRQ CPU). FALSIFIED if the exc_irq diff is
                  REAL and <= 0, or the pinned gap is not smaller than the unpinned one.
    H3 kernel IPC rmw_tickle does about the same kernel work at a lower IPC (stalls, e.g. a copy into cold memory):
                  instructions:k diff within 10% of fastdds's while cycles:k diff REAL and > 0 and kernel IPC lower
                  by > 10%. FALSIFIED if kernel IPC is not lower by > 5%.
  ATTRIBUTION   kernel cycles per sample are split by the per-thread table; the extra kernel cycles of rmw_tickle are
                set against (exc_svc diff) and (exc_irq diff) with each arm's own kernel instructions per syscall and
                per IRQ printed, so the reader sees which term could carry the gap.
"""
import argparse
import math
import re
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rmw_keepall_rig_summary import rows, sample_bytes, whole_counts  # noqa: E402

RATE = 15.0
SIZE = 4194304
WARM, COOL = 2, 1
USER_HZ = 100
EVENTS = ("task-clock", "context-switches", "cpu-migrations", "instructions:u", "instructions:k", "cycles:u",
          "cycles:k", "exc_svc", "exc_irq")


def steady(rs, key):
    live = [i for i, r in enumerate(rs) if r.get(key, 0) > 0]
    if len(live) <= WARM + COOL + 1:
        return None
    idx = live[WARM:-COOL]
    if idx[0] == 0:
        return None
    n = sum(rs[i][key] for i in idx)
    dt = sum(rs[i]["D"] for i in idx)
    return rs[idx[0] - 1], rs[idx[-1]], n, dt


def read_pstat(path):
    """{tid: {"comm": c, event: value}} or (None, why)."""
    try:
        text = path.read_text()
    except OSError:
        return None, "no .pstat"
    threads = {}
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        f = line.split(",")
        if len(f) < 4:
            continue
        who, val, ev = f[0], f[1], f[3]
        if "not supported" in val:
            return None, f"{ev} {val}"
        if "not counted" in val:  # a thread that did not run in the window (perf prints it so): zero
            val = "0"
        m = re.match(r"(.*)-(\d+)$", who.strip())
        if not m:
            continue
        d = threads.setdefault(int(m.group(2)), {"comm": m.group(1)})
        try:
            d[ev] = float(val)
        except ValueError:
            return None, f"{ev} unreadable {val!r}"
    if not threads:
        return None, "empty .pstat"
    seen = set().union(*(set(d) for d in threads.values()))
    missing = [e for e in EVENTS if e not in seen]
    if missing:
        return None, "missing " + " ".join(missing)
    return threads, ""


def read_psnap(path):
    """{"begin"/"end": {"t": ns, "cpu": {name: [fields]}, "softirqs": {name: [per cpu]}, "eth0": [per cpu],
    "tasks": {tid: (comm, utime, stime)}}}."""
    try:
        text = path.read_text()
    except OSError:
        return None
    out, cur, sect = {}, None, None
    for line in text.splitlines():
        m = re.match(r"--- (begin|end) (\d+)$", line)
        if m:
            cur = out.setdefault(m.group(1), {"t": int(m.group(2)), "cpu": {}, "softirqs": {}, "eth0": [], "tasks": {}})
            sect = "stat"
            continue
        if cur is None:
            continue
        if line.startswith("--- "):
            sect = line[4:].strip()
            continue
        if sect == "stat" and line.startswith("cpu"):
            p = line.split()
            cur["cpu"][p[0]] = [int(x) for x in p[1:]]
        elif sect == "softirqs" and ":" in line:
            name, _, rest = line.partition(":")
            cur["softirqs"][name.strip()] = [int(x) for x in rest.split()]
        elif sect == "interrupts" and "eth0" in line:
            cur["eth0"] = [int(x) for x in line.split()[1:] if x.isdigit()]
        elif sect == "tasks":
            tid, _, st_line = line.partition(" ")
            if ")" not in st_line:
                continue
            comm = st_line[st_line.index("(") + 1: st_line.rindex(")")]
            rest = st_line[st_line.rindex(")") + 2:].split()
            cur["tasks"][int(tid)] = (comm, int(rest[11]), int(rest[12]))
    return out if "begin" in out and "end" in out else None


def read_pplace(path):
    """{tid: (running samples, of them on CPU 0)}."""
    out = {}
    try:
        for line in path.read_text().splitlines():
            p = line.split()
            if len(p) == 3 and p[1] == "R":
                r, c0 = out.get(int(p[0]), (0, 0))
                out[int(p[0])] = (r + 1, c0 + (p[2] == "0"))
    except OSError:
        pass
    return out


def rusage_gap_side(runs, stem, side):
    log, _ = rows(runs / f"{stem}_{side}.log")
    whole_counts(log, sample_bytes("Array4m") if side == "sub" else None)
    s = steady(log, "n_received" if side == "sub" else "n_sent")
    if not s:
        return float("nan")
    b, e, n, _ = s
    return ((e["ru_utime"] + e["ru_stime"]) - (b["ru_utime"] + b["ru_stime"])) * 1e3 / (n * SIZE / 1e6)


def run(runs, stem, window):
    sub, _ = rows(runs / f"{stem}_sub.log")
    whole_counts(sub, sample_bytes("Array4m"))
    s = steady(sub, "n_received")
    if not s:
        return None, "no steady subscriber window"
    rate = s[2] / s[3]
    if abs(rate - RATE) > 0.01 * RATE:
        return None, f"subscriber rate {rate:.2f} Hz"
    n = rate * window
    rec = {"rate": rate, "n": n}
    for side in ("pub", "sub"):
        th, why = read_pstat(runs / f"{stem}_{side}.pstat")
        if th is None:
            return None, f"{side}: {why}"
        snap = read_psnap(runs / f"{stem}_{side}.psnap")
        if snap is None:
            return None, f"{side}: no .psnap begin/end"
        place = read_pplace(runs / f"{stem}_{side}.pplace")
        tot = {e: sum(d.get(e, 0.0) for d in th.values()) for e in EVENTS}
        # Instrument (a): perf's task-clock (ms) against the process's own ticks over the window.
        b, e = snap["begin"]["tasks"], snap["end"]["tasks"]
        ticks = sum((e[t][1] + e[t][2]) - (b[t][1] + b[t][2]) for t in e if t in b)
        tick_ms = ticks * 1000.0 / USER_HZ
        tol = 0.10 * tot["task-clock"] + 2 * len(e) * 1000.0 / USER_HZ
        if abs(tick_ms - tot["task-clock"]) > tol:
            return None, f"{side}: task-clock {tot['task-clock']:.0f} ms vs own ticks {tick_ms:.0f} ms (instrument a)"
        for k, v in tot.items():
            rec[f"{side}_{k}"] = v / n
        rec[f"{side}_utime_ms"] = sum(e[t][1] - b[t][1] for t in e if t in b) * 1000.0 / USER_HZ / n
        rec[f"{side}_stime_ms"] = sum(e[t][2] - b[t][2] for t in e if t in b) * 1000.0 / USER_HZ / n
        # Host: softirq/irq ticks per CPU, NET_RX/NET_TX and eth0 IRQs per CPU, per sample.
        cb, ce = snap["begin"]["cpu"], snap["end"]["cpu"]
        for cpu in sorted(k for k in ce if k != "cpu"):
            if cpu in cb:
                rec[f"{side}_host_{cpu}_softirq_ticks"] = (ce[cpu][6] - cb[cpu][6]) / n
                rec[f"{side}_host_{cpu}_irq_ticks"] = (ce[cpu][5] - cb[cpu][5]) / n
        for name in ("NET_RX", "NET_TX", "TIMER", "SCHED"):
            sb, se = snap["begin"]["softirqs"].get(name), snap["end"]["softirqs"].get(name)
            if sb and se:
                rec[f"{side}_host_{name}"] = [(y - x) / n for x, y in zip(sb, se)]
        eb, ee = snap["begin"]["eth0"], snap["end"]["eth0"]
        if eb and ee:
            rec[f"{side}_host_eth0_irq"] = [(y - x) / n for x, y in zip(eb, ee)]
        threads = []
        for tid, d in th.items():
            r, c0 = place.get(tid, (0, 0))
            threads.append((d.get("task-clock", 0.0) / n, d["comm"], tid, d.get("cycles:k", 0) / n,
                            d.get("exc_irq", 0) / n, d.get("exc_svc", 0) / n, d.get("context-switches", 0) / n,
                            (c0 / r) if r else float("nan"), r))
        rec[f"{side}_threads"] = sorted(threads, reverse=True)
        rec[f"{side}_rusage_ms_per_mb"] = rusage_gap_side(runs, stem, side)
    return rec, ""


def read_set(runs, window):
    arms, voids = {}, []
    for sub in sorted(runs.glob("*_Array4m_l0_r*_sub.log")):
        stem = sub.name[: -len("_sub.log")]
        rec, why = run(runs, stem, window)
        arm = stem.split("_Array4m")[0]
        if rec is None:
            voids.append(f"VOID {stem}: {why}")
        else:
            arms.setdefault(arm, []).append(rec)
    return arms, voids


def window_of(runs):
    for line in (runs / "meta.txt").read_text().splitlines():
        if line.startswith("perf_attach="):
            v = line.split("=", 1)[1].replace("_", " ").split()
            if len(v) == 2:
                return float(v[1])
    return None


def med(recs, k):
    xs = [r[k] for r in recs if k in r and not (isinstance(r[k], float) and math.isnan(r[k]))]
    return st.median(xs) if xs else float("nan")


def se(recs, k):
    xs = [r[k] for r in recs if k in r]
    return st.stdev(xs) / math.sqrt(len(xs)) if len(xs) > 1 else float("inf")


def baseline_gap(runs):
    """Uninstrumented rusage gap tickle@head - fastdds, ms/MB, per side (medians)."""
    vals = {}
    for sub in sorted(runs.glob("*_Array4m_l0_r*_sub.log")):
        stem = sub.name[: -len("_sub.log")]
        arm = stem.split("_Array4m")[0]
        if arm not in ("tickle@head", "fastdds"):
            continue
        for side in ("pub", "sub"):
            vals.setdefault((arm, side), []).append(rusage_gap_side(runs, stem, side))
    return {side: st.median(vals[("tickle@head", side)]) - st.median(vals[("fastdds", side)])
            for side in ("pub", "sub") if ("tickle@head", side) in vals and ("fastdds", side) in vals}


METRICS = ("task-clock", "utime_ms", "stime_ms", "cycles:u", "cycles:k", "instructions:u", "instructions:k",
           "exc_svc", "exc_irq", "context-switches", "cpu-migrations")


def verdict(arms, side, k):
    h, a, f = arms.get("tickle@head", []), arms.get("tickle@aa", []), arms.get("fastdds", [])
    key = f"{side}_{k}"
    d = med(h, key) - med(f, key)
    floor = abs(med(a, key) - med(h, key)) if a else float("inf")
    err = math.sqrt(se(h, key) ** 2 + se(f, key) ** 2)
    real = abs(d) > 2 * floor and abs(d) > 2 * err
    return d, floor, err, real


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("main")
    ap.add_argument("--pin")
    ap.add_argument("--baseline")
    a = ap.parse_args()
    runs = Path(a.main)
    window = window_of(runs)
    if not window:
        print("VOID: meta.txt names no perf_attach window")
        return 1
    arms, voids = read_set(runs, window)
    print(f"== {runs} (window {window:.0f} s)")
    for v in voids:
        print(v)
    for arm, recs in arms.items():
        print(f"  {arm}: {len(recs)} usable reps")
    if not all(len(arms.get(x, [])) >= 3 for x in ("tickle@head", "tickle@aa", "fastdds")):
        print("VOID overall: fewer than 3 usable reps of tickle@head, tickle@aa or fastdds")
        return 1
    base = baseline_gap(Path(a.baseline)) if a.baseline else {}
    side_void = {}
    for side in ("sub", "pub"):
        g = med(arms["tickle@head"], f"{side}_rusage_ms_per_mb") - med(arms["fastdds"], f"{side}_rusage_ms_per_mb")
        b = base.get(side)
        if b is None:
            ok, why = False, "no baseline"
        else:
            ok = (g > 0) == (b > 0) and abs(g - b) <= 0.5 * abs(b) + 0.03
            why = f"instrumented gap {g:+.3f} ms/MB vs uninstrumented {b:+.3f}"
        side_void[side] = not ok
        print(f"INSTRUMENT (b) {side}: {'OK' if ok else 'VOID'} - {why}")
    for side in ("sub", "pub"):
        print(f"\n--- {side}: per sample, median over reps (head / aa / fastdds), head - fastdds, A/A floor, verdict")
        for k in METRICS:
            d, floor, err, real = verdict(arms, side, k)
            vals = " / ".join(f"{med(arms[x], f'{side}_{k}'):14.1f}" for x in ("tickle@head", "tickle@aa", "fastdds"))
            print(f"  {k:16} {vals}   diff {d:+14.1f}  floor {floor:12.1f}  2se {2 * err:12.1f}  "
                  f"{'REAL' if real else 'within floor'}")
        for arm in ("tickle@head", "fastdds"):
            recs = arms[arm]
            ipc_k = med(recs, f"{side}_instructions:k") / med(recs, f"{side}_cycles:k")
            ipc_u = med(recs, f"{side}_instructions:u") / med(recs, f"{side}_cycles:u")
            k_per_svc = med(recs, f"{side}_instructions:k") / max(med(recs, f"{side}_exc_svc"), 1e-9)
            ghz = (med(recs, f"{side}_cycles:u") + med(recs, f"{side}_cycles:k")) / (med(recs, f"{side}_task-clock")
                                                                                       * 1e6)
            print(f"  {arm:12} IPC k {ipc_k:.3f} u {ipc_u:.3f}; kernel insn per syscall {k_per_svc:,.0f}; "
                  f"cycles per task-clock ns {ghz:.3f}")
            hosts = [k for k in recs[0] if k.startswith(f"{side}_host_")]
            for k in sorted(hosts):
                v = recs[0][k]
                if isinstance(v, list):
                    xs = [st.median(r[k][i] for r in recs if k in r) for i in range(len(v))]
                    print(f"    {k[len(side) + 6:]:22} per CPU " + " ".join(f"{x:9.2f}" for x in xs))
                else:
                    print(f"    {k[len(side) + 6:]:22} {med(recs, k):9.3f}")
            print("    threads (rep 1): task-clock ms, comm, tid, cycles:k, exc_irq, exc_svc, cs, CPU0 share of running "
                  "samples (n)")
            for t in recs[0][f"{side}_threads"][:6]:
                print(f"      {t[0]:8.3f} {t[1]:>16} {t[2]:>8} {t[3]:12.0f} {t[4]:8.2f} {t[5]:8.1f} {t[6]:7.1f} "
                      f"{t[7]:5.2f} ({t[8]})")
        # Hypotheses
        if side_void[side]:
            print(f"  H1-H3 {side}: VOID (instrument check b failed)")
            continue
        dk, _, _, realk = verdict(arms, side, "cycles:k")
        ds, _, _, reals = verdict(arms, side, "exc_svc")
        di, _, _, reali = verdict(arms, side, "exc_irq")
        h1 = "SUPPORTED" if reals and ds > 0 else ("FALSIFIED" if (reals and ds <= 0) or (not reals and realk)
                                                    else "UNDECIDED")
        print(f"  H1 syscalls {side}: {h1} (exc_svc diff {ds:+.1f}/sample)")
        h2 = "SUPPORTED (correlational)" if reali and di > 0 else ("FALSIFIED" if reali and di <= 0 else "UNDECIDED")
        print(f"  H2 interrupts {side}: {h2} (exc_irq diff {di:+.2f}/sample)")
        h, f = arms["tickle@head"], arms["fastdds"]
        ik_h, ik_f = med(h, f"{side}_instructions:k"), med(f, f"{side}_instructions:k")
        ipc_h, ipc_f = ik_h / med(h, f"{side}_cycles:k"), ik_f / med(f, f"{side}_cycles:k")
        if ipc_h >= ipc_f * 0.95:
            h3 = "FALSIFIED"
        elif abs(ik_h - ik_f) <= 0.10 * ik_f and realk and dk > 0 and ipc_h < 0.90 * ipc_f:
            h3 = "SUPPORTED"
        else:
            h3 = "UNDECIDED"
        print(f"  H3 kernel IPC {side}: {h3} (IPC k {ipc_h:.3f} vs {ipc_f:.3f}; insn:k {ik_h:,.0f} vs {ik_f:,.0f})")
    if a.pin:
        pr = Path(a.pin)
        pw = window_of(pr)
        parms, pvoids = read_set(pr, pw)
        print(f"\n== pinned {pr}")
        for v in pvoids:
            print(v)
        for side in ("sub", "pub"):
            if len(parms.get("tickle@head", [])) < 3 or len(parms.get("fastdds", [])) < 3:
                print(f"  H2 causal {side}: VOID (fewer than 3 usable pinned reps per arm)")
                continue
            g0 = med(arms["tickle@head"], f"{side}_task-clock") - med(arms["fastdds"], f"{side}_task-clock")
            g1 = med(parms["tickle@head"], f"{side}_task-clock") - med(parms["fastdds"], f"{side}_task-clock")
            i1 = med(parms["tickle@head"], f"{side}_exc_irq") - med(parms["fastdds"], f"{side}_exc_irq")
            for arm in ("tickle@head", "fastdds"):
                print(f"  {side} {arm:12} pinned task-clock {med(parms[arm], f'{side}_task-clock'):8.3f} ms/sample "
                      f"(unpinned {med(arms[arm], f'{side}_task-clock'):8.3f}), exc_irq "
                      f"{med(parms[arm], f'{side}_exc_irq'):7.2f} (unpinned {med(arms[arm], f'{side}_exc_irq'):7.2f}), "
                      f"cycles:k {med(parms[arm], f'{side}_cycles:k'):12.0f}")
            if g0 <= 0:
                v = "UNDECIDED (no unpinned gap)"
            elif g1 <= 0.5 * g0:
                v = "SUPPORTED"
            elif g1 >= g0:
                v = "FALSIFIED"
            else:
                v = "UNDECIDED (shrinks by less than half)"
            print(f"  H2 causal {side}: {v} - task-clock gap {g0:+.3f} ms/sample unpinned, {g1:+.3f} pinned; "
                  f"exc_irq gap pinned {i1:+.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
