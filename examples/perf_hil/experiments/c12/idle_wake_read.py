#!/usr/bin/env python3
"""C12 idle wake: the pre-registered reader of idle_wake_rig.sh (written 2026-10-31, before any of its runs).

HYPOTHESIS (claude_reports/C12_PONG_RESIDUAL_2026-10-30.md, side finding): rmw_tickle's receive after ~100 ms of idle
(io_uring completion and task_work, a cold ring or cache, a CPU idle state) holds much of the documented +2.0 us pong
gap to CycloneDDS (docs/RESULTS.md). There, a ping 26-47 ms after the pong thread read the peer's announcement was
received in 13.26 us p50 (tap -> return of the receive), one after ~100 ms of idle in 15.50 us; CycloneDDS, also idle
~100 ms, in 12.9 us.

TREATMENT (idle_wake_injector.py): an empty UDP datagram from the ping host to the pong's data socket LEAD ms before
each ping - arms none, 1 and 50 - for rmw_tickle@libc and rmw_cyclonedds_cpp alike; CycloneDDS is the control.

WHAT IS MEASURED, per ping (rmw_pcap_split.py / pong_residual.py): v = pong tap -> return of the call that received it,
for pings the receiving thread was already asleep for. What last woke that thread before the receiving wait:
  inj   a call on the receiving thread that returned within 5 ms after an injected datagram's pong tap
  ping  a return within 5 ms after the previous ping's receive (nothing else woke it in between)
  other anything else (the peer's announcement, the node's own tick)
Samples per arm, fixed here: none -> kind ping with tap - last wake in 95..105 ms (a plain ~100 ms idle);
lead L -> kind inj with tap - injected tap in WINDOW_MS[L].

ROW VALIDITY (each is printed per row, so an arm reports its treatment):
  - the harness row is "ok" (identity, loss, INJECT_END and its sent count are checked there);
  - none: no injected datagram at the pong tap at all;
  - lead: >= MIN_INJ_TAPS injected datagrams at the pong tap; >= MIN_WOKE_FRAC of them followed within 5 ms by a return
    on the thread that receives the pings (the wake was SEEN, not assumed); >= MIN_APPLIED_FRAC of the asleep pings are
    kind inj in the window;
  - >= MIN_ROW_N samples used.
SAMPLE SIZE: N_REPS from the largest A/A rep-to-rep p50 difference seen for rmw_tickle's pong (1.24 us, taken as the
SD of a paired none-minus-lead difference - conservative), to see DELTA_US = 1.0 at alpha 0.05 two-sided, power 0.8:
n = ((1.96 + 0.84) * 1.24 / 1.0)^2 -> 13 reps. Fewer than MIN_PAIRED paired valid reps in a cell -> VOID.

DECISION (d = per rep p50(none) - p50(lead 50), paired by rep; mean and t 95% CI):
  VOID         a cell has < MIN_PAIRED paired reps; or the control's none arm (median rep p50) is outside
               CYC_NONE_US +- CYC_NONE_TOL (the rig is not the one the observation came from); or the control moved
               with the injection, |d_cyclone| >= CONTROL_TOL_US (the effect would not be TickLE's path).
  SUPPORTED    d_tickle >= 1.5 us and its CI low end > 0.5 us.
  PARTIAL      0.5 <= d_tickle < 1.5 us and its CI low end > 0.
  FALSIFIED    d_tickle < 0.5 us and its CI high end < 1.5 us: an injected wake does not buy the ~2 us.
  INCONCLUSIVE anything else.
The 1 ms arm is read the same way and printed beside it (dose), and so is how much of the TickLE - CycloneDDS gap the
50 ms arm closes; neither changes the verdict.

Usage: idle_wake_read.py <rmw_crosshost_rtt OUT file> | --selftest | --reps-needed
Last line: "IDLE_WAKE_VERDICT <verdict> ..." (always printed, also when VOID).
"""
import math
import re
import statistics
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import pong_residual as R  # noqa: E402
import rmw_pcap_split as S  # noqa: E402

