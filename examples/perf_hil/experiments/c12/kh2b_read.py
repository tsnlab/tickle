#!/usr/bin/env python3
"""KH2 warm, step B: two cheap production-shaped candidates for TickLE's warm receive residual, A/B on the rig. The
pre-registered reader of kh2b_rig.sh, written 2026-11-01 before any of its runs.

WHY THESE TWO: in C12 step 6's warm (50 ms) arm, TickLE's receiving thread spent ~1200 more in-window cycles per ping
than CycloneDDS's in io_uring code (CycloneDDS: none), fput among them (~400 cycles): the wait ring's recvmsg takes a
file reference when issued and drops it when it completes. Candidates (both in the wait ring's wr_arm(), hal_linux.c):
  (ff) ab/ring-fixed-file:  the data socket registered with the ring (IORING_REGISTER_FILES) and the recvmsg issued
                            IOSQE_FIXED_FILE - no fget/fput per receive. Switch TT_RX_RING_FIXED_FILE.
  (pf) ab/ring-poll-first:  the recvmsg issued IORING_RECVSEND_POLL_FIRST - armed on the poll at once, without the
                            speculative read that fails on an empty socket. Switch TT_RX_RING_POLL_FIRST.
ARMS (one patched tree, c12/kh2b.patch = both changes behind their switches, applied on the Pis by PI_PATCH, so every arm
is the same source; the libc build so sysstamp sees the ring):
  (base) rmw_tickle@libc_ringbase  -DTT_RX_RING_FIXED_FILE=0 -DTT_RX_RING_POLL_FIRST=0  (today's main behaviour)
  (ff)   rmw_tickle@libc_ringff    -DTT_RX_RING_POLL_FIRST=0
  (pf)   rmw_tickle@libc_ringpf    -DTT_RX_RING_FIXED_FILE=0
  Injection none (idle) and 50 ms (warm); unpinned; 13 reps; STAMPS=1 (the pong's callback time).

MEASURED per row (rep): p50 of tap -> receive return (warm/idle, idle_wake_read.py's sample sets, rxthread_read.py's
receive matching); p50 of tap -> the pong's callback (its --stamps, the same pings); the ping's RTT p50 (RESULT); the
pong's CPU per ping (pong_cpu_ns / sent).
READBACK per row, any failure VOIDs the run: the pong log's HAL ring line reads ring=1 and ring_fixed / ring_poll_first
equal to the arm's switches (ff: 1/0, pf: 0/1, base: 0/0); the harness row ok and row_reading() valid.

PASS for a candidate x, all of (differences x - base paired by rep, mean, t 95% CI):
  warm receive improves >= 0.3 us: mean(base - x) at 50 ms >= 0.3, and the two arms' rep-p50 CIs do not overlap;
  RTT p50 not worse: mean(x - base) <= +0.3 us in each injection arm;
  pong CPU per ping not worse: x / base - 1 <= +2% (mean over all its rows);
  idle not worse: tap -> receive mean(x - base) at none <= +0.3 us;
  callback not worse: tap -> callback mean(x - base) <= +0.3 us in each injection arm.
Otherwise FAIL, with each failed criterion named. VOID: a readback failure, or < 10 paired reps in a cell.

Usage: kh2b_read.py <OUT file> | --selftest | --reps-needed     Last line: "KH2B_VERDICT ff=<...> pf=<...>" (always).
"""
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import idle_wake_read as W  # noqa: E402
import rxthread_read as X  # noqa: E402

R, S = W.R, W.S
BASE, FF, PF = "rmw_tickle@libc_ringbase", "rmw_tickle@libc_ringff", "rmw_tickle@libc_ringpf"
RMWS = (BASE, FF, PF)
EXPECT = {BASE: (0, 0), FF: (1, 0), PF: (0, 1)}
ARMS = ("none", "50")
N_REPS, MIN_PAIRED = W.N_REPS, W.MIN_PAIRED
GAIN, WORSE_US, WORSE_CPU = 0.3, 0.3, 0.02


def readback(log_text, rmw):
    m = re.search(r" ring=(-?\d+) .*? ring_fixed=(\d) ring_poll_first=(\d)", log_text)
    if not m:
        return False, "VOID(readback: no ring line)"
    ring, fx, pf = int(m.group(1)), int(m.group(2)), int(m.group(3))
    line = f"ring={ring} ring_fixed={fx} ring_poll_first={pf}"
    if ring != 1 or (fx, pf) != EXPECT[rmw]:
        return False, f"VOID(readback: wanted ring=1 {EXPECT[rmw]}) " + line
    return True, line


