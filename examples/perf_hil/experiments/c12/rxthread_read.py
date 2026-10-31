#!/usr/bin/env python3
"""C12 experiment (a): does a receive-only thread take TickLE's idle cost away? The pre-registered reader of
rxthread_rig.sh, written 2026-10-31 before any of its runs.

THE SWITCH: rmw_tickle already has the thread the experiment asks for. Its poll thread (rmw_node.c, one per context)
waits in tt_Context_poll() and does every receive, delivering into the subscriber/service/client queues and waking
rmw_wait() on wait_cond - CycloneDDS's recvUC + executor shape. Since 2026-09-27 the default is executor-driven receive
(RMW_TICKLE_EXECUTOR_POLL unset or 1): rmw_wait() takes the poll role and receives on the executor thread itself, which
is the thread every step 0-6 measured. RMW_TICKLE_EXECUTOR_POLL=0 is the existing, tested switch back to the poll thread
receiving (rmw_init.c); the core is thread-safe since 2026-09-25 (tickle.h "Threading"), so no new production code is
needed and none is written.

ARMS (unpinned; both TickLE arms are the libc build of origin/main 224911cf, built twice under two names):
  (r) rmw_tickle@libc             switch off: the executor receives (as shipped)
  (t) rmw_tickle@libc_rxthread    RMW_TICKLE_EXECUTOR_POLL=0: the poll thread receives, the executor only takes/replies
  CycloneDDS                      the control
  Injection none and 50 ms (idle_wake_injector.py); 13 reps (idle_wake_read.py's formula).

MEASURED: tap -> return of the call that received the ping, for pings the receiving thread was asleep for. The receive
is matched as the io_uring_enter (any thread) that was entered before the tap and returned first after it, before the
reply's send - so it is found whichever thread receives; CycloneDDS by its recvmsg carrying the key, as before. Sample
sets and row validity: idle_wake_read.py's. Printed and NOT deciding: the pong turnaround (pong tap in -> pong tap out,
which includes (t)'s handoff to the executor) and the ping's RTT p50.

READBACK per TickLE row (any failure VOIDs the run):
  (r): the pong log's "rmw_tickle: executor_poll=1", executor_poll_waits >= 50, and the receiving thread is the pong's
       main thread (its tid = the sysstamp file's pid);
  (t): "executor_poll=0", executor_poll_waits = 0, and the receiving thread is NOT the main thread. Each row prints the
       receiving thread and how many ping receives it made.

DECISION (E = d_arm - d_cyclone per rep, d = p50(none) - p50(50); mean, t 95% CI):
  VOID         a readback failure; < 10 paired reps in a cell; CycloneDDS's mean d above +1.0 us.
  IDLE BETTER  E_t <= E_r - 0.6 us and E_t's CI high end < E_r's CI low end.
  NO GAIN      otherwise.
  STREAM WORSE, reported beside it: (t)'s 50 ms p50 more than 0.3 us above (r)'s (mean of the per-rep differences), or
               the pong's CPU per ping (pong_cpu_ns / sent, mean over all its rows) more than 10% above (r)'s.

Usage: rxthread_read.py <OUT file> | --selftest | --reps-needed     Last line: "RXTHREAD_VERDICT ..." (always).
"""
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import idle_wake_read as W  # noqa: E402

R, S = W.R, W.S
REF, RXT, CYC = "rmw_tickle@libc", "rmw_tickle@libc_rxthread", "rmw_cyclonedds_cpp"
RMWS = (REF, RXT, CYC)
ARMS = ("none", "50")
N_REPS, MIN_PAIRED = W.N_REPS, W.MIN_PAIRED
CYC_D_MAX, GAIN_US, STREAM_US, CPU_FRAC = 1.0, 0.6, 0.3, 0.10


