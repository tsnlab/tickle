#!/usr/bin/env python3
"""C12 idle wake, step 5: is TickLE's idle cost the cold core? The pre-registered reader of step5_rig.sh, written
2026-10-31 before any of its runs. A DIAGNOSTIC: the pongs are pinned to one core, so no figure here is a headline one.

ARMS (every pong, TickLE and CycloneDDS, pinned with all its threads to PONG_CPU; libc_rb = b6f0c635 + c12/step3.patch's
getsockopt readback, as steps 3-4's reference):
  (a) rmw_tickle@libc_rb, no warmer                        the reference
  (w) rmw_tickle@libc_rb, c12/warmer.c pinned to PONG_CPU   5 us of spinning every 10 ms on the pong's own core, so that
                                                            core is never idle for more than 10 ms
  (x) rmw_tickle@libc_rb, the same warmer on OTHER_CPU      the control for the warmer's load
  CycloneDDS, pinned the same way, no warmer               the control
  Injection none and 50 ms (idle_wake_injector.py). n = 13 (idle_wake_read.py's formula).

STATISTIC (steps 3-4): per rep E_k = d_k - d_cyclone, d = p50(none) - p50(50); mean and t 95% CI.
READBACK, per row, all must hold, and ANY failure makes the run VOID (the coordinator's rule):
  pinning: the pong's Cpus_allowed_list read after the ping equals PONG_CPU, and every pong task's last cpu is PONG_CPU
           (the receiving thread's cpu is printed per row);
  warmer:  (w) and (x): its WARMER_END line, iters >= WARM_MIN_FRAC of expected, its cpu = PONG_CPU for (w) and OTHER_CPU
           for (x); (a) and CycloneDDS: no warmer ran (no WARMER_END in the row).
DECISION:
  VOID          any readback failure; a cell with < 10 paired valid reps; CycloneDDS's none median outside 12.9 +- 1.0;
                CycloneDDS's mean d above +1.0 us.
  CAUSE         E_w < +0.4 AND E_x - E_w >= 0.6 with non-overlapping CIs (E_x's CI low end above E_w's high end): keeping
                the pong's core from idling removes TickLE's excess, and the same load elsewhere does not.
  NOT COLD CORE E_w >= +1.0.
  PARTIAL       anything else.
E_a is printed beside them, and every raw d.

Usage: step5_read.py <OUT file> | --selftest | --reps-needed     Last line: "STEP5_VERDICT ..." (always).
"""
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import idle_wake_read as W  # noqa: E402
import ringoff_read as RO  # noqa: E402

R = W.R
TICKLE, CYC = "rmw_tickle@libc_rb", "rmw_cyclonedds_cpp"
CELLS = {"a": (TICKLE, "off"), "w": (TICKLE, "same"), "x": (TICKLE, "other"), "cyc": (CYC, "off")}
ARMS = ("none", "50")
PONG_CPU, OTHER_CPU = 2, 3
WARM_MIN_FRAC = 0.9
N_REPS, MIN_PAIRED = W.N_REPS, W.MIN_PAIRED
CYC_NONE_US, CYC_NONE_TOL, CYC_D_MAX = 12.9, 1.0, 1.0
CAUSE_E, CAUSE_GAP, NOT_E = 0.4, 0.6, 1.0


