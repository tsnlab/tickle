#!/usr/bin/env python3
"""C12 idle wake, step 2: is TickLE's idle cost its timer slack or its timed wait? The pre-registered reader of
step2_rig.sh, written 2026-10-31 before any of its runs.

BACKGROUND: an injected wake 50 ms before each ping takes ~2.2-2.3 us off rmw_tickle@libc's pong receive and ~0.5 us off
CycloneDDS's (idle_wake_rig.sh); with the wait ring compiled out the cost stays (ringoff_rig.sh: NOT THE RING). Two things
TickLE's waiting thread does that CycloneDDS's blocking recvmsg does not:
  (b) libc_noslack: hal_linux.c lowers the polling thread's timer slack to 1 ns (TT_RX_TIMER_SLACK_NS 1); this arm builds
      with TT_RX_TIMER_SLACK_NS=0, which leaves the kernel's default (50 us).
  (c) libc_far: every wait is timed to the next scheduled entry, and the pong always has one (its 1 Hz announcement and
      the poll loop's own budget), so "no timeout while nothing is scheduled" never happens; the arm is "timeout far in
      the future" instead: TT_EXP_WAIT_FAR_NS = 10 s (c12/wait_far.patch, experiment only, off by default) makes every
      timed wait 10 s long with no deadline timer.
  (a) libc, the reference. All three are built from the same patched tree (the patch is inert without its flag).
CycloneDDS is the control. Arms: injection none and 50 ms (idle_wake_injector.py).

MEASURED and row validity: idle_wake_read.py's (via ringoff_read.samples()). TREATMENT READBACK, per TickLE row, from
sysstamp's io_uring_enter heads (timeout given to the wait, the thread's timer slack via PR_GET_TIMERSLACK - the /proc
file /proc/<tid>/timerslack_ns is refused to a non-ancestor under Yama ptrace_scope 1 on the Pis, and bpftrace cannot
read task_struct there, so the slack is read by the thread itself):
  - slack: every sample on the thread that receives the pings, after the first ping, equals EXPECT_SLACK[arm]
    (1 for a and c, 50000 for b); at least MIN_SLACK_SAMPLES of them;
  - the timeout of the waits that received the pings: printed for every arm (share without one, median); for c, at least
    FAR_FRAC of them must be >= FAR_MIN_NS, and the thread arms no deadline timer (timerfd_settime) after the first ping.
  A row whose readback fails is VOID. Each row prints its readback.

SAMPLE SIZE: N_REPS = 13 (idle_wake_read.py's formula); < MIN_PAIRED (10) paired valid reps in a cell -> VOID.

DECISION (d = per rep p50(none) - p50(50), paired by rep; mean, t 95% CI):
  VOID       control: CycloneDDS's none arm outside 12.9 +- 1.0 us or |d_cyclone| >= 0.5 us; the reference not reproducing
             (d_a < 1.5 us or CI low <= 0.5); too few paired reps.
  A candidate EXPLAINS THE COST when its d < EXPLAIN_D (0.8 us) and its CI high end < (a)'s CI low end.
  Verdict: "b EXPLAINS", "c EXPLAINS", "b AND c EXPLAIN", or "NEITHER EXPLAINS".
Printed, not deciding: d_a - d_x paired per candidate, and every arm's gap to CycloneDDS.

Usage: step2_read.py <OUT file> | --selftest | --reps-needed      Last line: "STEP2_VERDICT ..." (always).
"""
import re
import statistics
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import idle_wake_read as W  # noqa: E402
import ringoff_read as RO  # noqa: E402

S, R = W.S, W.R
A, B, C, CYC = "rmw_tickle@libc", "rmw_tickle@libc_noslack", "rmw_tickle@libc_far", "rmw_cyclonedds_cpp"
RMWS = (A, B, C, CYC)
ARMS = ("none", "50")
N_REPS, MIN_PAIRED = W.N_REPS, W.MIN_PAIRED
EXPECT_SLACK = {A: 1, B: 50000, C: 1}
MIN_SLACK_SAMPLES = 3
FAR_MIN_NS, FAR_FRAC = 9_000_000_000, 0.9
EXPLAIN_D = 0.8
REPRO_US, REPRO_LO = 1.5, 0.5