def samples(out, stem, off):
    """(recs, [(tap, v_us, receive record)]), the receive found on any thread (see the module comment)."""
    pc = out.with_suffix(out.suffix + ".pcaps")
    sst = out.with_suffix(out.suffix + ".sysstamp") / f"{stem}_pong"
    if not sst.is_dir():
        return None, []
    sent, cli = S.ping_keys(pc / f"{stem}_ping.pcap", off)
    arr = S.first_by_key(pc / f"{stem}_pong.pcap", sent, cli)
    recs = S.sysstamp_records(sst)
    rings = [r for r in recs if r[1] == S.RING and r[4] >= 0]
    res = []
    for k, tap in sorted(arr.items(), key=lambda kv: kv[1]):
        pr = S.first_call(recs, k.hex(), "recv", tap)
        if pr is not None:
            w = S.wake_before(recs, pr, tap)
            entry = w[2] if w else pr[2]
            if entry >= tap:
                continue
            pr = (pr[0], pr[1], entry, pr[3], pr[4], pr[5])
        elif stem.startswith("rmw_tickle"):
            pss = S.first_call(recs, k.hex(), "send", tap)
            if not pss:
                continue
            cands = [r for r in rings if r[2] < tap <= r[3] <= pss[2]]
            if not cands:
                continue
            pr = min(cands, key=lambda r: r[3])
        else:
            continue
        res.append((tap, (pr[3] - tap) / 1000, pr))
    return recs, res


def main_pid(out, stem):
    sst = out.with_suffix(out.suffix + ".sysstamp") / f"{stem}_pong"
    pids = [p.name.rsplit(".", 1)[-1] for p in sst.glob("*") if p.name.rsplit(".", 1)[-1].isdigit()]
    return pids[0] if len(pids) == 1 else None


def readback(rmw, log_text, rtid, pid, n_recv):
    m = re.search(r"rmw_tickle: executor_poll=(\d) executor_poll_waits=(\d+)", log_text)
    ep, waits = (int(m.group(1)), int(m.group(2))) if m else (None, None)
    line = f"executor_poll={ep} waits={waits}, receiving thread {rtid} (main {pid}) received {n_recv} pings"
    why = []
    if m is None or pid is None or rtid is None:
        why.append("readback missing")
    elif rmw == REF and not (ep == 1 and waits >= 50 and rtid == pid):
        why.append("not the executor receiving")
    elif rmw == RXT and not (ep == 0 and waits == 0 and rtid != pid):
        why.append("not the poll thread receiving")
    return (not why), (f"VOID(readback: {'; '.join(why)}) " if why else "") + line