def readback(pin_text, warm_text, warm, rtid):
    """(ok, line): pinning and warmer for one row."""
    m = re.search(r"Cpus_allowed_list:\s*(\S+)", pin_text)
    tasks = re.findall(r"^task (\d+) (\d+) (.*)$", pin_text, re.M)
    cpus = {t: int(c) for t, c, _ in tasks}
    why = []
    if not m or m.group(1) != str(PONG_CPU):
        why.append(f"allowed {m.group(1) if m else 'missing'} not {PONG_CPU}")
    if not tasks or any(c != PONG_CPU for c in cpus.values()):
        why.append(f"task cpus {sorted(set(cpus.values()))}")
    wm = re.search(r"WARMER_END iters=(\d+) expected=(\d+) cpu=(\d+)", warm_text or "")
    if warm == "off":
        if wm:
            why.append("a warmer ran in an off row")
        wl = "warmer none"
    else:
        want = PONG_CPU if warm == "same" else OTHER_CPU
        if not wm:
            why.append("no WARMER_END")
            wl = "warmer missing"
        else:
            it, ex, cpu = (int(x) for x in wm.groups())
            wl = f"warmer iters {it}/{ex} on cpu {cpu}"
            if it < WARM_MIN_FRAC * ex or cpu != want:
                why.append(f"warmer {it}/{ex} on cpu {cpu}, wanted cpu {want}")
    line = f"pin {m.group(1) if m else '?'}, receiving thread {rtid} on cpu {cpus.get(rtid, '?')}, {len(tasks)} tasks, {wl}"
    return (not why), (f"VOID(readback: {'; '.join(why)}) " if why else "") + line


def decide(p, readback_failures):
    lines = []
    if readback_failures:
        return f"VOID({readback_failures} rows failed their readback)", lines
    d = {k: W.paired(p[k], "none", "50") for k in CELLS}
    for k in CELLS:
        if len(d[k]) < MIN_PAIRED:
            return f"VOID({k}: {len(d[k])} paired none/50 reps < {MIN_PAIRED})", lines
    e = {}
    for k in ("a", "w", "x"):
        reps = [r for r in p[k]["none"] if r in p[k]["50"] and r in p["cyc"]["none"] and r in p["cyc"]["50"]]
        e[k] = [(p[k]["none"][r] - p[k]["50"][r]) - (p["cyc"]["none"][r] - p["cyc"]["50"][r]) for r in reps]
        if len(e[k]) < MIN_PAIRED:
            return f"VOID({k}: {len(e[k])} reps paired with the control < {MIN_PAIRED})", lines
    cd = {k: W.mean_ci(d[k]) for k in CELLS}
    ce = {k: W.mean_ci(e[k]) for k in e}
    cnone = statistics.median(p["cyc"]["none"].values())
    for k in CELLS:
        lines.append(f"raw d50 {k:3s} {cd[k][0]:+.2f} us [{cd[k][1]:+.2f}, {cd[k][2]:+.2f}] n={len(d[k])}")
    for k in ("a", "w", "x"):
        lines.append(f"E       {k:3s} {ce[k][0]:+.2f} us [{ce[k][1]:+.2f}, {ce[k][2]:+.2f}] n={len(e[k])}")
    lines.append(f"E_x - E_w {ce['x'][0] - ce['w'][0]:+.2f} us; cyclone none median {cnone:.2f}")
    if abs(cnone - CYC_NONE_US) > CYC_NONE_TOL:
        return f"VOID(control none arm {cnone:.2f} us, not {CYC_NONE_US} +- {CYC_NONE_TOL})", lines
    if cd["cyc"][0] > CYC_D_MAX:
        return f"VOID(control d50 {cd['cyc'][0]:+.2f} us > {CYC_D_MAX:+.1f})", lines
    if ce["w"][0] < CAUSE_E and ce["x"][0] - ce["w"][0] >= CAUSE_GAP and ce["x"][1] > ce["w"][2]:
        return "CAUSE", lines
    if ce["w"][0] >= NOT_E:
        return "NOT COLD CORE", lines
    return "PARTIAL", lines


