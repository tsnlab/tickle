#!/usr/bin/env python3
"""trace_replay_read.py OUT - the reader of trace_replay_mm.sh (release stage 3, trace replay with Mahimahi;
2026-11-03).

Implements the pre-registered rules of that harness's header IN CODE. Runs are loaded by time_varying_read.load_run (the
same node outputs, RESULT/RSTATS fields and run VOIDs), and the limits are time_varying_read's own constants, imported:
V1_COPIES_MAX, X1_TIMER_PER_GAP_MAX, X1_RESEND_PER_MISSING_MAX, V3_GOODPUT_DROP_MAX, V3_DEADLINE_MS.

Each trace key K is judged as trace_K minus const_K (the constant-rate link of K's mean rate, same delay and loss), per
cell, on the usable reps pooled. Prints one line per (rule, trace, cell): "<rule> <trace> <cell>
PASS|FAIL|NOISE|STATIC|VOID|N/A <text>", REPORT lines marked "(report only)", a NETNS line (where every node ran), and
a summary. NOISE: over the limit, but trace - const inside the A/A spread (the trace arm's odd reps against its even
reps). STATIC: const_K already over the same limit.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import time_varying_read as tv  # noqa: E402
from wifi_burst_read import kv, num, quantile, read_lines  # noqa: E402

# --- pre-registered (2026-11-03, before the first run). Change only before a run, and say so in the harness header.
TR_MIN_REPS = 4  # usable reps each of trace_K and const_K, else every verdict of that trace and cell is VOID
TR_MIN_DELIVERED_LARGE = 25  # rel_large copies: delivered samples pooled (and per A/A half) - 0.25 Hz gives 15 a run
TRACE_SEEN_MS = 10.0  # trace_K's largest mm-link queueing delay (data direction) >= const_K's + this, else not seen
CONST_RATE_TOL = 0.05  # const_K's realized opportunity rate within this share of its nominal
BITS_PER_OPPORTUNITY = 1500 * 8  # one MTU-sized packet per trace line (the traces' README)
JUDGED = ("T_COPIES", "T_COPIES_RETURN", "T_TIMER_PER_GAP", "T_TIMER_PER_GAP_RETURN", "T_RESEND_PER_MISSING",
          "T_RESEND_PER_MISSING_RETURN", "T_ONTIME")
ARM_RE = re.compile(r"^(delay|const_[a-z0-9]+|trace_[a-z0-9]+)$")
NETNS_RE = re.compile(r"^NSCHECK tag=(\w+) .*verdict=(\S+)$")


def pct(x):
    return tv.pct(x)


def fmt(v, f):
    if v is None:
        return "n/a"
    return pct(v) if f == "pct" else (f"{v:.2f}" if f == "f2" else f"{v:.1f}")


# --- loading ----------------------------------------------------------------------------------------------------------


def mm_log(path):
    """mm-link's own log of one direction: '<ms> # <bytes>' a delivery opportunity, '<ms> + <bytes>' an arrival,
    '<ms> - <bytes> <delay ms>' a departure, '<ms> d <packets> <bytes>' a drop; '#' lines at the top are its header.
    Returns the realized opportunity rate, the departures' queueing delays and the drops; None when unreadable."""
    if not os.path.exists(path):
        return None
    opps = arrivals = drops = 0
    first = last = None
    delays = []
    for line in read_lines(path):
        p = line.split()
        if len(p) < 3 or not p[0].isdigit():
            continue
        t = int(p[0])
        first = t if first is None else min(first, t)
        last = t if last is None else max(last, t)
        if p[1] == "#":
            opps += 1
        elif p[1] == "+":
            arrivals += 1
        elif p[1] == "-" and len(p) >= 4 and p[3].isdigit():
            delays.append(int(p[3]))
        elif p[1] == "d" and p[2].isdigit():
            drops += int(p[2])
    if not opps or first is None or last <= first:
        return None
    delays.sort()
    return {"rate_bps": opps * BITS_PER_OPPORTUNITY / ((last - first) / 1000.0), "arrivals": arrivals,
            "departures": len(delays), "drops": drops, "qdelay_p99": quantile(delays, 0.99) if delays else None,
            "qdelay_max": delays[-1] if delays else None}


