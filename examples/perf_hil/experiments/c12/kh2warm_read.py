#!/usr/bin/env python3
"""KH2 warm, step A (release 2.1, ROADMAP item (i)): the rmw receive-return gap on the Pi split into its idle and warm
cells. The pre-registered reader of kh2warm_rig.sh, written 2026-11-01 before any of its runs.

BACKGROUND: RESULTS' headline "+2.0 / +1.1 us after CycloneDDS / FastDDS" (KH2B) comes from a ~100 ms-paced pong, i.e. the
idle cell; C12 closed the idle part (RESULTS C12I). This run measures, on main 4df669cc, unpinned, 13 reps, the C12
harness's tap -> receive-return for rmw_tickle@libc (the shipped receive path, libc build so sysstamp sees it),
rmw_cyclonedds_cpp and rmw_fastrtps_cpp, with the injected wake none (idle) and 50 ms (warm; idle_wake_injector.py).
Measurement only, no code.

MEASURED: per row, the p50 of tap -> receive return over the pings idle_wake_read.py's sample sets use (none: the last
wake the previous ping 95-105 ms before; 50: an injected datagram 40-60 ms before), the receive matched by its key
(recv*) or, for TickLE's ring, the io_uring_enter entered before the tap and returned first after it (rxthread_read.py's
samples()). Row validity and the treatment checks are idle_wake_read.py's row_reading() (a row failing them is VOID).

REPORTED: per rmw and arm, n reps, median and mean of the rep p50s with the t 95% CI of the mean; per arm, TickLE minus
each vendor, paired by rep, mean with t 95% CI. The restated headline: idle gap (none) and warm gap (50 ms) to both.
VOID  any row failing its readback (harness verdict or row_reading); a cell with < 10 reps, or < 10 paired reps for a gap;
      CycloneDDS's none median outside 12.9 +- 1.0 us.
STEP B is called for when TickLE's warm gap to either vendor is >= +0.4 us with its CI low end > 0 ("STEP_B NEEDED"),
otherwise "STEP_B NOT NEEDED".

Usage: kh2warm_read.py <OUT file> | --selftest | --reps-needed     Last line: "KH2WARM_VERDICT ..." (always).
"""
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import idle_wake_read as W  # noqa: E402
import rxthread_read as X  # noqa: E402

R = W.R
T, C, F = "rmw_tickle@libc", "rmw_cyclonedds_cpp", "rmw_fastrtps_cpp"
RMWS = (T, C, F)
ARMS = ("none", "50")
N_REPS, MIN_PAIRED = W.N_REPS, W.MIN_PAIRED
CYC_NONE_US, CYC_NONE_TOL = 12.9, 1.0
STEP_B_US = 0.4


def gap(p, arm, vendor):
    return W.paired({"t": p[T][arm], "v": p[vendor][arm]}, "t", "v")


def decide(p, fails):
    lines = []
    if fails:
        return f"VOID({fails} rows failed their readback)", lines
    for r in RMWS:
        for a in ARMS:
            if len(p[r][a]) < MIN_PAIRED:
                return f"VOID({r} {a}: {len(p[r][a])} reps < {MIN_PAIRED})", lines
    for r in RMWS:
        for a in ARMS:
            xs = list(p[r][a].values())
            m, lo, hi = W.mean_ci(xs)
            lines.append(f"{r:20s} {a:>4s}: n={len(xs)} median {statistics.median(xs):.2f} mean {m:.2f} "
                         f"[{lo:.2f}, {hi:.2f}] us")
    g = {}
    for a in ARMS:
        for v in (C, F):
            ds = gap(p, a, v)
            if len(ds) < MIN_PAIRED:
                return f"VOID(gap {a} vs {v}: {len(ds)} paired reps < {MIN_PAIRED})", lines
            g[(a, v)] = W.mean_ci(ds) + (len(ds),)
            m, lo, hi, n = g[(a, v)]
            lines.append(f"TickLE - {v:18s} {a:>4s}: {m:+.2f} us [{lo:+.2f}, {hi:+.2f}] n={n}")
    cnone = statistics.median(p[C]["none"].values())
    lines.append(f"cyclone none median {cnone:.2f}")
    if abs(cnone - CYC_NONE_US) > CYC_NONE_TOL:
        return f"VOID(control none arm {cnone:.2f} us, not {CYC_NONE_US} +- {CYC_NONE_TOL})", lines
    head = (f"HEADLINE idle gap {g[('none', C)][0]:+.2f} / {g[('none', F)][0]:+.2f} us, warm gap "
            f"{g[('50', C)][0]:+.2f} / {g[('50', F)][0]:+.2f} us after CycloneDDS / FastDDS")
    lines.append(head)
    need = [v for v in (C, F) if g[("50", v)][0] >= STEP_B_US and g[("50", v)][1] > 0]
    return ("STEP_B NEEDED (warm gap to " + ", ".join(need) + ")") if need else "STEP_B NOT NEEDED", lines


def selftest():
    def arm(v, n=13, jit=0.3):
        return {r: v + jit * ((r * 7) % 5 - 2) / 2 for r in range(n)}
    def p_of(t_none, t_50, c_none=12.9, c_50=12.4, f_none=14.0, f_50=13.0, n=13):
        return {T: {"none": arm(t_none, n), "50": arm(t_50, n)}, C: {"none": arm(c_none), "50": arm(c_50)},
                F: {"none": arm(f_none), "50": arm(f_50)}}
    cases = [
        (p_of(15.3, 13.2), 0, "STEP_B NEEDED (warm gap to rmw_cyclonedds_cpp"),   # +0.8 vs C, +0.2 vs F
        (p_of(15.3, 12.6), 0, "STEP_B NOT NEEDED"),                               # +0.2 / -0.4
        (p_of(15.3, 13.6), 0, "STEP_B NEEDED (warm gap to rmw_cyclonedds_cpp, rmw_fastrtps_cpp"),
        (p_of(15.3, 13.2, c_none=14.5), 0, "VOID(control none"),
        (p_of(15.3, 13.2), 1, "VOID(1 rows"),
        (p_of(15.3, 13.2, n=9), 0, "VOID(rmw_tickle@libc none: 9"),
    ]
    bad = 0
    for p, fails, want in cases:
        got, _ = decide(p, fails)
        ok = got.startswith(want)
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} want {want[:44]:44s} got {got}")
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
    print(f"=== KH2 warm (step A) reading of {out.name}: {len(reps)} reps, N_REPS {N_REPS}")
    W.R.samples = X.samples
    p = {r: {a: {} for a in ARMS} for r in RMWS}
    fails = 0
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
                    fails += 1
                    continue
                v, line = W.row_reading(out, stem, offs[stem], arm)
                print(f"  {stem:60s} {line}")
                if v is None:
                    fails += 1
                else:
                    p[rmw][arm][rep] = v
    verdict, lines = decide(p, fails)
    for line in lines:
        print("  " + line)
    print(f"KH2WARM_VERDICT {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
