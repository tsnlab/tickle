#!/usr/bin/env python3
"""C12 idle wake, step 1 of the follow-up: is the wait ring (io_uring) the TickLE-only idle cost? The pre-registered
reader of ringoff_rig.sh, written 2026-10-31 before any of its runs.

QUESTION: idle_wake_rig.sh (c12iw_3178006c_20261031-055459) read SUPPORTED: an injected wake 50 ms before each ping took
2.33 us off rmw_tickle@libc's pong receive (tap -> receive return) and 0.49 us off CycloneDDS's. The same harness now
runs rmw_tickle@libc (wait ring: io_uring recvmsg, DEFER_TASKRUN) beside rmw_tickle@libc_ringoff (the ring compiled off,
-Dtt_RX_URING_WAIT=0: epoll_pwait2 then recvfrom) and CycloneDDS, arms none and 50 ms (idle_wake_injector.py).

MEASURED, per ping: v = pong tap -> return of the call that hands the datagram to user space (io_uring_enter for the ring,
recvfrom for ringoff, recvmsg for CycloneDDS), for pings the thread was already asleep for: the call that waited (the
ring's io_uring_enter; ringoff's epoll_pwait2; CycloneDDS's recvmsg) was entered before the tap. Samples, treatment
check and row validity are idle_wake_read.py's, unchanged (none: last wake = previous ping 95..105 ms before; 50: last wake
= an injected datagram 40..60 ms before; >= 50 injected at the tap, >= 90% of them seen waking the receiving thread,
>= 70% of pings carrying it; >= 50 samples a row).

SAMPLE SIZE: N_REPS = 13, idle_wake_read.py's formula ((1.96 + 0.84) * 1.24 / 1.0)^2; < MIN_PAIRED (10) paired valid
reps in any cell -> VOID.

DECISION (d = per rep p50(none) - p50(50), paired by rep; mean and t 95% CI; gap = per rep p50(ringoff, none) -
p50(CycloneDDS, none), paired):
  VOID  - the control: CycloneDDS's none arm (median rep p50) outside 12.9 +- 1.0 us, or |d_cyclone| >= 0.5 us;
        - the effect not reproduced: d_libc < 1.5 us or its CI low end <= 0.5 (without it there is nothing to attribute);
        - too few paired reps in a cell.
  RING IS THE CAUSE      d_ringoff < 0.7 us and mean gap < 1.0 us.
  NOT THE RING (falsified) d_ringoff >= 1.5 us and its CI low end > 0.5: the idle cost survives without the ring, so it
                         is in TickLE's code or socket set-up, not io_uring.
  RING CARRIES THE IDLE COST, RINGOFF HAS ITS OWN GAP   d_ringoff < 0.7 us but gap >= 1.0 us: without the ring the
                         idle cost is gone, and ringoff is behind CycloneDDS for another reason (its two calls, epoll_pwait2
                         then recvfrom). Known before this run: on 2026-10-22 (rx_uring_wait_R_20261022-144419) ringoff's
                         none-arm p50 read 17.9-18.9 us against CycloneDDS's 12.6-13.1, so this branch is the likely
                         one if the ring is the cause; the gap rule is kept as the report wrote it.
  RING HOLDS PART        0.7 <= d_ringoff < 1.5 us - printed with the numbers.
Printed beside it, not deciding: d_libc - d_ringoff paired with its CI, and every arm's gap to CycloneDDS.

Usage: ringoff_read.py <rmw_crosshost_rtt OUT file> | --selftest | --reps-needed
Last line: "RINGOFF_VERDICT <verdict> ..." (always printed).
"""
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import idle_wake_read as W  # noqa: E402

S, R = W.S, W.R
LIBC, OFF, CYC = "rmw_tickle@libc", "rmw_tickle@libc_ringoff", "rmw_cyclonedds_cpp"
RMWS = (LIBC, OFF, CYC)
ARMS = ("none", "50")
N_REPS, MIN_PAIRED = W.N_REPS, W.MIN_PAIRED
CYC_NONE_US, CYC_NONE_TOL, CONTROL_TOL_US = W.CYC_NONE_US, W.CYC_NONE_TOL, W.CONTROL_TOL_US
REPRO_US, REPRO_LO = 1.5, 0.5
RING_D, RING_GAP = 0.7, 1.0
NOT_RING_D, NOT_RING_LO = 1.5, 0.5


