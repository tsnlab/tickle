#!/usr/bin/env python3
"""Gate v2 for the rmw-layer rig harnesses: rmw_tickle against rmw_tickle (the user's decision of 2026-10-11).

Reads the blocks rmw_gate_v2_chain.sh ran - A (the parent), P (A's commit rebuilt with TICKLE_PLACEMENT_CFLAGS) and B
(the change), mirrored A P B B P A - and decides by gate_v2.py, whose docstring is the rule: one Bonferroni Student t
threshold over every PRIMARY and WITNESS comparison, and a comparison is WORSE only beyond that threshold AND beyond its
placement floor |mean(P) - mean(A)|. Written before any rmw gate v2 run; every rule here is implemented below, and
--selftest checks them against fixtures. ab_compare.py's `rmw` mode is the old reading and is left as it was.

UNIT. One row (rtt) or one run (keepall) is one observation; an arm's observations are pooled over its blocks (two
blocks x REPS). Only rows the harness judged ok, and runs the summary did not VOID, are read; a counted row or run
without both processes' counts was already VOID there (insn=fail is "could not look", never "no work").

rtt  (rmw_crosshost_rtt.sh outputs, RMW_LIST=rmw_tickle, INSN_COUNT=1)
  cell     "<rmw> <msg> <qos> [tags]" - the row's identity without its rep, as ab_compare.py rmw names it.
  PRIMARY  ping.insn_u_per_sample, ping.insn_k_per_sample, pong.insn_u_per_sample, pong.insn_k_per_sample: each
           process's whole-life instructions:u / instructions:k over the ping's sent= (its round trips). Whole-life:
           start-up, discovery and the pong's idle wait are in the count too, the same in every arm, so they add
           variance to the per-round-trip figure but no offset to B - A.
  WITNESS  rtt_avg_ms and pong.cpu_ns (the pong's schedstat CPU over its life) - what the rmw A/Bs decided on before.
  secondary  rtt_min_ms, rtt_max_ms, ping.cpu_s, pong.idle_cpu_ns, ping.maxrss_kb, pong.maxrss_kb. Lower is better
           for all.
keepall  (rmw_keepall_rig.sh <OUT>.runs directories, ARMS=tickle@head, INSN_COUNT=1)
  cell     "<topic> l<loss>", arm tickle@head only.
  PRIMARY  pub.insn_{u,k}_per_sample over the publisher's sent, sub.insn_{u,k}_per_sample over the subscriber's
           received (whole counts, rmw_keepall_rig_summary.py COUNTS).
  WITNESS  rate (delivered msgs/s, higher is better), pub.cpu_us_per_sample, sub.cpu_us_per_sample.
  secondary  latency_ms (the two Pis' clocks are not synchronised: printed, never deciding).
  A refused run (the publisher ended on "failed to publish") is left out of every metric and counted per arm.
  CORRECTNESS, as the summary's header: a tickle@head run with lost > 0 or a publisher error is a defect. Any in B
           makes the overall result WORSE whatever the table says; any in A or P is printed (the parent's defect is
           not the change's) and the gate still reads the rest.
--primary / --witness SPEC narrow the families: comma-separated items "[<cell substring>:]<metric>", the metric with or
  without its role ("insn_u_per_sample" is every role's, "pong.insn_u_per_sample" the pong's), e.g.
  --witness "rtt_avg_ms,bench best_effort:pong.idle_cpu_ns". An item matching no comparison present in both A and B
  is NO VERDICT (gate_v2.report, family "unmatched"), as is a run with no counts or no P arm.
The last line printed is "GATE: PASS|WORSE|NO VERDICT (...)". Exit status 0 PASS, 1 WORSE, 3 NO VERDICT, 2 usage.

Usage:
  rmw_gate_v2.py rtt     --a A1.txt A2.txt --p P1.txt P2.txt --b B1.txt B2.txt [--primary SPEC] [--witness SPEC]
  rmw_gate_v2.py keepall --a A1.runs A2.runs --p P1.runs P2.runs --b B1.runs B2.runs [--primary SPEC] [--witness SPEC]
  rmw_gate_v2.py --selftest [seed]
"""
import os
import random
import re
import shutil
import sys
import tempfile
from collections import OrderedDict
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_v2  # noqa: E402

RMW_LINE = re.compile(r"^(?P<rmw>rmw_\w+) (?P<msg>\w+) (?P<qos>\w+) rep(?P<rep>\d+)(?P<tags>[^|]*)\|\s*(?P<verdict>\S+)"
                      r"[^|]*\|\s*(?P<fields>.*)$")
