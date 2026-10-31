#!/usr/bin/env python3
"""C12 idle wake, step 6: where in the receiving thread's own tap -> return window do TickLE's extra idle cycles go? The
pre-registered reader of step6_rig.sh, written 2026-10-31 before any of its runs.

RUN: unpinned, rmw_tickle@libc_rb (b6f0c635 + c12/step3.patch, the steps 3-5 reference) and rmw_cyclonedds_cpp, injection
none and 50 ms, 13 reps; EVERY row runs c12/step6_prof.bt (bpftrace, root: kernel symbols, which perf cannot resolve for a
non-root user under kptr_restrict 1): a sample of the receiving thread every 10000 cycles, user or kernel, with its
CLOCK_MONOTONIC time and 6 kernel frames. Why 13 reps and 10000 cycles: the thread runs ~5-8 us of each ping's window, so
~0.6-1.0 samples a ping, ~700-1100 in-window samples per rmw and arm over 13 reps x ~85 used pings; TickLE's extra
(~1.5 us, ~2250 cycles) is ~0.22 samples a ping, ~250 samples an arm, against a Poisson SD of ~30 for a group of ~800.

WINDOW: per used ping (idle_wake_read.py's sample sets: none = last wake the previous ping 95-105 ms before; 50 = last wake
the injected datagram 40-60 ms before), from the pong tap to the return of the call that received it; a sample counts
when its time (CLOCK_MONOTONIC + the pong host's REALTIME - MONOTONIC offset, recorded around the row) falls inside.
Only the thread's own on-cpu time is sampled: softirq work on the IRQ cpu and the time before the thread is switched in
are not in it (the step-0 lean split put most of the extra after switch-in).

GROUPS (fixed here): a user-mode sample is "user"; a kernel sample is grouped by its first frame that is not a
lock/irq-restore helper (arm64 takes the PMU interrupt as an IRQ, so samples in irq-off regions land on the unlock)
by GROUP_RULES, first match, else "kernel other". Per (rmw, arm): cycles a ping in group g = in-window samples in g x
10000 / used pings. Delta(g) = none - 50, per rmw. The per-symbol table is printed for drill-down and decides nothing.

STATISTICS: E = d_tickle - d_cyclone per rep (steps 3-5), d = p50(none) - p50(50) of the instrumented rows; the extra to
attribute X = mean E x 1500 cycles/us (the woken cpu sat at 1.5 GHz in ~99% of step-0's bursts).
VOID   CycloneDDS's mean d above +1.0 us (the instruments moved the control); mean E below +0.75 us (no E-sized extra to
       attribute under the instruments); in-window samples below MIN_SAMPLES (400) for any rmw and arm; a row whose
       sampled thread is not sysstamp's receiving thread or whose profile did not attach is VOID itself, and a cell with
       < 10 paired valid reps VOIDs the run.
FOUND    some group g with Delta_tickle(g) >= 0.5 X and |Delta_cyclone(g)| < 0.25 Delta_tickle(g).
DIFFUSE  no group with Delta_tickle(g) >= 0.25 X.
PARTIAL  otherwise.

Usage: step6_read.py <OUT file> | --selftest | --reps-needed     Last line: "STEP6_VERDICT ..." (always).
"""
import collections
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import idle_wake_read as W  # noqa: E402
import ringoff_read as RO  # noqa: E402

R, S = W.R, W.S
TICKLE, CYC = "rmw_tickle@libc_rb", "rmw_cyclonedds_cpp"
ARMS = ("none", "50")
PERIOD = 10000
CYC_PER_US = 1500
MIN_SAMPLES = 400
CYC_D_MAX, E_MIN = 1.0, 0.75
N_REPS, MIN_PAIRED = W.N_REPS, W.MIN_PAIRED
HELPER = re.compile(r"^(_raw_|__raw_|arch_local_irq|local_irq|preempt_|queued_|rcu_read|do_raw_|__local_bh|_local_bh)")
GROUP_RULES = [
    ("io_uring", re.compile(r"^(io_|__io_|io_uring)")),
    ("sched", re.compile(r"(schedule|finish_task_switch|ttwu|try_to_wake_up|enqueue|dequeue|pick_next|put_prev|set_next|"
                         r"update_curr|update_load|update_rq|select_task|wake_up|__switch_to|cpu_switch_to|psi_|sched_|"
                         r"resched|task_tick|reweight|place_entity|check_preempt|wakeup)")),
    ("idle", re.compile(r"(do_idle|cpu_idle|default_idle|arch_cpu_idle|cpuidle|psci|cpu_startup_entry)")),
    ("net/socket", re.compile(r"(udp|skb|sock|^ip_|^__ip|inet|^net_|napi|netif|^__dev|^dev_|^sk_|recvmsg|recvfrom|"
                              r"sendmsg|sendto|__sys_|___sys|csum)")),
    ("copy", re.compile(r"(copy_|__arch_copy|_copy_to|memcpy|memset|__memcpy)")),
    ("syscall entry/exit", re.compile(r"^(el0|el1|do_el0|invoke_syscall|__arm64_sys|syscall_|exit_to_user|ret_to_user|"
                                      r"arm64_|do_notify_resume|__do_sys|cortex_a76)")),
    ("irq", re.compile(r"(gic|irq|^handle_|^__handle|generic_handle|do_interrupt|call_on_irq)")),
    ("timer/time", re.compile(r"(hrtimer|ktime|timekeeping|^tick_|clockevents|arch_timer|timerqueue|__timer)")),
    ("mm", re.compile(r"(page|^mm_|do_mem|handle_mm|__do_page|tlb|flush_|^__pte|vma|folio)")),
]