def turnaround(out, stem, off):
    """p50 of pong tap in -> pong tap out (us), and the number of pings it covers."""
    pc = out.with_suffix(out.suffix + ".pcaps")
    sent, cli = S.ping_keys(pc / f"{stem}_ping.pcap", off)
    arr = S.first_by_key(pc / f"{stem}_pong.pcap", sent, cli)
    srv = S.server_ip(pc / f"{stem}_pong.pcap", sent, cli)
    dep = S.first_by_key(pc / f"{stem}_pong.pcap", sent, srv) if srv else {}
    xs = sorted((dep[k] - arr[k]) / 1000 for k in arr if k in dep and dep[k] > arr[k])
    return (xs[len(xs) // 2] if xs else float("nan")), len(xs)


def decide(p, cpu, fails):
    lines = []
    if fails:
        return f"VOID({fails} rows failed their readback)", "", lines
    d = {r: W.paired(p[r], "none", "50") for r in RMWS}
    for r in RMWS:
        if len(d[r]) < MIN_PAIRED:
            return f"VOID({r}: {len(d[r])} paired none/50 reps < {MIN_PAIRED})", "", lines
    e = {}
    for x in (REF, RXT):
        reps = [k for k in p[x]["none"] if k in p[x]["50"] and k in p[CYC]["none"] and k in p[CYC]["50"]]
        e[x] = [(p[x]["none"][k] - p[x]["50"][k]) - (p[CYC]["none"][k] - p[CYC]["50"][k]) for k in reps]
        if len(e[x]) < MIN_PAIRED:
            return f"VOID({x}: {len(e[x])} reps paired with the control < {MIN_PAIRED})", "", lines
    cd = {r: W.mean_ci(d[r]) for r in RMWS}
    ce = {x: W.mean_ci(e[x]) for x in e}
    for r in RMWS:
        lines.append(f"raw d50 {r:26s} {cd[r][0]:+.2f} us [{cd[r][1]:+.2f}, {cd[r][2]:+.2f}] n={len(d[r])}")
    for x in e:
        lines.append(f"E       {x:26s} {ce[x][0]:+.2f} us [{ce[x][1]:+.2f}, {ce[x][2]:+.2f}] n={len(e[x])}")
    s50 = W.paired({"t": p[RXT]["50"], "r": p[REF]["50"]}, "t", "r")
    ms50 = statistics.fmean(s50) if s50 else float("nan")
    cpu_r = statistics.fmean(cpu[REF]) if cpu[REF] else float("nan")
    cpu_t = statistics.fmean(cpu[RXT]) if cpu[RXT] else float("nan")
    lines.append(f"steady path: (t) - (r) 50 ms p50 {ms50:+.2f} us n={len(s50)}; pong CPU per ping (r) {cpu_r:.0f} ns "
                 f"(t) {cpu_t:.0f} ns ({(cpu_t / cpu_r - 1) if cpu_r == cpu_r and cpu_r else float('nan'):+.1%})")
    stream = "STREAM WORSE" if (ms50 > STREAM_US or cpu_t > (1 + CPU_FRAC) * cpu_r) else "STREAM NOT WORSE"
    if cd[CYC][0] > CYC_D_MAX:
        return f"VOID(control d50 {cd[CYC][0]:+.2f} us > {CYC_D_MAX:+.1f})", stream, lines
    if ce[RXT][0] <= ce[REF][0] - GAIN_US and ce[RXT][2] < ce[REF][1]:
        return "IDLE BETTER", stream, lines
    return "NO GAIN", stream, lines


def selftest():
    def arms(none, l50, n=13, jit=0.3):
        return {"none": {r: none + jit * ((r * 7) % 5 - 2) / 2 for r in range(n)},
                "50": {r: l50 + jit * ((r * 3) % 5 - 2) / 2 for r in range(n)}}
    ref, cyc = arms(15.5, 13.2), arms(12.9, 12.4)
    cpu_same = {REF: [1000.0] * 26, RXT: [1050.0] * 26}
    cases = [
        ({REF: ref, RXT: arms(13.5, 13.2), CYC: cyc}, cpu_same, 0, "IDLE BETTER", "STREAM NOT WORSE"),
        ({REF: ref, RXT: arms(15.3, 13.2), CYC: cyc}, cpu_same, 0, "NO GAIN", "STREAM NOT WORSE"),
        ({REF: ref, RXT: arms(14.1, 13.6), CYC: cyc}, cpu_same, 0, "IDLE BETTER", "STREAM WORSE"),   # 50 ms +0.4
        ({REF: ref, RXT: arms(13.5, 13.2), CYC: cyc}, {REF: [1000.0], RXT: [1200.0]}, 0, "IDLE BETTER", "STREAM WORSE"),
        ({REF: ref, RXT: arms(13.5, 13.2), CYC: arms(12.9, 11.7)}, cpu_same, 0, "VOID(control", None),
        ({REF: ref, RXT: arms(13.5, 13.2), CYC: cyc}, cpu_same, 2, "VOID(2 rows", None),
        ({REF: ref, RXT: arms(13.5, 13.2, n=9), CYC: cyc}, cpu_same, 0, "VOID(rmw_tickle@libc_rxthread: 9", None),
    ]
    bad = 0
    for p, cpu, fails, want, wstream in cases:
        got, stream, _ = decide(p, cpu, fails)
        ok = got.startswith(want) and (wstream is None or stream == wstream)
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} want {want} / {wstream} got {got} / {stream}")
    log1 = "rmw_tickle: executor_poll=1 executor_poll_waits=103 executor_handovers=1\n"
    log0 = "rmw_tickle: executor_poll=0 executor_poll_waits=0 executor_handovers=0\n"
    ok = (readback(REF, log1, "7", "7", 80)[0] and not readback(REF, log1, "8", "7", 80)[0]
          and readback(RXT, log0, "8", "7", 80)[0] and not readback(RXT, log0, "7", "7", 80)[0]
          and not readback(RXT, log1, "8", "7", 80)[0] and not readback(REF, "", "7", "7", 80)[0])
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} readback: executor vs poll thread receiving, each refused in the other arm")
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
    for m in re.finditer(r"^(\S+) bench best_effort rep(\d+) wait=block sst=on inject=(\S+) \| (\S+) \|(.*)$", text,
                         re.M):
        key = (m.group(1), int(m.group(2)), m.group(3))
        verdicts[key], rowtext[key] = m.group(4), m.group(5)
    reps = sorted({k[1] for k in verdicts})
    print(f"=== C12 experiment (a) reading of {out.name}: {len(reps)} reps, N_REPS {N_REPS}")
    W.R.samples = samples
    logs = out.with_suffix(out.suffix + ".logs")
    p = {r: {a: {} for a in ARMS} for r in RMWS}
    cpu = {REF: [], RXT: []}
    turn = {(r, a): [] for r in RMWS for a in ARMS}
    rtt = {(r, a): [] for r in RMWS for a in ARMS}
    fails = 0
    for rmw in RMWS:
        for arm in ARMS:
            for rep in reps:
                stem = f"{rmw}_bench_best_effort_rep{rep}_block_sst_inj{arm}"
                key = (rmw, rep, arm)
                hv = verdicts.get(key)
                if hv is None or stem not in offs:
                    print(f"  {stem}: MISSING")
                    continue
                if not hv.startswith("ok"):
                    print(f"  {stem}: harness {hv}")
                    continue
                v, line = W.row_reading(out, stem, offs[stem], arm)
                tn, _ = turnaround(out, stem, offs[stem])
                turn[(rmw, arm)].append(tn)
                mr = re.search(r"rtt_p50_ms=([0-9.]+)", rowtext[key])
                if mr:
                    rtt[(rmw, arm)].append(float(mr.group(1)) * 1000)
                if rmw != CYC:
                    _, res = samples(out, stem, offs[stem])
                    rtid = statistics.mode([pr[0] for _, _, pr in res]) if res else None
                    lf = logs / f"{stem}_pong.log"
                    ok, rb = readback(rmw, lf.read_text(errors="replace") if lf.exists() else "", rtid,
                                      main_pid(out, stem), sum(1 for _, _, pr in res if pr[0] == rtid))
                    line += " | " + rb
                    mc = re.search(r"pong_cpu_ns=(\d+)", rowtext[key])
                    ms = re.search(r" sent=(\d+)", rowtext[key])
                    if mc and ms and int(ms.group(1)) > 0:
                        cpu[rmw].append(int(mc.group(1)) / int(ms.group(1)))
                    if not ok:
                        fails += 1
                        v = None
                print(f"  {stem:62s} {line} | turnaround p50 {tn:.1f} us")
                if v is not None:
                    p[rmw][arm][rep] = v
    print("--- arms (rep p50s of tap -> receive return; us)")
    for rmw in RMWS:
        for arm in ARMS:
            xs = sorted(p[rmw][arm].values())
            if xs:
                tt = [x for x in turn[(rmw, arm)] if x == x]
                print(f"  {rmw:26s} {arm:>4s}: n_reps={len(xs):2d} median {statistics.median(xs):6.2f} "
                      f"mean {statistics.fmean(xs):6.2f} range {xs[0]:.2f}-{xs[-1]:.2f} (spread {xs[-1] - xs[0]:.2f})"
                      f" | (not deciding) turnaround median {statistics.median(tt) if tt else float('nan'):.1f} us, "
                      f"RTT p50 median {statistics.median(rtt[(rmw, arm)]) if rtt[(rmw, arm)] else float('nan'):.1f} us")
    verdict, stream, lines = decide(p, cpu, fails)
    for line in lines:
        print("  " + line)
    print(f"RXTHREAD_VERDICT {verdict}; {stream}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