def head_ints(h):
    """The two int64 LE of an io_uring_enter / timerfd_settime head, or (None, None)."""
    if len(h) != 32:
        return None, None
    return struct.unpack("<qq", bytes.fromhex(h))


def readback(out, stem, rmw, res, recs):
    """(ok, line): the slack and the timeouts the receiving thread actually had."""
    if not res:
        return False, "readback: no sample"
    tid = statistics.mode([pr[0] for _, _, pr in res])
    first = res[0][0]
    slacks = []
    timerfd = 0
    for r in recs:
        if r[0] != tid or r[3] < first:
            continue
        if r[1] == "io_uring_enter":
            _, sl = head_ints(r[5])
            if sl is not None and sl >= 0:
                slacks.append(sl)
        elif r[1] == "timerfd_settime":
            timerfd += 1
    # the wait that received each ping: the io_uring_enter whose return is the receive (pr itself for the ring)
    waits = [head_ints(pr[5])[0] for _, _, pr in res if pr[1] == "io_uring_enter"]
    waits = [w for w in waits if w is not None]
    none_share = sum(w < 0 for w in waits) / len(waits) if waits else float("nan")
    timed = sorted(w for w in waits if w >= 0)
    med = timed[len(timed) // 2] / 1e6 if timed else float("nan")
    line = (f"readback: slack {sorted(set(slacks))} (n={len(slacks)}), receiving waits {len(waits)}: without timeout "
            f"{none_share:.2f}, timed median {med:.1f} ms, timerfd_settime after first ping {timerfd}")
    exp = EXPECT_SLACK[rmw]
    if len(slacks) < MIN_SLACK_SAMPLES or any(s != exp for s in slacks):
        return False, f"VOID(slack not {exp}) {line}"
    if rmw == C:
        far = sum(w >= FAR_MIN_NS for w in waits)
        if not waits or far < FAR_FRAC * len(waits) or timerfd:
            return False, f"VOID(far timeout not applied: {far}/{len(waits)} >= 9 s, timerfd {timerfd}) {line}"
    return True, line


def decide(p):
    lines = []
    d = {r: W.paired(p[r], "none", "50") for r in RMWS}
    for r in RMWS:
        if len(d[r]) < MIN_PAIRED:
            return f"VOID({r}: {len(d[r])} paired none/50 reps < {MIN_PAIRED})", lines
    ci = {r: W.mean_ci(d[r]) for r in RMWS}
    cnone = statistics.median(p[CYC]["none"].values())
    for r in RMWS:
        m, lo, hi = ci[r]
        lines.append(f"d50 {r:24s} {m:+.2f} us [{lo:+.2f}, {hi:+.2f}] n={len(d[r])}")
    lines.append(f"cyclone none median {cnone:.2f}")
    if abs(cnone - W.CYC_NONE_US) > W.CYC_NONE_TOL:
        return f"VOID(control none arm {cnone:.2f} us, not {W.CYC_NONE_US} +- {W.CYC_NONE_TOL})", lines
    if abs(ci[CYC][0]) >= W.CONTROL_TOL_US:
        return f"VOID(control moved with the injection: d50 cyclone {ci[CYC][0]:+.2f} us)", lines
    if ci[A][0] < REPRO_US or ci[A][1] <= REPRO_LO:
        return f"VOID(reference not reproduced: d50 libc {ci[A][0]:+.2f} us, CI low {ci[A][1]:+.2f})", lines
    expl = [x for x, r in (("b", B), ("c", C)) if ci[r][0] < EXPLAIN_D and ci[r][2] < ci[A][1]]
    if not expl:
        return "NEITHER EXPLAINS", lines
    return ("b AND c EXPLAIN" if len(expl) == 2 else f"{expl[0]} EXPLAINS"), lines


def selftest():
    def arms(none, l50, n=13, jit=0.3):
        return {"none": {r: none + jit * ((r * 7) % 5 - 2) / 2 for r in range(n)},
                "50": {r: l50 + jit * ((r * 3) % 5 - 2) / 2 for r in range(n)}}
    ref, cyc = arms(15.5, 13.2), arms(12.9, 12.6)
    cases = [
        ({A: ref, B: arms(13.5, 13.2), C: arms(15.4, 13.2), CYC: cyc}, "b EXPLAINS"),
        ({A: ref, B: arms(15.4, 13.2), C: arms(13.5, 13.2), CYC: cyc}, "c EXPLAINS"),
        ({A: ref, B: arms(13.5, 13.2), C: arms(13.4, 13.2), CYC: cyc}, "b AND c EXPLAIN"),
        ({A: ref, B: arms(15.4, 13.2), C: arms(14.6, 13.2), CYC: cyc}, "NEITHER EXPLAINS"),  # c +1.4: not below 0.8
        ({A: ref, B: arms(15.4, 13.2), C: arms(13.9, 13.2, jit=4.0), CYC: cyc}, "NEITHER EXPLAINS"),  # wide CI
        ({A: arms(14.0, 13.2), B: ref, C: ref, CYC: cyc}, "VOID(reference not reproduced"),
        ({A: ref, B: ref, C: ref, CYC: arms(12.9, 12.0)}, "VOID(control moved"),
        ({A: ref, B: arms(13.5, 13.2, n=9), C: ref, CYC: cyc}, "VOID(rmw_tickle@libc_noslack: 9"),
    ]
    bad = 0
    for p, want in cases:
        got, _ = decide(p)
        ok = got.startswith(want)
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} want {want:34s} got {got}")
    h = struct.pack("<qq", 10_000_000_000, 1).hex()
    recs = [("7", "io_uring_enter", 0, 5, 0, h)] * 4 + [("7", "io_uring_enter", 6, 10, 0, h)]
    res = [(1, 15.0, ("7", "io_uring_enter", 0, 5, 0, h))]
    ok1, line = readback(None, "x", C, res, recs)
    ok2, _ = readback(None, "x", B, res, recs)
    ok = ok1 and not ok2
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} readback: far applied passes c ({line}); slack 1 fails b")
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
    print(f"=== C12 step 2 reading of {out.name}: {len(reps)} reps, N_REPS {N_REPS}, MIN_PAIRED {MIN_PAIRED}")
    W.R.samples = RO.samples  # as ringoff_read.py: the receive matched by key or ring, asleep judged at the wait
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
                    ok, rb = readback(out, stem, rmw, res, recs)
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
                print(f"  {rmw:24s} {arm:>4s}: n_reps={len(xs):2d} median {statistics.median(xs):6.2f} "
                      f"mean {statistics.fmean(xs):6.2f} range {xs[0]:.2f}-{xs[-1]:.2f} (spread {xs[-1] - xs[0]:.2f})")
    for x in (B, C):
        both = [r for r in reps if all(r in p[y][a] for y in (A, x) for a in ARMS)]
        if len(both) >= 2:
            diff = [(p[A]["none"][r] - p[A]["50"][r]) - (p[x]["none"][r] - p[x]["50"][r]) for r in both]
            m, lo, hi = W.mean_ci(diff)
            print(f"  (not deciding) d50 libc - d50 {x} {m:+.2f} us [{lo:+.2f}, {hi:+.2f}] n={len(diff)}")
    for x in (A, B, C):
        for arm in ARMS:
            g = W.paired({"t": p[x][arm], "c": p[CYC][arm]}, "t", "c")
            if g:
                print(f"  (not deciding) {x} - CycloneDDS, arm {arm:>4s}: {statistics.fmean(g):+.2f} us n={len(g)}")
    verdict, lines = decide(p)
    for line in lines:
        print("  " + line)
    print(f"STEP2_VERDICT {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
