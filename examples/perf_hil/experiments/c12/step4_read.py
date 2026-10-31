#!/usr/bin/env python3
"""C12 idle wake, step 4: the deadline timerfd in the wait set as TickLE's idle cost, with descriptive counters. The
pre-registered reader of step4_rig.sh, written 2026-10-31 before any of its runs.

ARMS (wait ring as shipped; built from b6f0c635 + c12/step3.patch with TT_EXP_C12_READBACK, as step 3's reference):
  (a) rmw_tickle@libc_rb         the reference
  (d) rmw_tickle@libc_rb_nolazy  TT_RX_LAZY_TIMER=0 in the environment (an existing runtime switch, hal_linux.c
                                 wait_set_setup()): the deadline timerfd is never created nor put in the wait set, and
                                 every wait takes its own timeout (io_uring_enter's EXT_ARG). The pong runs without it -
                                 the switch is the A/B control the timer was landed with.
  CycloneDDS the control. Injection none and 50 ms. n = 13 (idle_wake_read.py's formula).

DECIDING ROWS - step 3's E rule, unchanged: E_x = d_x - d_cyclone per rep, d = p50(none) - p50(50).
  VOID  a cell < 10 paired valid reps (a failed readback voids its row); CycloneDDS's none median outside 12.9 +- 1.0;
        CycloneDDS's mean d above +1.0 us.
  (d) EXPLAINS THE COST when E_d < +0.4 us and its CI high end < (a)'s E CI low end; else NOT EXPLAINED.
TREATMENT READBACK per TickLE row (all must hold or the row is VOID):
  both: step 3's readback (gro_probe=1, csum0=1, UDP_GRO 0 at bind and close);
  (d):  the HAL's close line timer_waits=0 timer_sets=0; no timerfd_settime() after the first ping (sysstamp); >= 90% of
        the waits that received a ping carried a timeout of their own (io_uring_enter EXT_ARG);
  (a):  printed only (its waits mostly have none: the timerfd carries the deadline).

COUNTER ROWS (stems ..._cnt, reps 1..6, (a) and CycloneDDS only): DESCRIPTIVE, THEY DECIDE NOTHING. perf stat -t on the
receiving thread (dTLB-load-misses, L1-dcache-load-misses, instructions, cycles, over ~14 s from just before the ping,
divided by the ping's 100 samples), and c12/migr.bt (switch-ins on another cpu than the last switch-out, and
sched_migrate_task). A row is NOT READ when the counted thread is not the one sysstamp saw receive the pings, or a count
is missing. If CycloneDDS's d (none - 50) over its counter rows is above +1.0 us, the instruments perturbed the control
and NO counter is read. Written before the run, what each would show:
  cold caches or TLB: TickLE's misses and cycles per ping higher in none than in 50, instructions per ping about equal,
    the cycles difference of the order of the cost (~2 us at 1.5 GHz, ~3000 cycles); CycloneDDS's differences smaller;
  migration: TickLE's share of switch-ins on another cpu clearly higher in none than in 50 (>= 20 points), CycloneDDS's
    not;
  neither: the per-ping counts and the share alike across arms.

Usage: step4_read.py <OUT file> | --selftest | --reps-needed     Last line: "STEP4_VERDICT ..." (always).
"""
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import idle_wake_read as W  # noqa: E402
import ringoff_read as RO  # noqa: E402
import step2_read as S2  # noqa: E402
import step3_read as S3  # noqa: E402

R = W.R
A, D, CYC = "rmw_tickle@libc_rb", "rmw_tickle@libc_rb_nolazy", "rmw_cyclonedds_cpp"
RMWS = (A, D, CYC)
ARMS = ("none", "50")
N_REPS, MIN_PAIRED = W.N_REPS, W.MIN_PAIRED
CYC_NONE_US, CYC_NONE_TOL, CYC_D_MAX, EXPLAIN_E = 12.9, 1.0, 1.0, 0.4
TIMED_FRAC = 0.9
PINGS = 100
EVENTS = ("dTLB-load-misses", "L1-dcache-load-misses", "instructions", "cycles")
S3.EXPECT[D] = dict(S3.EXPECT[S3.A])  # (d) is the reference build with one switch: same socket readback


