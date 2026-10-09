#!/usr/bin/env python3
"""rmw_rtt_bisect.py - the reading of rmw_rtt_bisect.sh's runs. Every rule is written here before the run.

Per run (rtt_<msg>_<qos>_block_<arm>_r<rep>/):
  window   the stamps' round trips [4096 : n-4096] (the rig's window); VOID if fewer than 5,000 remain.
  TREATMENT  VOID unless both processes mapped <builds>/<arm sha>*/install/rmw_tickle/lib/librmw_tickle.so (meta
           "maps" lines) and the ping's traffic line shows tx_shm >= 0.9 of tx_datagrams (the segment carried it).
  metrics  rtt_mean, rtt_p50 (us); per measured round trip, over the window's time span, interpolated from the
           sampler: cpu_us (ping + pong thread run time), minflt, vcsw, ivcsw (ping + pong).
Per arm and cell: mean and standard error over the non-VOID reps; NO VERDICT for a cell where any arm has < 6.

Rules (cell bench/reliable is primary; bench/best_effort is reported alongside, read the same way):
  R0 CONTROL   the first arm against itself (<arm>_ctl): if |diff| > 2 x SE of the difference for rtt_mean or cpu_us
               in any cell, the PC cannot tell an arm from itself and the campaign is VOID.
  R1 REPRODUCED?  last arm - first arm, rtt_mean and cpu_us: t > 2 -> REPRODUCED on the PC; otherwise NOT REPRODUCED
               (the PC cannot decide the rig's question; a rig A/B is then needed).
  R2 WHICH COMMIT  each consecutive step (arm i -> arm i+1): t > 2.5 (4 steps) and diff > 0 names that commit.
  R3 LAZY FAULTS  the step into 718220d8 (if it is an arm): minflt per round trip diff < 0.01 -> the lazy-fault
               hypothesis is FALSIFIED (fewer than one extra fault per 100 round trips cannot add microseconds);
               otherwise its value and t are printed and the hypothesis stands.
"""
import math
import pathlib
import re
import sys

WARM = COOL = 4096
MIN_WINDOW = 5000
MIN_REPS = 6


def stamps(d):
    rows = []
    for line in (d / "stamps.txt").read_text().splitlines():
        if line.startswith("#"):
            continue
        p = line.split()
        if len(p) == 3:
            rows.append((int(p[1]), int(p[2])))
    return rows


def series(d):
    s, f = [], []
    for line in (d / "sampler.txt").read_text().splitlines():
        p = line.split()
        if len(p) < 2:
            continue
        t = int(p[1])
        if p[0] == "S":
            rec = {}
            for x in p[2:]:
                k, v = x.split("=", 1)
                if "/" in v:
                    rec[k] = int(v.split("/")[0])
            s.append((t, rec))
        elif p[0] == "F":
            rec = {}
            for x in p[2:]:
                k, v = x.split("=", 1)
                rec[k] = [int(y) for y in v.split("/")]
            f.append((t, rec))
    return s, f


def interp(pts, t):
    before = [p for p in pts if p[0] <= t]
    after = [p for p in pts if p[0] >= t]
    if not before or not after:
        return None
    (t0, v0), (t1, v1) = before[-1], after[0]
    return v0 if t1 == t0 else v0 + (v1 - v0) * (t - t0) / (t1 - t0)


def delta(rows, role, idx, t0, t1):
    pts = []
    for t, rec in rows:
        if role in rec:
            v = rec[role]
            pts.append((t, v[idx] if isinstance(v, list) else v))
    a, b = interp(pts, t0), interp(pts, t1)
    return None if a is None or b is None else b - a


def run(d, builds):
    meta = (d / "meta.txt").read_text() if (d / "meta.txt").exists() else ""
    m = re.match(r"rtt_(\w+?)_(reliable|best_effort)_block_(\w+)_r(\d+)$", d.name)
    msg, qos, arm, rep = m.group(1), m.group(2), m.group(3), int(m.group(4))
    sha = arm.removesuffix("_ctl")
    out = {"cell": f"{msg}/{qos}", "arm": arm, "rep": rep, "void": None}
    libs = re.findall(r"maps role=(ping|pong) libs=(\S*)", meta)
    want = f"{builds}/{sha}"
    if len(libs) != 2 or not all(re.search(re.escape(want) + r"\w*/install/rmw_tickle/lib/librmw_tickle\.so", l)
                                 for _, l in libs):
        out["void"] = "treatment: librmw_tickle.so of this arm not mapped by both"
        return out
    log = (d / "ping.log").read_text(errors="replace") if (d / "ping.log").exists() else ""
    tm = re.search(r"tx_datagrams=(\d+).*? tx_shm=(\d+)", log)
    if not tm or int(tm.group(1)) == 0 or int(tm.group(2)) / int(tm.group(1)) < 0.9:
        out["void"] = "treatment: tx_shm share < 0.9 (or no traffic line)"
        return out
    st = stamps(d)
    w = st[WARM:len(st) - COOL]
    if len(w) < MIN_WINDOW:
        out["void"] = f"window {len(w)} < {MIN_WINDOW}"
        return out
    r = sorted((b - a) / 1000 for a, b in w)
    out["rtt_mean"] = sum(r) / len(r)
    out["rtt_p50"] = r[len(r) // 2]
    t0, t1 = w[0][0], w[-1][1]
    s, f = series(d)
    vals = {}
    for name, rows, idx, scale in (("cpu_us", s, None, 1e-3), ("minflt", f, 0, 1), ("vcsw", f, 2, 1),
                                   ("ivcsw", f, 3, 1)):
        tot = 0
        for role in ("ping", "pong"):
            v = delta(rows, role, idx, t0, t1)
            if v is None:
                out["void"] = f"sampler does not bracket the window for {role} {name}"
                return out
            tot += v
        vals[name] = tot * scale / len(w)
    out.update(vals)
    out["n"] = len(w)
    return out


def stats(xs):
    n = len(xs)
    m = sum(xs) / n
    se = math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1) / n) if n > 1 else float("inf")
    return m, se