def qdisc_drops(path):
    """Every qdisc's drops in a run's counters.txt (both TUNs), and each TUN's tx_dropped."""
    total = 0
    for line in read_lines(path):
        m = re.search(r"Sent \d+ bytes \d+ pkt \(dropped (\d+)", line)
        if m:
            total += int(m.group(1))
        if line.startswith("== "):
            total += int(num(kv(line), "tx_dropped"))
    return total


def netem_loss(r, side):
    """The realized netem loss on one TUN (handle 10:, 'out' in O, 'in' in I): drops / (sent + drops)."""
    sent, drop = r["counters"].get(side, {}).get("bands", {}).get("10", (0, 0))
    return drop / (sent + drop) if sent + drop else None


def load(out):
    runs, dur = tv.load(out)
    prov_lines = read_lines(os.path.join(out, "provenance.txt"))
    prov = kv(" ".join(x for x in prov_lines if not x.startswith("TRACE ")))
    traces = {}
    for line in prov_lines:
        if line.startswith("TRACE "):
            d = kv(line)
            traces[d["key"]] = d
    ctx = {"dur": dur, "delay": num(prov, "mm_delay"), "loss": num(prov, "mm_loss") / 100.0,
           "data_dir": prov.get("data_dir", "down"), "traces": traces, "default_ns": prov.get("default_netns", "?")}
    kept = []
    for r in runs:
        if not ARM_RE.match(r["arm"]):
            continue
        d = os.path.join(out, "runs", f"{r['arm']}-{r['cell']}-{r['rep']}")
        r["kind"] = "delay" if r["arm"] == "delay" else r["arm"].split("_", 1)[0]
        r["key"] = None if r["kind"] == "delay" else r["arm"].split("_", 1)[1]
        r["drops"] = qdisc_drops(os.path.join(d, "counters.txt"))
        r["mm"] = mm_log(os.path.join(d, f"mm_{ctx['data_dir']}.log")) if r["kind"] != "delay" else None
        checks = [m.groups() for m in map(NETNS_RE.match, read_lines(os.path.join(d, "netns.txt"))) if m]
        ns_of = {}
        for line in read_lines(os.path.join(d, "netns.txt")):
            if line.startswith("NSCHECK"):
                k = kv(line)
                ns_of[k.get("tag")] = k.get("ns")
        r["netns"] = checks
        r["netns_default"] = any(v == "DEFAULT_NETNS" for _, v in checks) or ctx["default_ns"] in ns_of.values()
        bad = [f"{t} {v}" for t, v in checks if v != "ok"]
        if len(checks) != 2:
            bad.append(f"{len(checks)} NSCHECK lines, not 2")
        elif len(set(ns_of.values())) != 2:
            bad.append("both nodes in one namespace: the link was not crossed")
        if bad:
            r["void"].append("namespaces: " + "; ".join(bad))
        r["treat"] = treatment(r, ctx)
        kept.append(r)
    return kept, ctx


def treatment(r, ctx):
    """Why a run's own outcome does not show its treatment (empty when it does)."""
    why = []
    if r["void"]:
        return why
    d = ctx["delay"]
    p50 = tv.x1_owd(r, 0.5)
    need = d - tv.X1_DELAY_TOL_MS - tv.X1_DELAY_TOL_REL * d
    if p50 is None or p50 < need:
        why.append(f"one-way p50 {fmt(p50, 'ms')} ms under {need:.2f} ms: the delay is not seen")
    if r["kind"] == "delay":
        return why
    if ctx["loss"]:
        side = "out" if ctx["data_dir"] == "down" else "in"
        real = netem_loss(r, side)
        lo, hi = tv.LOSS_REALIZED
        if real is None or not lo * ctx["loss"] <= real <= hi * ctx["loss"]:
            why.append(f"realized netem loss {pct(real)} on the writer's TUN for nominal {pct(ctx['loss'])}")
    if r["mm"] is None:
        why.append(f"mm-link log mm_{ctx['data_dir']}.log missing or unreadable")
    elif r["kind"] == "const":
        bps = num(ctx["traces"].get(r["key"], {}), f"{ctx['data_dir']}_bps", -1)
        nominal = max(1, round(bps / BITS_PER_OPPORTUNITY)) * BITS_PER_OPPORTUNITY
        if bps < 0 or abs(r["mm"]["rate_bps"] / nominal - 1) > CONST_RATE_TOL:
            why.append(f"mm-link opportunity rate {r['mm']['rate_bps']:.0f} bit/s, nominal {nominal}")
    return why


