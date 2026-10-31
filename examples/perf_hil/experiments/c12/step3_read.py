#!/usr/bin/env python3
"""C12 idle wake, step 3: the sockets' options as TickLE's idle cost. The pre-registered reader of step3_rig.sh, written
2026-10-31 before any of its runs.

ARMS (wait ring as shipped; all built from b6f0c635 + c12/step3.patch, whose code is inert without its flags, and all
with TT_EXP_C12_READBACK, which reads the sockets' options back with getsockopt() at bind and at close):
  (a) rmw_tickle@libc_rb        the reference
  (b) rmw_tickle@libc_rb_csum   UDP checksum kept: TT_UDP_CSUM0=0 in the environment, so csum0_setup() never sets
                                SO_NO_CHECK on the data socket (by default it is set on and off once at bind, and on only
                                before a send of a datagram larger than a frame - which this bench never makes)
  (c) rmw_tickle@libc_rb_nogro  UDP_GRO never touched: gro_setup() returns before its probe (TT_EXP_NO_GRO_SETUP); by
                                default the probe sets UDP_GRO on and off once on both sockets at bind
  CycloneDDS the control. Injection none and 50 ms (idle_wake_injector.py). (d), the deadline timerfd left out of the wait
  set, is not run: five arms (130 rows, ~2.6 h) do not fit the time given; four do (104 rows).

MEASURED and row validity: idle_wake_read.py's (receive matched by ringoff_read.samples()). TREATMENT READBACK per TickLE
row, from the pong's own log ("c12 readback bind|close: gro_probe= data_gro= wk_gro= data_no_check= wk_no_check=", and
the HAL's "csum0="), all must hold or the row is VOID:
  every arm: both readback lines present; data_gro = wk_gro = 0 at bind and at close (offload off for small traffic);
  (a): csum0=1, gro_probe=1 (the probe ran and read UDP_GRO on);
  (b): csum0=0, gro_probe=1, data_no_check=0 at close;
  (c): csum0=1, gro_probe=-1 (the probe never ran).

STATISTIC (the coordinator's rule, 2026-10-31): CycloneDDS's own none - 50 ms difference moved +0.49 / +0.45 / +0.52 us
across steps 0-2, so it is subtracted rather than bounded: per rep, E_x = d_x - d_cyclone, with d = p50(none) - p50(50);
mean and t 95% CI over paired reps. n: N_REPS = 13 (idle_wake_read.py's formula).
VOID   a cell with < MIN_PAIRED (10) paired valid reps (a treatment readback that fails voids its row); CycloneDDS's none
       arm (median rep p50) outside 12.9 +- 1.0 us; CycloneDDS's mean d above +1.0 us.
A candidate EXPLAINS THE COST when E_x < EXPLAIN_E (+0.4 us) and its CI high end < the reference's E CI low end.
Verdict: "b EXPLAINS", "c EXPLAINS", "b AND c EXPLAIN", "NEITHER EXPLAINS". Both raw d50s are printed beside it.

Usage: step3_read.py <OUT file> | --selftest | --reps-needed     Last line: "STEP3_VERDICT ..." (always).
"""
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import idle_wake_read as W  # noqa: E402
import ringoff_read as RO  # noqa: E402

R = W.R
A, B, C, CYC = "rmw_tickle@libc_rb", "rmw_tickle@libc_rb_csum", "rmw_tickle@libc_rb_nogro", "rmw_cyclonedds_cpp"
RMWS = (A, B, C, CYC)
ARMS = ("none", "50")
N_REPS, MIN_PAIRED = W.N_REPS, W.MIN_PAIRED
CYC_NONE_US, CYC_NONE_TOL, CYC_D_MAX = 12.9, 1.0, 1.0
EXPLAIN_E = 0.4
EXPECT = {A: {"csum0": 1, "gro_probe": 1}, B: {"csum0": 0, "gro_probe": 1}, C: {"csum0": 1, "gro_probe": -1}}


def readback(log_text, rmw):
    """(ok, line) from the pong log."""
    rb = {}
    for m in re.finditer(r"c12 readback (bind|close): gro_probe=(-?\d+) data_gro=(-?\d+) wk_gro=(-?\d+) "
                         r"data_no_check=(-?\d+) wk_no_check=(-?\d+)", log_text):
        rb[m.group(1)] = dict(zip(("gro_probe", "data_gro", "wk_gro", "data_no_check", "wk_no_check"),
                                  (int(x) for x in m.groups()[1:])))
    cs = re.findall(r" csum0=(-?\d+) ", log_text)
    line = f"readback: bind {rb.get('bind')} close {rb.get('close')} csum0={cs[-1] if cs else 'missing'}"
    if "bind" not in rb or "close" not in rb or not cs:
        return False, "VOID(readback missing) " + line
    why = []
    for when in ("bind", "close"):
        if rb[when]["data_gro"] != 0 or rb[when]["wk_gro"] != 0:
            why.append(f"GRO on at {when}")
    if int(cs[-1]) != EXPECT[rmw]["csum0"]:
        why.append(f"csum0 {cs[-1]} not {EXPECT[rmw]['csum0']}")
    if rb["bind"]["gro_probe"] != EXPECT[rmw]["gro_probe"]:
        why.append(f"gro_probe {rb['bind']['gro_probe']} not {EXPECT[rmw]['gro_probe']}")
    if rmw == B and rb["close"]["data_no_check"] != 0:
        why.append("SO_NO_CHECK set at close")
    if why:
        return False, f"VOID(readback: {'; '.join(why)}) " + line
    return True, line


def excess(p, x):
    reps = [r for r in p[x]["none"] if r in p[x]["50"] and r in p[CYC]["none"] and r in p[CYC]["50"]]
    return [(p[x]["none"][r] - p[x]["50"][r]) - (p[CYC]["none"][r] - p[CYC]["50"][r]) for r in reps]