def readback(log_text, rmw, res, recs):
    ok, line = S3.readback(log_text, rmw if rmw in S3.EXPECT else S3.A)
    if not ok:
        return ok, line
    m = re.search(r" timer_waits=(\d+) timer_sets=(\d+)", log_text)
    tw, ts = (int(m.group(1)), int(m.group(2))) if m else (None, None)
    waits = [S2.head_ints(pr[5])[0] for _, _, pr in res if pr[1] == "io_uring_enter"]
    waits = [w for w in waits if w is not None]
    timed = sum(w >= 0 for w in waits)
    tid = statistics.mode([pr[0] for _, _, pr in res]) if res else None
    first = res[0][0] if res else 0
    tfd = sum(1 for r in recs if r[0] == tid and r[1] == "timerfd_settime" and r[3] >= first)
    line += (f" | timer_waits={tw} timer_sets={ts}, receiving waits with their own timeout {timed}/{len(waits)}, "
             f"timerfd_settime after first ping {tfd}")
    if rmw == D and (tw != 0 or ts != 0 or tfd != 0 or not waits or timed < TIMED_FRAC * len(waits)):
        return False, "VOID(readback: deadline timer still used) " + line
    return True, line


def decide(p):
    lines = []
    d = {r: W.paired(p[r], "none", "50") for r in RMWS}
    for x in RMWS:
        if len(d[x]) < MIN_PAIRED:
            return f"VOID({x}: {len(d[x])} paired none/50 reps < {MIN_PAIRED})", lines
    e = {x: S3.excess(p, x) for x in (A, D)}
    for x in (A, D):
        if len(e[x]) < MIN_PAIRED:
            return f"VOID({x}: {len(e[x])} reps paired with the control < {MIN_PAIRED})", lines
    cd = {x: W.mean_ci(d[x]) for x in RMWS}
    ce = {x: W.mean_ci(e[x]) for x in (A, D)}
    cnone = statistics.median(p[CYC]["none"].values())
    for x in RMWS:
        lines.append(f"raw d50 {x:26s} {cd[x][0]:+.2f} us [{cd[x][1]:+.2f}, {cd[x][2]:+.2f}] n={len(d[x])}")
    for x in (A, D):
        lines.append(f"E       {x:26s} {ce[x][0]:+.2f} us [{ce[x][1]:+.2f}, {ce[x][2]:+.2f}] n={len(e[x])}")
    lines.append(f"cyclone none median {cnone:.2f}")
    if abs(cnone - CYC_NONE_US) > CYC_NONE_TOL:
        return f"VOID(control none arm {cnone:.2f} us, not {CYC_NONE_US} +- {CYC_NONE_TOL})", lines
    if cd[CYC][0] > CYC_D_MAX:
        return f"VOID(control d50 {cd[CYC][0]:+.2f} us > {CYC_D_MAX:+.1f})", lines
    if ce[D][0] < EXPLAIN_E and ce[D][2] < ce[A][1]:
        return "d EXPLAINS", lines
    return "d NOT EXPLAINED", lines


def parse_counters(text):
    """{tid, comm list, events{}, same, moved, migrate} from one counter file, or None if incomplete."""
    m = re.search(r"^counted (\d+)$", text, re.M)
    ev = {}
    for name in EVENTS:
        mm = re.search(rf"^(\d+),[^,]*,{re.escape(name)},", text, re.M)
        if mm:
            ev[name] = int(mm.group(1))
    if not m or len(ev) != len(EVENTS):
        return None
    def cnt(k):
        mm = re.search(rf"^@{k}: (\d+)$", text, re.M)
        return int(mm.group(1)) if mm else 0
    if "# c12 migr tid" not in text:
        return None
    return {"tid": m.group(1), "ev": ev, "same": cnt("same"), "moved": cnt("moved"), "migrate": cnt("migrate")}