def usable(rs):
    return [r for r in rs if not r["void"] and not r["treat"]]


def runs_of(runs, arm, cell):
    return [r for r in runs if r["arm"] == arm and r["cell"] == cell]


# --- metrics: pooled (numerator, denominator) over runs, None when a part is absent ---------------------------------


def pooled(rs, part):
    n = d = 0
    for r in rs:
        x = part(r)
        if x is None:
            return None
        n, d = n + x[0], d + x[1]
    return (n, d)


def copies_part(r):
    return (r["copies"], r["copy_den"]) if r["copy_den"] else None


def ret_copies_part(r):
    parts = [tv.x1_get(r, "ping.late_below_ack"), tv.x1_get(r, "ping.duplicates")]
    if None in parts or "duplicates" not in r["ping"] or "scored" not in r["ping"]:
        return None
    return (sum(parts) + int(num(r["ping"], "duplicates")), int(num(r["ping"], "scored")))


def pair(n, d):
    return None if n is None or d is None else (n, d)


METRICS = {  # name: (cells, per-run part, floor on the pooled denominator, limit kind, limit, format)
    "T_COPIES": (tv.REL_CELLS, copies_part, None, "delta", tv.V1_COPIES_MAX, "pct"),
    "T_COPIES_RETURN": (("rel_small",), ret_copies_part, tv.MIN_DELIVERED, "delta", tv.V1_COPIES_MAX, "pct"),
    "T_TIMER_PER_GAP": (tv.REL_CELLS, lambda r: pair(tv.x1_rs(r, "timer"), tv.x1_rs(r, "gaps_opened")),
                        tv.X1_MIN_GAPS, "abs", tv.X1_TIMER_PER_GAP_MAX, "f2"),
    "T_TIMER_PER_GAP_RETURN": (("rel_small",), lambda r: pair(tv.x1_get(r, "ping.timer"),
                                                              tv.x1_get(r, "ping.gaps_opened")),
                               tv.X1_MIN_GAPS, "abs", tv.X1_TIMER_PER_GAP_MAX, "f2"),
    "T_RESEND_PER_MISSING": (tv.REL_CELLS, lambda r: pair(tv.x1_resent(r), tv.x1_rs(r, "missing_opened")),
                             tv.X1_MIN_GAPS, "abs", tv.X1_RESEND_PER_MISSING_MAX, "f2"),
    "T_RESEND_PER_MISSING_RETURN": (("rel_small",), lambda r: pair(tv.x1_get(r, "echo.retransmitted"),
                                                                   tv.x1_get(r, "ping.missing_opened")),
                                    tv.X1_MIN_GAPS, "abs", tv.X1_RESEND_PER_MISSING_MAX, "f2"),
}


def copies_floor(cell):
    return tv.MIN_DELIVERED if cell != "rel_large" else TR_MIN_DELIVERED_LARGE


def ratio_value(rs, name, cell):
    _, part, floor, _, _, _ = METRICS[name]
    p = pooled(rs, part)
    if p is None or not rs:
        return None
    if name == "T_COPIES":  # the floor is on delivered samples, not on the per-unit denominator
        if sum(r["delivered"] for r in rs) < copies_floor(cell):
            return None
    elif p[1] < floor:
        return None
    return p[0] / p[1] if p[1] else None