def samples(out, stem, off):
    """pong_residual.samples() generalised: the receive is a recv* carrying the key (ringoff, CycloneDDS) or else the
    ring's io_uring_enter; "asleep" is judged at the call that WAITED - for ringoff the epoll_pwait2 that returned
    between the tap and its recvfrom - and the record handed on carries that wait's entry, so classify() looks at what
    woke the thread before it."""
    pc = out.with_suffix(out.suffix + ".pcaps")
    sst = out.with_suffix(out.suffix + ".sysstamp") / f"{stem}_pong"
    if not sst.is_dir():
        return None, []
    sent, cli = S.ping_keys(pc / f"{stem}_ping.pcap", off)
    arr = S.first_by_key(pc / f"{stem}_pong.pcap", sent, cli)
    recs = S.sysstamp_records(sst)
    res = []
    for k, tap in sorted(arr.items(), key=lambda kv: kv[1]):
        pr = S.first_call(recs, k.hex(), "recv", tap)
        if pr is None and stem.startswith("rmw_tickle"):
            pss = S.first_call(recs, k.hex(), "send", tap)
            pr = S.ring_receive(recs, tap, pss[2], pss[0]) if pss else None
        if not pr:
            continue
        w = S.wake_before(recs, pr, tap)
        entry = w[2] if w else pr[2]
        if entry >= tap:
            continue
        res.append((tap, (pr[3] - tap) / 1000, (pr[0], pr[1], entry, pr[3], pr[4], pr[5])))
    return recs, res


def decide(p):
    """p: {rmw: {arm: {rep: p50}}}. Returns (verdict, lines)."""
    lines = []
    d = {r: W.paired(p[r], "none", "50") for r in RMWS}
    for r in RMWS:
        if len(d[r]) < MIN_PAIRED:
            return f"VOID({r}: {len(d[r])} paired none/50 reps < {MIN_PAIRED})", lines
    gap = W.paired({"o": p[OFF]["none"], "c": p[CYC]["none"]}, "o", "c")
    if len(gap) < MIN_PAIRED:
        return f"VOID(gap: {len(gap)} paired reps < {MIN_PAIRED})", lines
    ci = {r: W.mean_ci(d[r]) for r in RMWS}
    mg = statistics.fmean(gap)
    cnone = statistics.median(p[CYC]["none"].values())
    for r in RMWS:
        m, lo, hi = ci[r]
        lines.append(f"d50 {r:24s} {m:+.2f} us [{lo:+.2f}, {hi:+.2f}] n={len(d[r])}")
    lines.append(f"gap ringoff - cyclone (none) {mg:+.2f} us n={len(gap)}; cyclone none median {cnone:.2f}")
    if abs(cnone - CYC_NONE_US) > CYC_NONE_TOL:
        return f"VOID(control none arm {cnone:.2f} us, not {CYC_NONE_US} +- {CYC_NONE_TOL})", lines
    if abs(ci[CYC][0]) >= CONTROL_TOL_US:
        return f"VOID(control moved with the injection: d50 cyclone {ci[CYC][0]:+.2f} us)", lines
    if ci[LIBC][0] < REPRO_US or ci[LIBC][1] <= REPRO_LO:
        return f"VOID(effect not reproduced: d50 libc {ci[LIBC][0]:+.2f} us, CI low {ci[LIBC][1]:+.2f})", lines
    mo, lo, _ = ci[OFF]
    if mo < RING_D and mg < RING_GAP:
        return "RING IS THE CAUSE", lines
    if mo >= NOT_RING_D and lo > NOT_RING_LO:
        return "NOT THE RING (falsified)", lines
    if mo < RING_D:
        return f"RING CARRIES THE IDLE COST, RINGOFF HAS ITS OWN GAP (gap {mg:+.2f} us)", lines
    return f"RING HOLDS PART (d50 ringoff {mo:+.2f} us, gap {mg:+.2f} us)", lines