TICKLE, CYC = "rmw_tickle@libc", "rmw_cyclonedds_cpp"
ARMS = ("none", "1", "50")
WINDOW_MS = {"1": (0.5, 2.0), "50": (40.0, 60.0)}
IDLE_MS = (95.0, 105.0)
WAKE_AFTER_NS = 5_000_000
INJ_SPORT = 47999
MIN_INJ_TAPS, MIN_WOKE_FRAC, MIN_APPLIED_FRAC, MIN_ROW_N = 50, 0.9, 0.7, 50
SD_DIFF_US, DELTA_US = 1.24, 1.0
N_REPS = math.ceil(((1.96 + 0.84) * SD_DIFF_US / DELTA_US) ** 2)
MIN_PAIRED = 10
CYC_NONE_US, CYC_NONE_TOL = 12.9, 1.0
CONTROL_TOL_US = 0.5
T975 = {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31, 9: 2.26, 10: 2.23, 11: 2.20,
        12: 2.18, 13: 2.16, 14: 2.14, 15: 2.13}


def inj_taps(pcap, client):
    """Pong-tap times of the injected datagrams: UDP from the client, source port INJ_SPORT, empty."""
    out = []
    with open(pcap, "rb") as f:
        head = f.read(24)
        scale = 1 if struct.unpack("<I", head[:4])[0] == 0xA1B23C4D else 1000
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                break
            sec, frac, incl, _ = struct.unpack("<IIII", rec)
            buf = f.read(incl)
            if len(buf) < 42 or struct.unpack(">H", buf[12:14])[0] != 0x0800 or buf[23] != 17:
                continue
            ip = buf[14:]
            ihl = (ip[0] & 0x0F) * 4
            sport, _, ulen = struct.unpack(">HHH", ip[ihl:ihl + 6])
            if ip[12:16] == client and sport == INJ_SPORT and ulen == 8:
                out.append(sec * 10**9 + frac * scale)
    return out


def classify(recs, res, junk):
    """Per sample (tap, v_us, kind, gap_ms to the wake that counts for its kind)."""
    out = []
    by_tid = {}
    for r in recs:
        by_tid.setdefault(r[0], []).append(r)
    for i, (tap, v, pr) in enumerate(res):
        prior = [r for r in by_tid.get(pr[0], []) if r[3] <= pr[2]]
        last = prior[-1] if prior else None
        j = max((t for t in junk if t < tap), default=None)
        kind, gap = "other", float("nan")
        if last is not None:
            pp = res[i - 1][2] if i else None
            if j is not None and j <= last[3] <= j + WAKE_AFTER_NS:
                kind, gap = "inj", (tap - j) / 1e6
            elif pp and pp[0] == pr[0] and 0 <= last[3] - pp[3] <= WAKE_AFTER_NS:
                kind, gap = "ping", (tap - last[3]) / 1e6
        out.append((tap, v, kind, gap))
    return out


def woke(recs, junk, tid):
    """How many injected datagrams were followed within 5 ms by a return on thread tid."""
    t_outs = sorted(r[3] for r in recs if r[0] == tid)
    n = 0
    for j in junk:
        n += any(j <= t <= j + WAKE_AFTER_NS for t in t_outs)
    return n