def selftest():
    def arms(none, l50, n=13, jit=0.3):
        return {"none": {r: none + jit * ((r * 7) % 5 - 2) / 2 for r in range(n)},
                "50": {r: l50 + jit * ((r * 3) % 5 - 2) / 2 for r in range(n)}}
    ref, cyc = arms(15.5, 13.2), arms(12.9, 12.4)
    cases = [
        ({"a": ref, "w": arms(13.6, 13.2), "x": ref, "cyc": cyc}, 0, "CAUSE"),
        ({"a": ref, "w": ref, "x": ref, "cyc": cyc}, 0, "NOT COLD CORE"),
        ({"a": ref, "w": arms(14.2, 13.2), "x": ref, "cyc": cyc}, 0, "PARTIAL"),   # E_w +0.5
        ({"a": ref, "w": arms(13.6, 13.2), "x": arms(13.8, 13.2), "cyc": cyc}, 0, "PARTIAL"),  # x falls too
        ({"a": ref, "w": arms(13.6, 13.2), "x": ref, "cyc": cyc}, 1, "VOID(1 rows failed"),
        ({"a": ref, "w": ref, "x": ref, "cyc": arms(12.9, 11.7)}, 0, "VOID(control d50"),
        ({"a": ref, "w": ref, "x": ref, "cyc": arms(14.5, 14.0)}, 0, "VOID(control none"),
        ({"a": ref, "w": arms(13.6, 13.2, n=9), "x": ref, "cyc": cyc}, 0, "VOID(w: 9"),
    ]
    bad = 0
    for p, fails, want in cases:
        got, _ = decide(p, fails)
        ok = got.startswith(want)
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} want {want:22s} got {got}")
    pin = "Cpus_allowed_list:\t2\ntask 7 2 pong_node\ntask 8 2 pong_node\n"
    warm = "WARMER_END iters=1600 expected=1600 cpu=2 allowed=4 work_us=5 period_us=10000\n"
    ok = (readback(pin, warm, "same", "7")[0] and not readback(pin, warm, "other", "7")[0]
          and readback(pin, "", "off", "7")[0] and not readback(pin, warm, "off", "7")[0]
          and not readback(pin.replace("task 8 2", "task 8 1"), "", "off", "7")[0]
          and not readback(pin, warm.replace("iters=1600", "iters=900"), "same", "7")[0])
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} readback: right cpu passes, wrong warmer cpu / stray warmer / unpinned task / "
          "short warmer fail")
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
    for m in re.finditer(r"^(\S+) bench best_effort rep(\d+) wait=block sst=on inject=(\S+) warm=(\S+) \| (\S+)", text,
                         re.M):
        verdicts[(m.group(1), int(m.group(2)), m.group(3), m.group(4))] = m.group(5)
    reps = sorted({k[1] for k in verdicts})
    print(f"=== C12 step 5 reading of {out.name}: {len(reps)} reps, N_REPS {N_REPS}, MIN_PAIRED {MIN_PAIRED}")
    W.R.samples = RO.samples
    pind = out.with_suffix(out.suffix + ".pin")
    p = {k: {a: {} for a in ARMS} for k in CELLS}
    fails = 0
    for k, (rmw, warm) in CELLS.items():
        for arm in ARMS:
            for rep in reps:
                stem = f"{rmw}_bench_best_effort_rep{rep}_block_sst_inj{arm}_warm{warm}"
                hv = verdicts.get((rmw, rep, arm, warm))
                if hv is None or stem not in offs:
                    print(f"  {stem}: MISSING")
                    continue
                if not hv.startswith("ok"):
                    print(f"  {stem}: harness {hv}")
                    continue
                v, line = W.row_reading(out, stem, offs[stem], arm)
                _, res = RO.samples(out, stem, offs[stem])
                rtid = statistics.mode([pr[0] for _, _, pr in res]) if res else None
                pf, wf = pind / f"{stem}.txt", pind / f"{stem}.warmer.txt"
                ok, rb = readback(pf.read_text(errors="replace") if pf.exists() else "",
                                  wf.read_text(errors="replace") if wf.exists() else "", warm, rtid)
                line += " | " + rb
                if not ok:
                    fails += 1
                    v = None
                print(f"  {k:3s} {stem:72s} {line}")
                if v is not None:
                    p[k][arm][rep] = v
    print("--- arms (rep p50s; us)")
    for k in CELLS:
        for arm in ARMS:
            xs = sorted(p[k][arm].values())
            if xs:
                print(f"  {k:3s} {arm:>4s}: n_reps={len(xs):2d} median {statistics.median(xs):6.2f} "
                      f"mean {statistics.fmean(xs):6.2f} range {xs[0]:.2f}-{xs[-1]:.2f} (spread {xs[-1] - xs[0]:.2f})")
    verdict, lines = decide(p, fails)
    for line in lines:
        print("  " + line)
    print(f"STEP5_VERDICT {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