def main():
    d = pathlib.Path(sys.argv[1])
    params = (d / "params.txt").read_text().split()
    arms = next(p for p in params if p.startswith("arms=")) if any(p.startswith("arms=") for p in params) else None
    line = (d / "params.txt").read_text()
    arms = re.search(r"arms=(.*?) host=", line).group(1).split()
    builds = str(pathlib.Path.home() / "rmw_bisect_builds")
    runs = [run(x, builds) for x in sorted(d.iterdir()) if x.is_dir() and x.name.startswith("rtt_")]
    print(f"rmw_rtt_bisect.py {d}  arms {' '.join(arms)}  runs {len(runs)}")
    for x in runs:
        if x["void"]:
            print(f"VOID {x['cell']} {x['arm']} r{x['rep']}: {x['void']}")
    metrics = ("rtt_mean", "rtt_p50", "cpu_us", "minflt", "vcsw", "ivcsw")
    cells = sorted({x["cell"] for x in runs}, key=lambda c: c != "bench/reliable")
    table = {}
    print("\nper rep (rtt_mean us / cpu us per rt / minflt per rt):")
    for c in cells:
        for a in arms:
            ok = sorted((x for x in runs if x["cell"] == c and x["arm"] == a and not x["void"]), key=lambda x: x["rep"])
            table[(c, a)] = ok
            print(f"  {c:18s} {a:14s} " + "  ".join(f"r{x['rep']} {x['rtt_mean']:.1f}/{x['cpu_us']:.1f}/{x['minflt']:.3f}"
                                                     for x in ok))
    verdict_lines = []
    for c in cells:
        print(f"\n== {c}")
        print(f"  {'arm':14s} n " + " ".join(f"{m:>16s}" for m in metrics))
        for a in arms:
            ok = table[(c, a)]
            if len(ok) < 2:
                print(f"  {a:14s} {len(ok)} (too few)")
                continue
            print(f"  {a:14s} {len(ok)} " + " ".join(
                "{:>9.3f}±{:<6.3f}".format(*stats([x[m] for x in ok])) for m in metrics))
        if any(len(table[(c, a)]) < MIN_REPS for a in arms):
            verdict_lines.append(f"{c}: NO VERDICT (an arm has fewer than {MIN_REPS} usable reps)")
            continue

        def diff(a, b, m):
            ma, sa = stats([x[m] for x in table[(c, a)]])
            mb, sb = stats([x[m] for x in table[(c, b)]])
            se = math.sqrt(sa * sa + sb * sb)
            return mb - ma, se, (mb - ma) / se if se > 0 else float("inf")

        real = [a for a in arms if not a.endswith("_ctl")]
        ctl = arms[0]
        base = ctl.removesuffix("_ctl")
        void = False
        for m in ("rtt_mean", "cpu_us"):
            dd, se, t = diff(base, ctl, m)
            line = f"{c} R0 control {base} vs itself, {m}: {dd:+.3f} (SE {se:.3f}, t {t:+.2f})"
            if abs(dd) > 2 * se:
                void = True
                line += " -> VOID"
            verdict_lines.append(line)
        if void:
            verdict_lines.append(f"{c}: VOID - the PC cannot tell {base} from itself")
            continue
        for m in ("rtt_mean", "cpu_us"):
            dd, se, t = diff(real[0], real[-1], m)
            verdict_lines.append(f"{c} R1 {real[0]} -> {real[-1]}, {m}: {dd:+.3f} (SE {se:.3f}, t {t:+.2f}) -> "
                                 + ("REPRODUCED" if t > 2 else "NOT REPRODUCED"))
        for i in range(len(real) - 1):
            for m in ("rtt_mean", "cpu_us", "minflt"):
                dd, se, t = diff(real[i], real[i + 1], m)
                tag = ""
                if m != "minflt" and t > 2.5 and dd > 0:
                    tag = " -> THIS STEP"
                verdict_lines.append(f"{c} R2 step {real[i]} -> {real[i + 1]}, {m}: {dd:+.4f} (SE {se:.4f}, t {t:+.2f}){tag}")
        lazy = [i for i, a in enumerate(real) if a.startswith("718220d8")]
        if lazy and lazy[0] > 0:
            i = lazy[0]
            dd, se, t = diff(real[i - 1], real[i], "minflt")
            verdict_lines.append(f"{c} R3 lazy faults, {real[i - 1]} -> {real[i]}: minflt per rt {dd:+.4f} (t {t:+.2f}) -> "
                                 + ("FALSIFIED" if dd < 0.01 else "STANDS"))
    print("\nVERDICTS")
    for v in verdict_lines:
        print("  " + v)


main()