def row_reading(out, stem, off, arm):
    """(p50 or None, line): one row's sample set, its treatment check and validity."""
    recs, res = R.samples(out, stem, off)
    if not res:
        return None, "NO SAMPLE"
    pc = out.with_suffix(out.suffix + ".pcaps")
    _, client = S.ping_keys(pc / f"{stem}_ping.pcap", off)
    junk = inj_taps(pc / f"{stem}_pong.pcap", client) if client else []
    rows = classify(recs, res, junk)
    tid = statistics.mode([pr[0] for _, _, pr in res])
    nw = woke(recs, junk, tid)
    if arm == "none":
        used = [r[1] for r in rows if r[2] == "ping" and IDLE_MS[0] <= r[3] <= IDLE_MS[1]]
        why = f"injected datagrams at pong tap {len(junk)} (must be 0)" if junk else ""
        treat = f"treatment none: injected at pong tap {len(junk)}"
    else:
        lo, hi = WINDOW_MS[arm]
        used = [r[1] for r in rows if r[2] == "inj" and lo <= r[3] <= hi]
        frac = len(used) / len(rows)
        why = ""
        if len(junk) < MIN_INJ_TAPS:
            why = f"only {len(junk)} injected datagrams at the pong tap"
        elif nw < MIN_WOKE_FRAC * len(junk):
            why = f"only {nw} of {len(junk)} injected datagrams woke the receiving thread"
        elif frac < MIN_APPLIED_FRAC:
            why = f"only {len(used)} of {len(rows)} pings had the injected wake {lo}-{hi} ms before"
        treat = (f"treatment {arm} ms: injected at pong tap {len(junk)}, woke receiving thread {nw}, "
                 f"pings with it as last wake in {lo}-{hi} ms {len(used)}/{len(rows)}")
    if not why and len(used) < MIN_ROW_N:
        why = f"only {len(used)} samples used"
    kinds = " ".join(f"{k}={sum(r[2] == k for r in rows)}" for k in ("inj", "ping", "other"))
    p = R.p50(used) if used else float("nan")
    line = f"n_asleep={len(rows)} used={len(used)} p50 {p:6.2f} | {treat} | kinds {kinds}"
    if why:
        return None, f"VOID({why}) {line}"
    return p, f"ok {line}"


def mean_ci(ds):
    m = statistics.fmean(ds)
    if len(ds) < 2:
        return m, float("-inf"), float("inf")
    h = T975.get(len(ds) - 1, 2.0) * statistics.stdev(ds) / math.sqrt(len(ds))
    return m, m - h, m + h


def paired(p50s, a, b):
    reps = sorted(set(p50s[a]) & set(p50s[b]))
    return [p50s[a][r] - p50s[b][r] for r in reps]


def decide(t, c):
    """t, c: {arm: {rep: p50}} for rmw_tickle and CycloneDDS. Returns (verdict, lines)."""
    lines = []
    dt, dc = paired(t, "none", "50"), paired(c, "none", "50")
    for name, d in (("tickle", dt), ("cyclone", dc)):
        if len(d) < MIN_PAIRED:
            return f"VOID({name}: {len(d)} paired none/50 reps < {MIN_PAIRED})", lines
    cnone = statistics.median(c["none"].values())
    mt, lt, ht = mean_ci(dt)
    mc, lc, hc = mean_ci(dc)
    lines.append(f"d50 tickle {mt:+.2f} us [{lt:+.2f}, {ht:+.2f}] n={len(dt)}; "
                 f"d50 cyclone {mc:+.2f} us [{lc:+.2f}, {hc:+.2f}] n={len(dc)}; cyclone none median {cnone:.2f}")
    if abs(cnone - CYC_NONE_US) > CYC_NONE_TOL:
        return f"VOID(control none arm {cnone:.2f} us, not {CYC_NONE_US} +- {CYC_NONE_TOL})", lines
    if abs(mc) >= CONTROL_TOL_US:
        return f"VOID(control moved with the injection: d50 cyclone {mc:+.2f} us)", lines
    if mt >= 1.5 and lt > 0.5:
        return "SUPPORTED", lines
    if 0.5 <= mt < 1.5 and lt > 0:
        return "PARTIAL", lines
    if mt < 0.5 and ht < 1.5:
        return "FALSIFIED", lines
    return "INCONCLUSIVE", lines