def group_of(sample):
    mode, name, frames = sample
    if mode == "user":
        return "user"
    attributed = next((f for f in frames if not HELPER.match(f)), frames[0] if frames else name)
    for g, rx in GROUP_RULES:
        if rx.search(attributed):
            return g
    return "kernel other"


def parse_prof(text):
    """(profiled tid or None, attached, [(t_mono, (mode, symbol, frames))])."""
    m = re.search(r"^profiled (\d+)$", text, re.M)
    attached = "# c12 prof tid" in text
    out = []
    cur = None
    for line in text.splitlines():
        if line.startswith("S "):
            f = line.split()
            if len(f) < 4:
                cur = None
                continue
            ks, us = f[2], f[3]
            user = ks.startswith("0x")
            cur = (int(f[1]), ["user" if user else "kernel", re.sub(r"\+.*$", "", us if user else ks), []])
            out.append(cur)
        elif cur and line.startswith("\t"):
            parts = line.split()
            if len(parts) >= 2:
                cur[1][2].append(re.sub(r"\+.*$", "", parts[1]))
    return (m.group(1) if m else None), attached, [(t, tuple(s)) for t, s in out]


def server_offsets(text):
    offs = {}
    for m in re.finditer(r"clock_offset_ns stem=(\S+) before: client=(-?\d+) server=(-?\d+) "
                         r"after: client=(-?\d+) server=(-?\d+)", text):
        offs[m.group(1)] = (int(m.group(3)) + int(m.group(5))) // 2
    return offs


def used_windows(out, stem, off, arm):
    """[(tap, return)] of the pings idle_wake_read.py's sample set uses for this row."""
    recs, res = RO.samples(out, stem, off)
    if not res:
        return [], None
    pc = out.with_suffix(out.suffix + ".pcaps")
    _, client = S.ping_keys(pc / f"{stem}_ping.pcap", off)
    junk = W.inj_taps(pc / f"{stem}_pong.pcap", client) if client else []
    rows = W.classify(recs, res, junk)
    win = []
    for (tap, _v, kind, gap), (_t, _vv, pr) in zip(rows, res):
        if arm == "none" and kind == "ping" and W.IDLE_MS[0] <= gap <= W.IDLE_MS[1]:
            win.append((tap, pr[3]))
        elif arm != "none" and kind == "inj" and W.WINDOW_MS[arm][0] <= gap <= W.WINDOW_MS[arm][1]:
            win.append((tap, pr[3]))
    rtid = statistics.mode([pr[0] for _, _, pr in res])
    return win, rtid


def in_window(samples, srv_off, windows):
    ws = sorted(windows)
    hits = []
    j = 0
    for t, s in sorted(samples):
        rt = t + srv_off
        while j < len(ws) and ws[j][1] < rt:
            j += 1
        if j < len(ws) and ws[j][0] <= rt <= ws[j][1]:
            hits.append(s)
    return hits