def selftest():
    def arms(none, l50, n=13, jit=0.3):
        return {"none": {r: none + jit * ((r * 7) % 5 - 2) / 2 for r in range(n)},
                "50": {r: l50 + jit * ((r * 3) % 5 - 2) / 2 for r in range(n)}}
    cyc = arms(12.9, 12.6)
    cases = [
        ({LIBC: arms(15.5, 13.2), OFF: arms(13.4, 13.2), CYC: cyc}, "RING IS THE CAUSE"),
        ({LIBC: arms(15.5, 13.2), OFF: arms(15.6, 13.4), CYC: cyc}, "NOT THE RING"),
        ({LIBC: arms(15.5, 13.2), OFF: arms(14.3, 13.2), CYC: cyc}, "RING HOLDS PART"),
        ({LIBC: arms(15.5, 13.2), OFF: arms(14.4, 14.2), CYC: cyc}, "RING CARRIES THE IDLE COST, RINGOFF HAS"),
        ({LIBC: arms(14.0, 13.2), OFF: arms(13.4, 13.2), CYC: cyc}, "VOID(effect not reproduced"),
        ({LIBC: arms(15.5, 13.2), OFF: arms(13.4, 13.2), CYC: arms(12.9, 12.0)}, "VOID(control moved"),
        ({LIBC: arms(15.5, 13.2), OFF: arms(13.4, 13.2), CYC: arms(14.5, 14.4)}, "VOID(control none"),
        ({LIBC: arms(15.5, 13.2), OFF: arms(13.4, 13.2, n=8), CYC: cyc}, "VOID(rmw_tickle@libc_ringoff: 8"),
    ]
    bad = 0
    for p, want in cases:
        got, _ = decide(p)
        ok = got.startswith(want)
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} want {want:34s} got {got}")
    # samples(): a ringoff receive is asleep when its epoll_pwait2 was entered before the tap
    recs = [("7", "epoll_pwait2", 100, 1_000_010, 1, ""), ("7", "recvfrom", 1_000_012, 1_000_020, 100, "ab")]
    w = S.wake_before(recs, recs[1], 1_000_000)
    ok = w is not None and w[2] == 100
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} ringoff wait found before its recvfrom: {w}")
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
    print(f"=== C12 ringoff reading of {out.name}: {len(reps)} reps, N_REPS {N_REPS}, MIN_PAIRED {MIN_PAIRED}")
    W.R.samples = samples  # row_reading() keeps its checks; only the receive matching above is generalised
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
                print(f"  {stem:64s} {line}")
                if v is not None:
                    p[rmw][arm][rep] = v
    print("--- arms (rep p50s; us)")
    for rmw in RMWS:
        for arm in ARMS:
            xs = sorted(p[rmw][arm].values())
            if xs:
                print(f"  {rmw:24s} {arm:>4s}: n_reps={len(xs):2d} median {statistics.median(xs):6.2f} "
                      f"mean {statistics.fmean(xs):6.2f} range {xs[0]:.2f}-{xs[-1]:.2f} (spread {xs[-1] - xs[0]:.2f})")
    reps_both = [r for r in reps if all(r in p[x][a] for x in (LIBC, OFF) for a in ARMS)]
    if len(reps_both) >= 2:
        diff = [(p[LIBC]["none"][r] - p[LIBC]["50"][r]) - (p[OFF]["none"][r] - p[OFF]["50"][r]) for r in reps_both]
        m, lo, hi = W.mean_ci(diff)
        print(f"  (not deciding) d50 libc - d50 ringoff {m:+.2f} us [{lo:+.2f}, {hi:+.2f}] n={len(diff)}")
    for rmw in (LIBC, OFF):
        for arm in ARMS:
            g = W.paired({"t": p[rmw][arm], "c": p[CYC][arm]}, "t", "c")
            if g:
                print(f"  (not deciding) {rmw} - CycloneDDS, arm {arm:>4s}: {statistics.fmean(g):+.2f} us n={len(g)}")
    verdict, lines = decide(p)
    for line in lines:
        print("  " + line)
    print(f"RINGOFF_VERDICT {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