INSN = ("insn_u_per_sample", "insn_k_per_sample")
DEFAULTS = {
    "rtt": ("insn_u_per_sample,insn_k_per_sample", "rtt_avg_ms,pong.cpu_ns"),
    "keepall": ("insn_u_per_sample,insn_k_per_sample", "rate,pub.cpu_us_per_sample,sub.cpu_us_per_sample"),
}
HIGHER_BETTER = {"rate"}


def rtt_rows(paths):
    """cell -> metric -> [values] over the ok rows of rmw_crosshost_rtt.sh outputs, and the void row count."""
    cells, voids = OrderedDict(), 0
    for path in paths:
        for text in open(path, encoding="utf-8", errors="replace"):
            m = RMW_LINE.match(text.rstrip())
            if not m:
                continue
            if not m["verdict"].startswith("ok"):
                voids += 1
                continue
            f = {k: float(v) for k, v in re.findall(r"([A-Za-z_]\w*)=(-?[\d.]+)(?=\s|$)", m["fields"])}
            tags = " ".join(t for t in m["tags"].split() if not t.startswith("execpoll="))
            cell = f"{m['rmw']} {m['msg']} {m['qos']} {tags}".strip()
            d = cells.setdefault(cell, {})
            met = {"rtt_avg_ms": f.get("rtt_avg_ms"), "rtt_min_ms": f.get("rtt_min_ms"),
                   "rtt_max_ms": f.get("rtt_max_ms"), "pong.cpu_ns": f.get("pong_cpu_ns"),
                   "pong.idle_cpu_ns": f.get("pong_idle_cpu_ns"), "ping.maxrss_kb": f.get("ping_maxrss_kb"),
                   "pong.maxrss_kb": f.get("pong_maxrss_kb")}
            if "ping_utime_s" in f and "ping_stime_s" in f:
                met["ping.cpu_s"] = f["ping_utime_s"] + f["ping_stime_s"]
            rts = f.get("sent")
            for role in ("ping", "pong"):
                for uk in ("u", "k"):
                    n = f.get(f"{role}_insn_{uk}")
                    if n and rts:
                        met[f"{role}.insn_{uk}_per_sample"] = n / rts
            for k, v in met.items():
                if v is not None:
                    d.setdefault(k, []).append(v)
    return cells, voids


def keepall_rows(paths):
    """cell -> metric -> [values] over tickle@head's usable runs; the void and refused counts; correctness failures."""
    import rmw_keepall_rig_summary as ka
    cells, voids, refused, failures = OrderedDict(), 0, 0, []
    for path in paths:
        _, rcells, rvoids, rfail, _ = ka.read_runs(Path(path))
        voids += sum(1 for v in rvoids if v.startswith("tickle@head_"))
        failures += [f for f in rfail if f.startswith("tickle@head_")]
        for (topic, loss), arms in sorted(rcells.items()):
            for r in arms.get("tickle@head", []):
                if r["refused"]:
                    refused += 1
                    continue
                d = cells.setdefault(f"{topic} l{loss}", {})
                met = {"rate": r["rate"], "pub.cpu_us_per_sample": r["cpu_pub"],
                       "sub.cpu_us_per_sample": r["cpu_sub"], "latency_ms": r["lat"]}
                for role in ("pub", "sub"):
                    for m in INSN:
                        if f"{role}_{m}" in r:
                            met[f"{role}.{m}"] = r[f"{role}_{m}"]
                for k, v in met.items():
                    if v == v:  # not nan
                        d.setdefault(k, []).append(v)
    return cells, voids, refused, failures


def parse_items(spec):
    """[(cell substring or '', metric)] from 'a,b:c'; raises ValueError on an empty item."""
    items = []
    for it in (s.strip() for s in (spec or "").split(",")):
        if not it:
            continue
        cell, _, metric = it.rpartition(":")
        if not metric:
            raise ValueError(f"empty metric in item '{it}'")
        items.append((cell, metric))
    return items


def selects(item, cell, metric):
    c, m = item
    return (not c or c in cell) and (metric == m or metric.split(".", 1)[-1] == m)


def gate_rows(ca, cp, cb, primary, witness):
    """gate_v2.report rows for every (cell, metric) present in both A and B; P supplies the floor where it has data."""
    pitems, witems = parse_items(primary), parse_items(witness)
    rows, seen = [], set()
    for cell in ca:
        for metric in ca[cell]:
            a = gate_v2.stats(ca[cell][metric])
            b = gate_v2.stats(cb.get(cell, {}).get(metric, []))
            if not (a and b):
                continue
            p = gate_v2.stats(cp.get(cell, {}).get(metric, []))
            fam = "secondary"
            for fam_name, items in (("primary", pitems), ("witness", witems)):
                hit = [it for it in items if selects(it, cell, metric)]
                if hit and fam == "secondary":
                    fam = fam_name
                    seen.update(hit)
            rows.append({"cell": cell, "metric": metric, "family": fam, "a": a, "b": b, "p": p,
                         "hi_better": metric in HIGHER_BETTER})
    for it in pitems + witems:
        if it not in seen:
            rows.append({"family": "unmatched", "metric": (f"{it[0]}:" if it[0] else "") + it[1]})
    return rows