def selftest():
    def arms(none, l50, n=13, jitter=0.3):
        return {"none": {r: none + jitter * ((r * 7) % 5 - 2) / 2 for r in range(n)},
                "1": {r: l50 for r in range(n)},
                "50": {r: l50 + jitter * ((r * 3) % 5 - 2) / 2 for r in range(n)}}
    cases = [
        (arms(15.5, 13.3), arms(12.9, 12.9), "SUPPORTED"),
        (arms(15.5, 14.5), arms(12.9, 12.8), "PARTIAL"),
        (arms(15.5, 15.4), arms(12.9, 12.9), "FALSIFIED"),
        (arms(15.5, 13.3), arms(12.9, 12.0), "VOID(control moved"),
        (arms(15.5, 13.3), arms(14.5, 14.5), "VOID(control none"),
        (arms(15.5, 13.3, n=9), arms(12.9, 12.9), "VOID(tickle: 9"),
        (arms(15.5, 14.6, jitter=6.0), arms(12.9, 12.9), "INCONCLUSIVE"),
    ]
    bad = 0
    for t, c, want in cases:
        got, _ = decide(t, c)
        ok = got.startswith(want)
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} want {want:22s} got {got}")
    # classify: a junk tap 50 ms before a ping whose thread returned 20 us after it -> inj at 50 ms
    recs = [("7", "io_uring_enter", 0, 1_000_000, 0, ""), ("7", "io_uring_enter", 1_000_100, 50_020_000, 0, ""),
            ("7", "io_uring_enter", 50_030_000, 100_015_000, 0, "")]
    res = [(100_000_000, 15.0, recs[2])]
    k = classify(recs, res, [50_000_000])
    ok = k[0][2] == "inj" and abs(k[0][3] - 50.0) < 1e-6
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} classify inj: {k}")
    k = classify(recs, res, [])
    ok = k[0][2] == "other"
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} classify no junk, no previous ping -> other: {k}")
    ok = woke(recs, [50_000_000, 70_000_000], "7") == 1
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} woke counts only a return within 5 ms")
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
    print(f"=== C12 idle wake reading of {out.name}: {len(reps)} reps, N_REPS pre-registered {N_REPS}, "
          f"MIN_PAIRED {MIN_PAIRED}")
    p50s = {TICKLE: {a: {} for a in ARMS}, CYC: {a: {} for a in ARMS}}
    for rmw in (TICKLE, CYC):
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
                p, line = row_reading(out, stem, offs[stem], arm)
                print(f"  {stem:58s} {line}")
                if p is not None:
                    p50s[rmw][arm][rep] = p
    print("--- arms (rep p50s of the samples used; us)")
    for rmw in (TICKLE, CYC):
        for arm in ARMS:
            xs = sorted(p50s[rmw][arm].values())
            if xs:
                print(f"  {rmw:20s} {arm:>4s}: n_reps={len(xs):2d} median {statistics.median(xs):6.2f} "
                      f"mean {statistics.fmean(xs):6.2f} range {xs[0]:.2f}-{xs[-1]:.2f} "
                      f"(spread {xs[-1] - xs[0]:.2f})")
            else:
                print(f"  {rmw:20s} {arm:>4s}: no valid rep")
    for rmw in (TICKLE, CYC):
        d1 = paired(p50s[rmw], "none", "1")
        if len(d1) >= 2:
            m, lo, hi = mean_ci(d1)
            print(f"  dose (not deciding): {rmw} none - 1 ms {m:+.2f} us [{lo:+.2f}, {hi:+.2f}] n={len(d1)}")
    for arm in ARMS:
        g = paired({"t": p50s[TICKLE][arm], "c": p50s[CYC][arm]}, "t", "c")
        if g:
            print(f"  TickLE - CycloneDDS in arm {arm:>4s} (not deciding): {statistics.fmean(g):+.2f} us n={len(g)}")
    verdict, lines = decide(p50s[TICKLE], p50s[CYC])
    for line in lines:
        print("  " + line)
    print(f"IDLE_WAKE_VERDICT {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