def callback_p50(out, stem, off, used_keys_taps):
    """p50 of tap -> pong callback (us) for the given (key, tap) pairs, from the pong's --stamps."""
    sf = out.with_suffix(out.suffix + ".stamps") / f"{stem}_pong.txt"
    pf = out.with_suffix(out.suffix + ".stamps") / f"{stem}_ping.txt"
    if not sf.exists() or not pf.exists():
        return None
    pong_rows, pong_off = S.load_stamps(sf)
    ping_rows, _ = S.load_stamps(pf)
    if pong_off is None:
        return None
    import struct
    seq_of = {struct.pack("<Q", send): seq for seq, (send, _) in ping_rows.items()}
    xs = []
    for k, tap in used_keys_taps:
        seq = seq_of.get(k)
        if seq is not None and seq in pong_rows:
            xs.append((pong_rows[seq][0] + pong_off - tap) / 1000)
    xs.sort()
    return xs[len(xs) // 2] if xs else None


def used_taps(out, stem, off, arm):
    """[(key, tap)] of the pings the arm's sample set uses."""
    recs, res = X.samples(out, stem, off)
    if not res:
        return []
    pc = out.with_suffix(out.suffix + ".pcaps")
    sent, client = S.ping_keys(pc / f"{stem}_ping.pcap", off)
    arr = S.first_by_key(pc / f"{stem}_pong.pcap", sent, client)
    tap_key = {t: k for k, t in arr.items()}
    junk = W.inj_taps(pc / f"{stem}_pong.pcap", client) if client else []
    rows = W.classify(recs, res, junk)
    out_ = []
    for tap, _v, kind, gap in rows:
        if (arm == "none" and kind == "ping" and W.IDLE_MS[0] <= gap <= W.IDLE_MS[1]) or \
           (arm != "none" and kind == "inj" and W.WINDOW_MS[arm][0] <= gap <= W.WINDOW_MS[arm][1]):
            if tap in tap_key:
                out_.append((tap_key[tap], tap))
    return out_


def judge(m, x):
    """m: {metric: {rmw: {arm: {rep: value}}}}; returns (verdict, lines) for candidate x against BASE."""
    lines, fails = [], []
    rx = m["recv"]
    for rmw in (BASE, x):
        for a in ARMS:
            if len(rx[rmw][a]) < MIN_PAIRED:
                return f"VOID({rmw} {a}: {len(rx[rmw][a])} reps < {MIN_PAIRED})", lines
    gain = W.paired({"b": rx[BASE]["50"], "x": rx[x]["50"]}, "b", "x")
    mg, glo, ghi = W.mean_ci(gain)
    _, blo, _ = W.mean_ci(list(rx[BASE]["50"].values()))
    _, _, xhi = W.mean_ci(list(rx[x]["50"].values()))
    lines.append(f"warm receive base - {x}: {mg:+.2f} us [{glo:+.2f}, {ghi:+.2f}]; CI hi(x) {xhi:.2f} vs CI lo(base) {blo:.2f}")
    if not (mg >= GAIN and xhi < blo):
        fails.append("warm receive gain")
    idle = W.paired({"x": rx[x]["none"], "b": rx[BASE]["none"]}, "x", "b")
    mi = statistics.fmean(idle) if idle else float("nan")
    lines.append(f"idle receive {x} - base: {mi:+.2f} us")
    if not mi <= WORSE_US:
        fails.append("idle receive worse")
    for metric, name in (("rtt", "RTT p50"), ("cb", "tap->callback")):
        for a in ARMS:
            d = W.paired({"x": m[metric][x][a], "b": m[metric][BASE][a]}, "x", "b")
            md = statistics.fmean(d) if d else float("nan")
            lines.append(f"{name} {a} {x} - base: {md:+.2f} us n={len(d)}")
            if not (d and md <= WORSE_US):
                fails.append(f"{name} {a} worse" if d else f"{name} {a} missing")
    cb, cx = [v for a in ARMS for v in m["cpu"][BASE][a].values()], [v for a in ARMS for v in m["cpu"][x][a].values()]
    rel = statistics.fmean(cx) / statistics.fmean(cb) - 1 if cb and cx else float("nan")
    lines.append(f"pong CPU per ping {x} / base - 1: {rel:+.1%}")
    if not rel <= WORSE_CPU:
        fails.append("pong CPU worse")
    return ("PASS" if not fails else "FAIL(" + "; ".join(fails) + ")"), lines


def selftest():
    def arm(v, n=13, jit=0.2):
        return {r: v + jit * ((r * 7) % 5 - 2) / 2 for r in range(n)}
    def mk(base50, x50, xnone=15.2, xrtt=250.0, xcb=20.0, xcpu=1.0):
        m = {k: {r: {} for r in RMWS} for k in ("recv", "rtt", "cb", "cpu")}
        for r in RMWS:
            x = r != BASE
            m["recv"][r] = {"none": arm(xnone if x else 15.2), "50": arm(x50 if x else base50)}
            m["rtt"][r] = {a: arm(xrtt if x else 250.0) for a in ARMS}
            m["cb"][r] = {a: arm(xcb if x else 20.0) for a in ARMS}
            m["cpu"][r] = {a: arm(1000.0 * (xcpu if x else 1.0), jit=0) for a in ARMS}
        return m
    cases = [
        (mk(13.2, 12.7), "PASS"),
        (mk(13.2, 13.0), "FAIL(warm receive gain"),
        (mk(13.2, 12.7, xrtt=251.0), "FAIL(RTT p50 none worse"),
        (mk(13.2, 12.7, xcpu=1.05), "FAIL(pong CPU worse"),
        (mk(13.2, 12.7, xnone=15.7), "FAIL(idle receive worse"),
        (mk(13.2, 12.7, xcb=20.5), "FAIL(tap->callback none worse"),
    ]
    bad = 0
    for m, want in cases:
        got, _ = judge(m, FF)
        ok = got.startswith(want)
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} want {want:30s} got {got}")
    log = "[INFO] Node 2 rx: socket_drops=0 drain=1 ring=1 ring_waits=5 ring_reads=3 ring_cancels=0 ring_others=0 " \
          "ring_moves=0 ring_stale=0 ring_fixed=1 ring_poll_first=0\n"
    ok = readback(log, FF)[0] and not readback(log, PF)[0] and not readback("", BASE)[0]
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} readback: ff line passes ff, fails pf; no line fails")
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
    verdicts, rowtext = {}, {}
    for mm in re.finditer(r"^(\S+) bench best_effort rep(\d+) wait=block sst=on inject=(\S+) \| (\S+) \|(.*)$", text,
                          re.M):
        k = (mm.group(1), int(mm.group(2)), mm.group(3))
        verdicts[k], rowtext[k] = mm.group(4), mm.group(5)
    reps = sorted({k[1] for k in verdicts})
    print(f"=== KH2 step B reading of {out.name}: {len(reps)} reps, N_REPS {N_REPS}")
    W.R.samples = X.samples
    logs = out.with_suffix(out.suffix + ".logs")
    m = {k: {r: {a: {} for a in ARMS} for r in RMWS} for k in ("recv", "rtt", "cb", "cpu")}
    fails = 0
    for rmw in RMWS:
        for arm in ARMS:
            for rep in reps:
                stem = f"{rmw}_bench_best_effort_rep{rep}_block_sst_inj{arm}"
                k = (rmw, rep, arm)
                if k not in verdicts or stem not in offs:
                    print(f"  {stem}: MISSING")
                    continue
                if not verdicts[k].startswith("ok"):
                    print(f"  {stem}: harness {verdicts[k]}")
                    fails += 1
                    continue
                v, line = W.row_reading(out, stem, offs[stem], arm)
                lf = logs / f"{stem}_pong.log"
                ok, rb = readback(lf.read_text(errors="replace") if lf.exists() else "", rmw)
                cb = callback_p50(out, stem, offs[stem], used_taps(out, stem, offs[stem], arm))
                mr = re.search(r"rtt_p50_ms=([0-9.]+)", rowtext[k])
                mc = re.search(r"pong_cpu_ns=(\d+)", rowtext[k])
                ms = re.search(r" sent=(\d+)", rowtext[k])
                line += f" | {rb} | tap->callback p50 {cb if cb is None else round(cb, 2)}"
                print(f"  {stem:60s} {line}")
                if not ok or v is None:
                    fails += 1
                    continue
                m["recv"][rmw][arm][rep] = v
                if mr:
                    m["rtt"][rmw][arm][rep] = float(mr.group(1)) * 1000
                if cb is not None:
                    m["cb"][rmw][arm][rep] = cb
                if mc and ms and int(ms.group(1)) > 0:
                    m["cpu"][rmw][arm][rep] = int(mc.group(1)) / int(ms.group(1))
    print("--- arms (rep medians; us)")
    for rmw in RMWS:
        for arm in ARMS:
            def md(metric):
                xs = list(m[metric][rmw][arm].values())
                return statistics.median(xs) if xs else float("nan")
            print(f"  {rmw:26s} {arm:>4s}: n={len(m['recv'][rmw][arm])} receive {md('recv'):.2f} callback {md('cb'):.2f} "
                  f"RTT {md('rtt'):.1f} CPU/ping {md('cpu'):.0f} ns")
    res = {}
    for x in (FF, PF):
        if fails:
            res[x] = f"VOID({fails} rows failed their readback)"
            continue
        res[x], lines = judge(m, x)
        for line in lines:
            print("  " + line)
    print(f"KH2B_VERDICT ff={res[FF]} pf={res[PF]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