def decide(p, cells, x_cycles_e):
    """p: {rmw: {arm: {rep: p50}}}; cells: {(rmw, arm): (Counter of groups, used pings)}."""
    lines = []
    d = {r: W.paired(p[r], "none", "50") for r in (TICKLE, CYC)}
    for r in d:
        if len(d[r]) < MIN_PAIRED:
            return f"VOID({r}: {len(d[r])} paired none/50 reps < {MIN_PAIRED})", lines
    reps = [k for k in p[TICKLE]["none"] if k in p[TICKLE]["50"] and k in p[CYC]["none"] and k in p[CYC]["50"]]
    e = [(p[TICKLE]["none"][k] - p[TICKLE]["50"][k]) - (p[CYC]["none"][k] - p[CYC]["50"][k]) for k in reps]
    if len(e) < MIN_PAIRED:
        return f"VOID(E: {len(e)} paired reps < {MIN_PAIRED})", lines
    cd = {r: W.mean_ci(d[r]) for r in d}
    me, lo, hi = W.mean_ci(e)
    for r in d:
        lines.append(f"raw d50 {r:20s} {cd[r][0]:+.2f} us [{cd[r][1]:+.2f}, {cd[r][2]:+.2f}] n={len(d[r])}")
    lines.append(f"E {me:+.2f} us [{lo:+.2f}, {hi:+.2f}] n={len(e)}")
    if cd[CYC][0] > CYC_D_MAX:
        return f"VOID(control d50 {cd[CYC][0]:+.2f} us > {CYC_D_MAX:+.1f}: the instruments moved it)", lines
    if me < E_MIN:
        return f"VOID(E {me:+.2f} us < {E_MIN:+.2f}: no E-sized extra to attribute)", lines
    for key in cells:
        n = sum(cells[key][0].values())
        if n < MIN_SAMPLES:
            return f"VOID({key[0]} {key[1]}: {n} in-window samples < {MIN_SAMPLES})", lines
    x = (x_cycles_e if x_cycles_e is not None else me * CYC_PER_US)
    groups = sorted({g for c, _ in cells.values() for g in c})
    per = {k: {g: c[g] * PERIOD / max(1, u) for g in groups} for k, (c, u) in cells.items()}
    dt = {g: per[(TICKLE, "none")][g] - per[(TICKLE, "50")][g] for g in groups}
    dc = {g: per[(CYC, "none")][g] - per[(CYC, "50")][g] for g in groups}
    lines.append(f"X = E x {CYC_PER_US} = {x:.0f} cycles a ping")
    lines.append(f"in-window cycles a ping, none - 50: TickLE {sum(dt.values()):+.0f}, CycloneDDS {sum(dc.values()):+.0f}")
    for g in sorted(groups, key=lambda k: -dt[k]):
        lines.append(f"  {g:20s} TickLE none {per[(TICKLE, 'none')][g]:7.0f} 50 {per[(TICKLE, '50')][g]:7.0f} "
                     f"delta {dt[g]:+7.0f} ({dt[g] / x:+.0%} of X) | CycloneDDS delta {dc[g]:+7.0f}")
    found = [g for g in groups if dt[g] >= 0.5 * x and abs(dc[g]) < 0.25 * dt[g]]
    if found:
        return f"FOUND ({', '.join(found)})", lines
    if all(dt[g] < 0.25 * x for g in groups):
        return "DIFFUSE", lines
    return "PARTIAL (" + ", ".join(g for g in groups if dt[g] >= 0.25 * x) + " >= 25% of X)", lines