def counters(out, text, offs):
    cdir = out.with_suffix(out.suffix + ".counters")
    rows = {}
    for m in re.finditer(r"^(\S+) bench best_effort rep(\d+) wait=block sst=on inject=(\S+) counted=1 \| (\S+)", text, re.M):
        rmw, rep, arm, hv = m.group(1), int(m.group(2)), m.group(3), m.group(4)
        stem = f"{rmw}_bench_best_effort_rep{rep}_block_sst_inj{arm}_cnt"
        f = cdir / f"{stem}.txt"
        c = parse_counters(f.read_text(errors="replace")) if f.exists() else None
        p50, why = None, ""
        if not hv.startswith("ok"):
            why = f"harness {hv}"
        elif c is None:
            why = "counts missing"
        elif stem in offs:
            v, _ = W.row_reading(out, stem, offs[stem], arm)
            _, res = RO.samples(out, stem, offs[stem])
            rtid = statistics.mode([pr[0] for _, _, pr in res]) if res else None
            if rtid != c["tid"]:
                why = f"counted tid {c['tid']} is not the receiving tid {rtid}"
            p50 = v
        rows[(rmw, arm, rep)] = (c, p50, why)
        print(f"  counter {stem:62s} {'NOT READ (' + why + ')' if why else 'ok'}"
              + (f" | tid {c['tid']} " + " ".join(f"{k}={v}" for k, v in c["ev"].items())
                 + f" same={c['same']} moved={c['moved']} migrate={c['migrate']} p50={p50}" if c else ""))
    # perturbation of the control under the instruments
    cyc = {a: {r: v[1] for (x, a2, r), v in rows.items() if x == CYC and a2 == a and v[1] is not None} for a in ARMS}
    dc = W.paired(cyc, "none", "50")
    if not dc:
        print("  COUNTERS NOT READ: no CycloneDDS counter row pair")
        return
    mdc = statistics.fmean(dc)
    print(f"  CycloneDDS d (none - 50) over its counter rows: {mdc:+.2f} us n={len(dc)}")
    if mdc > CYC_D_MAX:
        print(f"  COUNTERS NOT READ: the instruments perturbed the control (d {mdc:+.2f} > {CYC_D_MAX:+.1f})")
        return
    print("  --- counters per received ping (mean over reps; DESCRIPTIVE, decide nothing)")
    for rmw in (A, CYC):
        per = {}
        for arm in ARMS:
            ok = [v[0] for (x, a2, _), v in rows.items() if x == rmw and a2 == arm and not v[2]]
            if not ok:
                continue
            per[arm] = {k: statistics.fmean(c["ev"][k] / PINGS for c in ok) for k in EVENTS}
            per[arm]["moved_share"] = statistics.fmean(c["moved"] / max(1, c["same"] + c["moved"]) for c in ok)
            per[arm]["migrate"] = statistics.fmean(c["migrate"] for c in ok)
            per[arm]["n"] = len(ok)
            print(f"  {rmw:22s} {arm:>4s} n={len(ok)}: " + " ".join(f"{k}={per[arm][k]:.0f}" for k in EVENTS)
                  + f" moved_share={per[arm]['moved_share']:.2f} migrate/row={per[arm]['migrate']:.1f}")
        if "none" in per and "50" in per:
            print(f"  {rmw:22s} none-50: " + " ".join(f"{k}={per['none'][k] - per['50'][k]:+.0f}" for k in EVENTS)
                  + f" moved_share={per['none']['moved_share'] - per['50']['moved_share']:+.2f}")