def ontime_share(rs, deadline_ms):
    off = sum(r["offered"] for r in rs)
    got = sum(len(tv.on_time(r, deadline_ms)) for r in rs)
    return got / off if off else None


def ontime_ratio(rel, be, deadline_ms):
    """rel_small's on-time share over be_small's, the same arm: RELIABLE against the channel itself."""
    a, b = ontime_share(rel, deadline_ms), ontime_share(be, deadline_ms)
    return a / b if a is not None and b else None


def halves(rs):
    odd = [r for r in rs if int(r["rep"]) % 2]
    return odd, [r for r in rs if not int(r["rep"]) % 2]


# --- judging ----------------------------------------------------------------------------------------------------------


def verdict(kind, limit, t, c, spread):
    """(verdict, distance) for trace value t against const value c: STATIC when c fails the limit itself."""
    if kind == "delta":
        over_c, over, d = c > limit, t - c > limit, t - c
    elif kind == "abs":
        over_c, over, d = c > limit, t > limit, t - c
    else:  # "ratio_min": t >= limit x c
        over_c, over, d = False, t < limit * c, c - t
    if over_c:
        return "STATIC"
    if not over:
        return "PASS"
    if spread is None:
        return "VOID"
    return "FAIL" if d > spread else "NOISE"


def judge(emit, name, key, cell, t_runs, c_runs, value, kind, limit, f, gate):
    """One rule on one trace and cell. value(runs) -> number or None; gate: a reason that voids it, or None."""
    ut, uc = usable(t_runs), usable(c_runs)
    t, c = value(ut), value(uc)
    odd, even = halves(ut)
    a, b = value(odd), value(even)
    spread = abs(a - b) if a is not None and b is not None else None
    text = (f"trace {fmt(t, f)} const {fmt(c, f)} ({kind} {fmt(limit, f) if kind != 'ratio_min' else limit}) "
            f"A/A {fmt(spread, f)}; reps {len(ut)}/{len(t_runs)} trace, {len(uc)}/{len(c_runs)} const")
    if gate:
        emit(name, key, cell, "VOID", f"{gate}; {text}")
    elif len(ut) < TR_MIN_REPS or len(uc) < TR_MIN_REPS:
        emit(name, key, cell, "VOID", f"under {TR_MIN_REPS} usable reps; {text}")
    elif t is None or c is None:
        emit(name, key, cell, "N/A", f"too few samples or a field absent; {text}")
    else:
        v = verdict(kind, limit, t, c, spread)
        emit(name, key, cell, v, text + (" (A/A spread n/a)" if v == "VOID" else ""))


def control_void(runs, cells):
    """The clean control (mm-delay alone) must lose nothing; its rel_large must resend nothing."""
    ctl = [r for r in runs if r["arm"] == "delay"]
    if not ctl:
        return ["no control arm 'delay' in this OUT"], []
    why, large = [], []
    for cell in cells:
        if not runs_of(ctl, "delay", cell):
            why.append(f"no control run of {cell}")
    for r in ctl:
        name = f"delay/{r['cell']}/{r['rep']}"
        if r["void"] or r["treat"]:
            why.append(f"{name}: {'; '.join(r['void'] + r['treat'])}")
            continue
        share = r["delivered"] / r["offered"] if r["offered"] else 0.0
        need = tv.CONTROL_BE_MIN if r["cell"] == "be_small" else 1.0
        if share < need:
            why.append(f"{name}: delivered {pct(share)} (< {pct(need)})")
        if r["drops"]:
            why.append(f"{name}: {r['drops']} qdisc or TUN drops")
        if r["cell"] == "rel_large" and r.get("retransmitted", -1) != 0:
            large.append(f"{name}: retransmitted {r.get('retransmitted')}")
    return why, large