def selftest():
    bad = 0
    def arms(none, l50, n=13, jit=0.3):
        return {"none": {r: none + jit * ((r * 7) % 5 - 2) / 2 for r in range(n)},
                "50": {r: l50 + jit * ((r * 3) % 5 - 2) / 2 for r in range(n)}}
    p = {TICKLE: arms(15.5, 13.5), CYC: arms(12.9, 12.4)}  # E +1.5 -> X 2250
    def cells(t_none, t_50, c_none, c_50, used=1000):
        return {(TICKLE, "none"): (collections.Counter(t_none), used), (TICKLE, "50"): (collections.Counter(t_50), used),
                (CYC, "none"): (collections.Counter(c_none), used), (CYC, "50"): (collections.Counter(c_50), used)}
    base = {"sched": 300, "io_uring": 300, "user": 200}
    cases = [
        (p, cells({**base, "io_uring": 450}, base, base, base), "FOUND (io_uring)"),            # +1500 = 67% of X
        (p, cells({**base, "io_uring": 450}, base, {**base, "io_uring": 450}, base), "PARTIAL"),  # control moves too
        (p, cells({**base, "io_uring": 340, "sched": 340, "user": 240}, base, base, base), "DIFFUSE"),  # +400 each
        (p, cells({**base, "io_uring": 380}, base, base, base), "PARTIAL"),                     # +800 = 36%
        ({TICKLE: arms(15.5, 13.5), CYC: arms(12.9, 11.7)}, cells(base, base, base, base), "VOID(control d50"),
        ({TICKLE: arms(13.6, 13.2), CYC: arms(12.9, 12.4)}, cells(base, base, base, base), "VOID(E"),
        (p, cells({"sched": 100, "user": 100}, base, base, base), "VOID(rmw_tickle@libc_rb none: 200"),
    ]
    for pp, cc, want in cases:
        got, _ = decide(pp, cc, None)
        ok = got.startswith(want)
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} want {want:36s} got {got}")
    txt = ("task 7 pong_node\nprofiled 7\n# c12 prof tid 7 period 10000\n"
           "S 100 _raw_spin_unlock_irq 0xffff\n\tffff _raw_spin_unlock_irq+36\n\tfffe io_poll_wake+20\n"
           "S 200 0xaaaa tt_receive\nS 300 udp_recvmsg 0xffff\n\tffff udp_recvmsg+4\n")
    tid, att, samples = parse_prof(txt)
    gs = [group_of(s) for _, s in samples]
    ok = tid == "7" and att and gs == ["io_uring", "user", "net/socket"]
    hits = in_window(samples, 0, [(150, 250)])
    ok = ok and len(hits) == 1 and hits[0][0] == "user"
    bad += not ok
    print(f"  {'ok ' if ok else 'BAD'} parse: helper skipped to its caller, user sample, window membership ({gs})")
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
    offs, soffs = R.offsets(text), server_offsets(text)
    verdicts = {}
    for m in re.finditer(r"^(\S+) bench best_effort rep(\d+) wait=block sst=on inject=(\S+) \| (\S+)", text, re.M):
        verdicts[(m.group(1), int(m.group(2)), m.group(3))] = m.group(4)
    reps = sorted({k[1] for k in verdicts})
    print(f"=== C12 step 6 reading of {out.name}: {len(reps)} reps, N_REPS {N_REPS}, MIN_SAMPLES {MIN_SAMPLES}")
    W.R.samples = RO.samples
    pdir = out.with_suffix(out.suffix + ".prof")
    p = {r: {a: {} for a in ARMS} for r in (TICKLE, CYC)}
    cells = {(r, a): [collections.Counter(), 0] for r in (TICKLE, CYC) for a in ARMS}
    syms = {(r, a): collections.Counter() for r in (TICKLE, CYC) for a in ARMS}
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
                v, line = W.row_reading(out, stem, offs[stem], arm)
                pf = pdir / f"{stem}.txt"
                tid, att, samples = parse_prof(pf.read_text(errors="replace") if pf.exists() else "")
                win, rtid = used_windows(out, stem, offs[stem], arm)
                why = ""
                if not att:
                    why = "profile did not attach"
                elif tid != rtid:
                    why = f"profiled tid {tid} is not the receiving tid {rtid}"
                elif stem not in soffs:
                    why = "no server clock offset"
                hits = in_window(samples, soffs.get(stem, 0), win) if not why else []
                line += f" | profiled {tid} receiving {rtid}, samples {len(samples)}, in-window {len(hits)} over " \
                        f"{len(win)} pings"
                if why or v is None:
                    print(f"  {stem:58s} {'VOID(' + why + ') ' if why else ''}{line}")
                    continue
                p[rmw][arm][rep] = v
                cells[(rmw, arm)][1] += len(win)
                for s in hits:
                    cells[(rmw, arm)][0][group_of(s)] += 1
                    syms[(rmw, arm)][(s[0], s[1])] += 1
                print(f"  {stem:58s} {line}")
    print("--- arms (rep p50s under the instruments; us)")
    for rmw in (TICKLE, CYC):
        for arm in ARMS:
            xs = sorted(p[rmw][arm].values())
            if xs:
                print(f"  {rmw:20s} {arm:>4s}: n_reps={len(xs):2d} median {statistics.median(xs):6.2f} "
                      f"range {xs[0]:.2f}-{xs[-1]:.2f}; in-window samples {sum(cells[(rmw, arm)][0].values())} over "
                      f"{cells[(rmw, arm)][1]} pings")
    print("--- per-symbol in-window cycles a ping (drill-down, decides nothing; top 15 by TickLE none - 50)")
    allsym = set(syms[(TICKLE, "none")]) | set(syms[(TICKLE, "50")])
    def pp(k, s):
        return syms[k][s] * PERIOD / max(1, cells[k][1])
    ranked = sorted(allsym, key=lambda s: -(pp((TICKLE, "none"), s) - pp((TICKLE, "50"), s)))
    for s in ranked[:15]:
        print(f"  {s[0]:6s} {s[1]:40s} none {pp((TICKLE, 'none'), s):6.0f} 50 {pp((TICKLE, '50'), s):6.0f} "
              f"delta {pp((TICKLE, 'none'), s) - pp((TICKLE, '50'), s):+6.0f}")
    verdict, lines = decide(p, {k: (c, u) for k, (c, u) in cells.items()}, None)
    for line in lines:
        print("  " + line)
    print(f"STEP6_VERDICT {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