def selftest():
    def arms(none, l50, n=13, jit=0.3):
        return {"none": {r: none + jit * ((r * 7) % 5 - 2) / 2 for r in range(n)},
                "50": {r: l50 + jit * ((r * 3) % 5 - 2) / 2 for r in range(n)}}
    ref, cyc = arms(15.5, 13.2), arms(12.9, 12.4)
    cases = [
        ({A: ref, D: arms(13.6, 13.2), CYC: cyc}, "d EXPLAINS"),
        ({A: ref, D: arms(15.3, 13.2), CYC: cyc}, "d NOT EXPLAINED"),
        ({A: ref, D: arms(14.2, 13.2), CYC: cyc}, "d NOT EXPLAINED"),  # E +0.5
        ({A: ref, D: ref, CYC: arms(12.9, 11.7)}, "VOID(control d50"),
        ({A: ref, D: ref, CYC: arms(14.5, 14.0)}, "VOID(control none"),
        ({A: ref, D: arms(13.6, 13.2, n=9), CYC: cyc}, "VOID(rmw_tickle@libc_rb_nolazy: 9"),
    ]
    bad = 0
    for p, want in cases:
        got, _ = decide(p)
        ok = got.startswith(want)
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} want {want:34s} got {got}")
    txt = ("task 7 pong_node\ncounted 7\n--- perf\n5,,dTLB-load-misses,1,100.00,,\n6,,L1-dcache-load-misses,1,100.00,,\n"
           "7,,instructions,1,100.00,1.0,insn per cycle\n8,,cycles,1,100.00,,\n--- migr\n# c12 migr tid 7\n@same: 9\n@moved: 2\n")
    c = parse_counters(txt)
    ok = c is not None and c["tid"] == "7" and c["ev"]["cycles"] == 8 and c["moved"] == 2 and c["migrate"] == 0
    ok = ok and parse_counters(txt.replace("8,,cycles", "x")) is None
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} counter file parsed; one missing event -> not read")
    log = ("c12 readback bind: gro_probe=1 data_gro=0 wk_gro=0 data_no_check=0 wk_no_check=0\n rx: csum0=1 x timer_waits=0 "
           "timer_sets=0 \nc12 readback close: gro_probe=1 data_gro=0 wk_gro=0 data_no_check=0 wk_no_check=0\n")
    h = __import__("struct").pack("<qq", 5_000_000, 1).hex()
    res = [(1, 15.0, ("7", "io_uring_enter", 0, 5, 0, h))]
    ok = readback(log, D, res, [])[0] and not readback(log.replace("timer_sets=0", "timer_sets=3"), D, res, [])[0]
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} (d) readback passes with no timer, fails with timer_sets=3")
    print("SELFTEST OK" if bad == 0 else f"SELFTEST FAILED {bad}")
    return 1 if bad else 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--selftest":
        return selftest()
    if len(argv) > 1 and argv[1] == "--reps-needed":
        print(N_REPS)
        return 0
    out = Path(argv[1])
    text = out.read_text()
    offs = R.offsets(text)
    verdicts = {}
    for m in re.finditer(r"^(\S+) bench best_effort rep(\d+) wait=block sst=on inject=(\S+) \| (\S+)", text, re.M):
        verdicts[(m.group(1), int(m.group(2)), m.group(3))] = m.group(4)
    reps = sorted({k[1] for k in verdicts})
    print(f"=== C12 step 4 reading of {out.name}: {len(reps)} reps, N_REPS {N_REPS}, MIN_PAIRED {MIN_PAIRED}")
    W.R.samples = RO.samples
    logs = out.with_suffix(out.suffix + ".logs")
    p = {r: {a: {} for a in ARMS} for r in RMWS}
    for rmw in RMWS:
        for arm in ARMS:
            for rep in reps:
                stem = f"{rmw}_bench_best_effort_rep{rep}_block_sst_inj{arm}"
                hv = verdicts.get((rmw, rep, arm))
                if hv is None or stem not in offs:
                    print(f"  {stem}: MISSING")
                    continue
                if not hv.startswith("ok"):
                    print(f"  {stem}: harness {hv}")
                    continue
                v, line = W.row_reading(out, stem, offs[stem], arm)
                if v is not None and rmw != CYC:
                    recs, res = RO.samples(out, stem, offs[stem])
                    lf = logs / f"{stem}_pong.log"
                    ok, rb = readback(lf.read_text(errors="replace") if lf.exists() else "", rmw, res, recs)
                    line += " | " + rb
                    if not ok:
                        v = None
                        line = "VOID(readback) " + line
                print(f"  {stem:62s} {line}")
                if v is not None:
                    p[rmw][arm][rep] = v
    print("--- arms (rep p50s; us)")
    for rmw in RMWS:
        for arm in ARMS:
            xs = sorted(p[rmw][arm].values())
            if xs:
                print(f"  {rmw:26s} {arm:>4s}: n_reps={len(xs):2d} median {statistics.median(xs):6.2f} "
                      f"mean {statistics.fmean(xs):6.2f} range {xs[0]:.2f}-{xs[-1]:.2f} (spread {xs[-1] - xs[0]:.2f})")
    print("--- counter rows (descriptive only)")
    counters(out, text, offs)
    verdict, lines = decide(p)
    for line in lines:
        print("  " + line)
    print(f"STEP4_VERDICT {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