def trace_seen(runs, key, cell):
    """trace_K's largest data-direction queueing delay must exceed const_K's by TRACE_SEEN_MS."""
    t = [r["mm"]["qdelay_max"] for r in usable(runs_of(runs, "trace_" + key, cell)) if r["mm"]["qdelay_max"]]
    c = [r["mm"]["qdelay_max"] for r in usable(runs_of(runs, "const_" + key, cell)) if r["mm"]["qdelay_max"]]
    if not t or not c:
        return "no usable mm-link log to show the trace"
    if max(t) < max(c) + TRACE_SEEN_MS:
        return f"trace queueing max {max(t)} ms, not {TRACE_SEEN_MS:g} ms above const's {max(c)} ms: not seen"
    return None


def report(runs, arm, cell, ctx):
    rs = usable(runs_of(runs, arm, cell))
    allr = runs_of(runs, arm, cell)
    if not allr:
        return None
    owd = tv.pooled_owd(rs)
    parts = [f"usable {len(rs)}/{len(allr)}"]
    off = sum(r["offered"] for r in rs)
    if off:
        parts.append(f"delivered {pct(sum(min(r['delivered'], r['offered']) for r in rs) / off)}")
    if owd:
        parts.append(f"owd p50 {quantile(owd, 0.5):.1f} p99 {quantile(owd, 0.99):.1f} ms")
    rtt = [tv.x1_rt(r, i) for r in rs for i in (0, 1)]
    if rs and cell != "rel_large" and None not in rtt:
        parts.append(f"rtt p50 {tv.x1_median(rtt[0::2]):.1f} p99 {tv.x1_median(rtt[1::2]):.1f} ms (rep medians)")
    if cell in tv.REL_CELLS and rs:
        ab = [tv.x1_abandoned(r) for r in rs]
        parts.append(f"abandoned {'n/a' if None in ab else int(sum(ab))}")
        imm = pooled(rs, lambda r: pair(tv.x1_rs(r, "immediate"), tv.x1_rs(r, "gaps_opened")))
        parts.append(f"immediate/gap {fmt(imm[0] / imm[1] if imm and imm[1] else None, 'f2')}")
        parts.append(f"ontime {pct(ontime_share(rs, tv.V3_DEADLINE_MS[cell] + ctx['delay']))}")
    mms = [r["mm"] for r in rs if r.get("mm")]
    if mms:
        parts.append(f"mm {ctx['data_dir']} rate {sum(m['rate_bps'] for m in mms) / len(mms) / 1e6:.2f} Mbit/s "
                     f"qdelay p99 {max(m['qdelay_p99'] or 0 for m in mms)} max {max(m['qdelay_max'] or 0 for m in mms)}"
                     f" ms drops {sum(m['drops'] for m in mms)}")
    if arm != "delay" and ctx["loss"] and rs:
        side = "out" if ctx["data_dir"] == "down" else "in"
        parts.append(f"netem loss {pct(tv.x1_median([netem_loss(r, side) for r in rs]))}")
    return ", ".join(parts)