def run(mode, pa, pp, pb, primary, witness):
    title = f"{mode}: A {' '.join(pa)} | P {' '.join(pp) or '(none)'} | B {' '.join(pb)}"
    if mode == "rtt":
        (ca, va), (cp, vp), (cb, vb) = rtt_rows(pa), rtt_rows(pp), rtt_rows(pb)
        fb = []
        print(f"void rows: A {va}, P {vp}, B {vb}")
    else:
        ca, va, ra, fa = keepall_rows(pa)
        cp, vp, rp, fp = keepall_rows(pp)
        cb, vb, rb, fb = keepall_rows(pb)
        print(f"void runs: A {va}, P {vp}, B {vb}; refused runs (left out): A {ra}, P {rp}, B {rb}")
        for arm, fails in (("A", fa), ("P", fp), ("B", fb)):
            for f in fails:
                print(f"CORRECTNESS FAIL in {arm}: {f}")
    rc = gate_v2.report(gate_rows(ca, cp, cb, primary, witness), title=title)
    if fb:
        print(f"GATE: WORSE (correctness: {len(fb)} tickle@head run(s) of B lost samples or had a publisher error)")
        return 1
    print("GATE: " + {0: "PASS", 1: "WORSE", 3: "NO VERDICT"}[rc])
    return rc


# ---- selftest ---------------------------------------------------------------------------------------------------
def _rtt_line(rep, rtt, pong_cpu, insn, counted=True, verdict="ok"):
    fields = (f"framework=rmw_tickle scenario=pingpong qos=best_effort wait=poll sent=100 recv=100 loss_pct=0 "
              f"rtt_min_ms={rtt * 0.8:.4f} rtt_avg_ms={rtt:.4f} rtt_max_ms={rtt * 1.5:.4f} LOOP: x=1 "
              f"ping_utime_s=0.12 ping_stime_s=0.05 ping_maxrss_kb=30000 PINGMAPS: /h/librmw_tickle.so "
              f"pong_procs=1 ping_procs_before=0 pong_cpu_s=0.20 pong_cpu_ns={pong_cpu:.0f} pong_maxrss_kb=31000")
    if counted:
        fields += (f" ping_insn_u={insn[0]:.0f} ping_insn_k={insn[1]:.0f} ping_insn=ok"
                   f" pong_insn_u={insn[2]:.0f} pong_insn_k={insn[3]:.0f} pong_insn=ok")
    return f"rmw_tickle bench best_effort rep{rep} | {verdict} | {fields}\n"


def _rtt_arm(path, rng, rtt=0.5, cpu=2.0e8, insn_scale=(1, 1, 1, 1), counted=True, n=6):
    base = (4.0e7, 9.0e7, 3.5e7, 8.0e7)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("=== fixture ===\n")
        for rep in range(1, n + 1):
            ins = [b * s * (1 + rng.gauss(0, 0.002)) for b, s in zip(base, insn_scale)]
            fh.write(_rtt_line(rep, rtt * (1 + rng.gauss(0, 0.01)), cpu * (1 + rng.gauss(0, 0.01)), ins, counted))
        fh.write(_rtt_line(n + 1, rtt, cpu, base, counted, verdict="VOID(loss)"))  # a void row is never read


def _keepall_arm(d, rng, fixture, sub_u_scale=1.0, n=3):
    """A runs dir of n copies of the fixture's tickle@head run, each with its own instruction counts."""
    d.mkdir()
    lines = [x for x in (fixture / "index.txt").read_text().splitlines() if x.startswith("RUN tickle@head_")]
    stem = lines[0].split()[1]
    out = []
    for rep in range(1, n + 1):
        new = stem.replace("_r1", f"_r{rep}")
        for role in ("pub", "sub"):
            shutil.copy(fixture / f"{stem}_{role}.log", d / f"{new}_{role}.log")
        u, k = (9.0e9 * (1 + rng.gauss(0, 0.002)), 2.0e10 * (1 + rng.gauss(0, 0.002)))
        su = 7.0e9 * sub_u_scale * (1 + rng.gauss(0, 0.002))
        out.append(lines[0].replace(stem, new) + f" pub_insn_u={u:.0f} pub_insn_k={k:.0f} pub_insn=ok"
                   f" sub_insn_u={su:.0f} sub_insn_k={k:.0f} sub_insn=ok")
    (d / "index.txt").write_text("\n".join(out) + "\n")