def decide(p):
    lines = []
    d = {r: W.paired(p[r], "none", "50") for r in RMWS}
    e = {x: excess(p, x) for x in (A, B, C)}
    for x in RMWS:
        if len(d[x]) < MIN_PAIRED:
            return f"VOID({x}: {len(d[x])} paired none/50 reps < {MIN_PAIRED})", lines
    for x in (A, B, C):
        if len(e[x]) < MIN_PAIRED:
            return f"VOID({x}: {len(e[x])} reps paired with the control < {MIN_PAIRED})", lines
    ci_d = {x: W.mean_ci(d[x]) for x in RMWS}
    ci_e = {x: W.mean_ci(e[x]) for x in (A, B, C)}
    cnone = statistics.median(p[CYC]["none"].values())
    for x in RMWS:
        m, lo, hi = ci_d[x]
        lines.append(f"raw d50 {x:26s} {m:+.2f} us [{lo:+.2f}, {hi:+.2f}] n={len(d[x])}")
    for x in (A, B, C):
        m, lo, hi = ci_e[x]
        lines.append(f"E       {x:26s} {m:+.2f} us [{lo:+.2f}, {hi:+.2f}] n={len(e[x])}")
    lines.append(f"cyclone none median {cnone:.2f}")
    if abs(cnone - CYC_NONE_US) > CYC_NONE_TOL:
        return f"VOID(control none arm {cnone:.2f} us, not {CYC_NONE_US} +- {CYC_NONE_TOL})", lines
    if ci_d[CYC][0] > CYC_D_MAX:
        return f"VOID(control d50 {ci_d[CYC][0]:+.2f} us > {CYC_D_MAX:+.1f})", lines
    expl = [n for n, x in (("b", B), ("c", C)) if ci_e[x][0] < EXPLAIN_E and ci_e[x][2] < ci_e[A][1]]
    if not expl:
        return "NEITHER EXPLAINS", lines
    return ("b AND c EXPLAIN" if len(expl) == 2 else f"{expl[0]} EXPLAINS"), lines


def selftest():
    def arms(none, l50, n=13, jit=0.3):
        return {"none": {r: none + jit * ((r * 7) % 5 - 2) / 2 for r in range(n)},
                "50": {r: l50 + jit * ((r * 3) % 5 - 2) / 2 for r in range(n)}}
    ref, cyc = arms(15.5, 13.2), arms(12.9, 12.4)  # cyclone d +0.5: ref E +1.8
    cases = [
        ({A: ref, B: arms(13.6, 13.2), C: ref, CYC: cyc}, "b EXPLAINS"),
        ({A: ref, B: ref, C: arms(13.7, 13.2), CYC: cyc}, "c EXPLAINS"),
        ({A: ref, B: arms(13.6, 13.2), C: arms(13.7, 13.2), CYC: cyc}, "b AND c EXPLAIN"),
        ({A: ref, B: ref, C: arms(14.2, 13.2), CYC: cyc}, "NEITHER EXPLAINS"),  # E +0.5: not below 0.4
        ({A: ref, B: arms(13.7, 13.2, jit=5.0), C: ref, CYC: cyc}, "NEITHER EXPLAINS"),  # wide CI
        ({A: ref, B: ref, C: ref, CYC: arms(12.9, 11.7)}, "VOID(control d50"),
        ({A: ref, B: ref, C: ref, CYC: arms(14.5, 14.0)}, "VOID(control none"),
        ({A: ref, B: arms(13.6, 13.2, n=9), C: ref, CYC: cyc}, "VOID(rmw_tickle@libc_rb_csum: 9"),
        ({A: ref, B: ref, C: ref, CYC: arms(12.9, 12.0)}, "NEITHER EXPLAINS"),  # control +0.9: within +1.0, decides
    ]
    bad = 0
    for p, want in cases:
        got, _ = decide(p)
        ok = got.startswith(want)
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} want {want:34s} got {got}")
    log_c = ("x c12 readback bind: gro_probe=-1 data_gro=0 wk_gro=0 data_no_check=0 wk_no_check=0\n"
             "y rx: ... csum0=1 csum0_switches=0\nz c12 readback close: gro_probe=-1 data_gro=0 wk_gro=0 data_no_check=0 "
             "wk_no_check=0\n")
    ok = readback(log_c, C)[0] and not readback(log_c, A)[0] and not readback("", A)[0]
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} readback: (c)'s log passes c, fails a; an empty log fails")
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
    print(f"=== C12 step 3 reading of {out.name}: {len(reps)} reps, N_REPS {N_REPS}, MIN_PAIRED {MIN_PAIRED}")
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
                    lf = logs / f"{stem}_pong.log"
                    ok, rb = readback(lf.read_text(errors="replace") if lf.exists() else "", rmw)
                    line += " | " + rb
                    if not ok:
                        v = None
                        line = "VOID(readback) " + line
                print(f"  {stem:64s} {line}")
                if v is not None:
                    p[rmw][arm][rep] = v
    print("--- arms (rep p50s; us)")
    for rmw in RMWS:
        for arm in ARMS:
            xs = sorted(p[rmw][arm].values())
            if xs:
                print(f"  {rmw:26s} {arm:>4s}: n_reps={len(xs):2d} median {statistics.median(xs):6.2f} "
                      f"mean {statistics.fmean(xs):6.2f} range {xs[0]:.2f}-{xs[-1]:.2f} (spread {xs[-1] - xs[0]:.2f})")
    verdict, lines = decide(p)
    for line in lines:
        print("  " + line)
    print(f"STEP3_VERDICT {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