def main():
    if len(sys.argv) != 2:
        print("usage: trace_replay_read.py OUT", file=sys.stderr)
        return 2
    out = sys.argv[1]
    runs, ctx = load(out)
    lines = []

    def emit(rule, key, cell, v, text):
        lines.append((rule, key, cell, v))
        print(f"{rule:27} {key:5} {cell:9} {v:6} {text}")

    cells = sorted({r["cell"] for r in runs}, key=tv.ALL_CELLS.index)
    keys = sorted({r["key"] for r in runs if r["key"]})
    print(f"=== trace_replay_read {out}: {len(runs)} runs, dur {ctx['dur']:g} s, delay {ctx['delay']:g} ms, "
          f"loss {pct(ctx['loss'])}, data {ctx['data_dir']}link; limits from time_varying_read")
    nodes = sum(len(r["netns"]) for r in runs)
    ok = sum(1 for r in runs for _, v in r["netns"] if v == "ok")
    default = [f"{r['arm']}-{r['cell']}-{r['rep']}" for r in runs if r["netns_default"]]
    print(f"NETNS {len(runs)} runs, {nodes} node checks, {ok} ok (each node's /proc/<pid>/ns/net differs from this "
          f"PC's default {ctx['default_ns']} and the PID is in `ip netns pids` of its run's O or I); "
          + (f"IN THE DEFAULT NAMESPACE: {', '.join(default)}" if default else "none in the default namespace"))
    for line in read_lines(os.path.join(out, "skips.txt")):
        print(line)
    for r in runs:
        if r["void"] or r["treat"]:
            print(f"VOID run {r['arm']}-{r['cell']}-{r['rep']}: {'; '.join(r['void'] + r['treat'])}")
    cv, cl = control_void(runs, cells)
    if default:
        cv = [f"a node ran in the default namespace ({', '.join(default)})"] + cv
    if cl:
        print("CONTROL_RESENDS delay rel_large (rel_large verdicts VOID): " + "; ".join(cl))
    for arm in ["delay"] + [f"{k}_{key}" for key in keys for k in ("const", "trace")]:
        for cell in cells:
            rep = report(runs, arm, cell, ctx)
            if rep:
                print(f"REPORT {arm:11} {cell:9} (report only) {rep}")
    for key in keys:
        t_arm, c_arm = "trace_" + key, "const_" + key
        for cell in cells:
            if not runs_of(runs, t_arm, cell) and not runs_of(runs, c_arm, cell):
                continue
            gate = "; ".join(cv) if cv else None
            if not gate and cell == "rel_large" and cl:
                gate = "the control resent on rel_large"
            if not gate:
                gate = trace_seen(runs, key, cell)
            tr, cr = runs_of(runs, t_arm, cell), runs_of(runs, c_arm, cell)
            for name, (mcells, _, _, kind, limit, f) in METRICS.items():
                if cell in mcells:
                    judge(emit, name, key, cell, tr, cr, lambda rs, n=name, c=cell: ratio_value(rs, n, c), kind,
                          limit, f, gate)
            if cell == "rel_small":
                be_t, be_c = runs_of(runs, t_arm, "be_small"), runs_of(runs, c_arm, "be_small")
                dl = tv.V3_DEADLINE_MS[cell] + ctx["delay"]
                g = gate or (None if be_t and be_c else "no be_small runs to normalize by")
                lat = [f"{r['arm']}-{r['cell']}-{r['rep']}" for r in tr + cr + be_t + be_c if r["lat_void"]]
                g = g or (f"PC loaded at the start of {', '.join(lat)}" if lat else None)

                def value(rs, be_t=be_t, be_c=be_c, dl=dl):
                    if not rs:
                        return None
                    be = be_t if rs[0]["arm"].startswith("trace_") else be_c
                    reps = {r["rep"] for r in rs}
                    return ontime_ratio(rs, [r for r in usable(be) if r["rep"] in reps], dl)

                judge(emit, "T_ONTIME", key, cell, tr, cr, value, "ratio_min", 1 - tv.V3_GOODPUT_DROP_MAX, "f2", g)
            rel_be = []
            for arm in (t_arm, c_arm):
                rel = tv.pooled_owd(usable(runs_of(runs, arm, "rel_small")))
                be = tv.pooled_owd(usable(runs_of(runs, arm, "be_small")))
                if rel and be and cell == "rel_small":
                    rel_be.append(f"{arm} {quantile(rel, 0.99) - quantile(be, 0.99):.1f} ms")
            if rel_be:
                print(f"REPORT {key:11} rel_small (report only) rel_small minus be_small one-way p99: "
                      + ", ".join(rel_be))
    print("--- summary")
    for rule in JUDGED:
        got = [x for x in lines if x[0] == rule]
        if got:
            print(f"{rule}: " + ", ".join(f"{k}/{c} {v}" for _, k, c, v in got))
    counts = {}
    for x in lines:
        counts[x[3]] = counts.get(x[3], 0) + 1
    print("verdicts: " + (", ".join(f"{v} {n}" for v, n in sorted(counts.items())) or "none"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