def selftest(seed=20261011):
    import contextlib
    import io
    tmp = Path(tempfile.mkdtemp(prefix="rmw_gate_v2_selftest."))
    fails = []

    def case(what, want, mode, arms, primary=None, witness=None, why=""):
        buf = io.StringIO()
        dp, dw = DEFAULTS[mode]
        with contextlib.redirect_stdout(buf):
            rc = run(mode, *arms, dp if primary is None else primary, dw if witness is None else witness)
        last = buf.getvalue().strip().splitlines()[-1]
        ok = rc == want and last.startswith("GATE: " + {0: "PASS", 1: "WORSE", 3: "NO VERDICT"}[want])
        ok = ok and why in buf.getvalue()  # a NO VERDICT must give the reason it was given for
        print(f"  {'ok  ' if ok else 'FAIL'} {what}: exit {rc}, '{last}'")
        if not ok:
            fails.append(what)
            print(buf.getvalue())

    try:
        rng = random.Random(seed)

        def arms(tag, a=None, p=None, b=None, counted=True, with_p=True):
            paths = []
            for arm, kw in (("A", a or {}), ("P", p or {}), ("B", b or {})):
                f = tmp / f"{tag}_{arm}.txt"
                _rtt_arm(f, rng, counted=counted, **kw)
                paths.append([str(f)] if (arm != "P" or with_p) else [])
            return paths

        print("rtt fixtures (6 rows per arm, 0.2% instruction noise, 1% RTT and CPU noise):")
        case("A/A", 0, "rtt", arms("aa"))
        case("ping instructions:u +5% in B", 1, "rtt", arms("insn", b={"insn_scale": (1.05, 1, 1, 1)}))
        case("RTT +4% in B, and +8% in P (placement-sized)", 0, "rtt",
             arms("wpl", p={"rtt": 0.54}, b={"rtt": 0.52}), why="beyond the threshold but inside")
        case("control: RTT +4% in B, P unmoved", 1, "rtt", arms("w", b={"rtt": 0.52}))
        case("no instruction counts, default primaries", 3, "rtt", arms("nc", counted=False),
             why="insn_u_per_sample, insn_k_per_sample matched no")
        case("no instruction counts, no primary named", 3, "rtt", arms("nc0", counted=False), primary="",
             why="no instruction counts (PRIMARY)")
        case("no P arm", 3, "rtt", arms("np", with_p=False), why="have no placement arm P")
        case("a pre-registered item that selects nothing", 3, "rtt", arms("un"), witness="rtt_avg_ms,c9:nothing",
             why="c9:nothing")
        fx = Path(__file__).resolve().parent / "perf_test_counts_fixture" / "keepall.runs"
        print(f"keepall fixtures (3 copies of {fx.name}'s tickle@head run per arm, 0.2% instruction noise):")
        ka = []
        for tag, scale in (("aa", (1, 1, 1)), ("su", (1, 1, 1.05))):
            dirs = []
            for arm, s in zip("APB", scale):
                d = tmp / f"ka_{tag}_{arm}.runs"
                _keepall_arm(d, rng, fx, sub_u_scale=s)
                dirs.append([str(d)])
            ka.append(dirs)
        case("keepall A/A", 0, "keepall", ka[0])
        case("keepall subscriber instructions:u +5% in B", 1, "keepall", ka[1])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"selftest: {'PASS' if not fails else 'FAIL (' + ', '.join(fails) + ')'}")
    return 1 if fails else 0


def main(argv):
    args = list(argv[1:])
    if args[:1] == ["--selftest"]:
        return selftest(*(int(x) for x in args[1:2]))
    if not args or args[0] not in DEFAULTS:
        print(__doc__)
        return 2
    mode = args.pop(0)
    opt = {"--primary": DEFAULTS[mode][0], "--witness": DEFAULTS[mode][1]}
    for name in opt:
        if name in args:
            i = args.index(name)
            if i + 1 >= len(args):
                print(__doc__)
                return 2
            opt[name] = args[i + 1]
            del args[i:i + 2]
    lists, cur = {"--a": [], "--p": [], "--b": []}, None
    for a in args:
        if a in lists:
            cur = a
        elif cur is None:
            print(__doc__)
            return 2
        else:
            lists[cur].append(a)
    if not lists["--a"] or not lists["--b"]:
        print(__doc__)
        return 2
    try:
        parse_items(opt["--primary"])
        parse_items(opt["--witness"])
    except ValueError as err:
        print(err)
        return 2
    return run(mode, lists["--a"], lists["--p"], lists["--b"], opt["--primary"], opt["--witness"])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
