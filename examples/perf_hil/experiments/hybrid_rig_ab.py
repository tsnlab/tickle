#!/usr/bin/env python3
"""hybrid_rig_ab.py - the rig A/B of the hybrid transport (docs/DESIGN.md section 8, "Hybrid transport", its
pre-registration table), read by rules written 2026-10-11 before any rig run (~/rig_queue_hybrid.sh).

Input: one rmw_keepall_rig.sh invocation, OUT.runs/ (index.txt, each run's _pub.log, _sub.log and _net.txt), read
through largemsg_rig_summary.py's own run records (delivery, CPU per MB, clock-corrected latency median and p99 lower
bound). Two kinds of invocation, named by --zero or --loss:

  --zero  one cell at 0% loss (Array1m 30 Hz or Array4m 15 Hz, RELIABLE KEEP_LAST 10 or BEST_EFFORT KEEP_LAST 5), arms
          tickle@head (B, the hybrid transport, AUTO), tickle@pre (A, main without it), fastdds, cyclonedds.
  --loss  Array1m 30 Hz RELIABLE KEEP_LAST 10 at 0.01%, 0.1% and 1% netem loss, arms tickle@head (AUTO), tickle@aa (AUTO
          again: the A/A floor), tickle@mtu (forced n = 1), tickle@ip (forced IP, n = 45, the reader's cap off).

RULES (enforced below)
  TREATMENT  every TickLE run's publisher and subscriber logs carry rmw_tickle's "hybrid transport:
             large_fragmentation=<mode> reassembly_cap=<on|off>" line with the arm's mode (head, aa: auto; mtu: mtu; ip:
             ip with the cap off on both), and core's "hybrid:" counters at destroy. The publisher's line gives runs,
             the units in them and alone, and n_hist (n chosen per sample and destination: buckets 1, 2-3, 4-7, 8-15,
             16-31, 32-44, 45). A run is VOID when either is missing ("could not look" is not "off"), and:
               mtu  VOID unless runs = 0 and mtu units > 0 and every n_hist count is in bucket 1;
               ip   VOID unless runs > 0 and every n_hist count is in bucket 45 (a forced arm whose n is not the one
                    forced);
               head, aa at 0% loss  VOID unless runs > 0 and the median n_hist bucket is 45 (AUTO at 0% must reach
                    n_max); under loss AUTO's n is reported, not judged.
             tickle@pre (A, main) must print no "hybrid:" counters with runs > 0, or the whole reading is VOID.
             The realized treatment of every run is printed beside it: runs, run units, MTU units, n_hist, and the
             reader's advertised ceiling, cap and collapses.
  NET        each run's <stem>_net.txt: the server's (reader's) eth0 rx bytes are the wire bytes, every frame of every
             datagram and repair (an interface counter, not tcpdump: same frames, no capture cost). Its Udp
             RcvbufErrors delta is printed per run and summed per arm - reported, never tuned (a 4 MB BEST_EFFORT cell
             may overflow the receive socket, the I4s cause; the io_uring drain fix is not on main). A run whose net
             file is missing or unread has no wire-bytes value.
  USABLE     a run with a '-' latency counts for delivery, CPU and wire bytes only. Fewer than 3 usable runs in an arm
             a comparison needs: that comparison is NO VERDICT.
  DELIVERY   RELIABLE: every TickLE run delivered 100% with lost 0, or that arm's cell is LOSE whatever else it says.
  --zero     (the cell step A lost) pub and sub CPU ms/MB, head against fastdds and against cyclonedds by the range
             rule: WIN when every head run is below every run of the vendor, LOSE when every one is above, else DRAW.
             The criterion is WIN or DRAW against both on both, with RELIABLE delivery 100%. Printed beside it, never
             deciding: head against pre (Welch: BETTER/WORSE beyond 2 SE and the vendors' max-min drift) on CPU per
             MB, latency median, p99 lower bound and sub peak RSS.
  --loss     per cell, AUTO (tickle@head) against each forced arm on wire bytes per delivered unit (rx bytes / (whole
             samples received x units per sample: Array1m 723, Array4m 2889)) and on p99 (lower bound, ms):
             d = mean(head) - mean(forced), SE Welch; the floor is the A/A pair's |mean(aa) - mean(head)| on the same
             metric. WIN iff d < -2 SE and -d > floor; LOSE iff d > 2 SE and d > floor; else DRAW.
               0.01%  PASS iff WIN against mtu and against ip, on both metrics.
               0.1%   PASS iff WIN against mtu and against ip, on both metrics (AUTO's median n printed; the design
                      expects about 4 to 8).
               1%     PASS iff DRAW or WIN against mtu and WIN against ip, on both metrics.
             FALSIFIED (the design, docs/DESIGN.md "What would falsify the design") when AUTO LOSEs to mtu at 0.01% or
             0.1% on either metric, when AUTO's p99 LOSEs to mtu at any loss, or when the reader's cap collapsed in an
             AUTO run at 0.01% (collapses > 0 on its "hybrid:" line: the collapse test fired on frame loss).

UDP OFFLOAD (added 2026-10-11 at the user's instruction, "apply GRO/GSO too if they help"; written before any run)
  Arms: tickle@nooffload (AUTO, TT_UDP_OFFLOAD=0) beside tickle@head in every 0% invocation and in the loss cells;
  tickle@mtunooff (forced MTU, TT_UDP_OFFLOAD=0) beside tickle@mtu in the loss cells. Same binary; the switch is the
  only difference. TREATMENT: an arm with offload off is VOID unless every traffic line of both ends shows
  udp_offload=0, gso_sends=0, gro_merged=0; tickle@mtu (forced MTU, offload on) is VOID unless its publisher sent
  with UDP_SEGMENT (gso_sends > 0) and its subscriber merged (gro_merged > 0). AUTO arms' counters are printed.
  COMPARISON (offload on minus off, per cell): d and SE Welch; the floor is the A/A pair's |mean(aa) - mean(head)| in a
  loss cell, the vendors' max-min in a 0% cell; BETTER iff d < -2 SE and -d > floor (for delivered %, the sign
  flipped), WORSE symmetric. Metrics: pub and sub CPU ms/MB, wire bytes per delivered unit, p99, delivered %.
  DECISION (--offload, over every invocation of the queue):
    WHERE IT APPLIES - MTU-mode traffic: forced MTU at 1% (tickle@mtu vs tickle@mtunooff) and AUTO at 1%
    (tickle@head vs tickle@nooffload, its MTU repairs and n = 1 samples).
    KEEP-ON      in both comparisons where it applies, pub or sub CPU per MB BETTER and nothing WORSE (CPU, wire bytes,
                 p99, delivered %), AND in every 0% and 0.01% AUTO comparison nothing WORSE.
    DROP         anything WORSE where it applies: it harms the traffic it is for - removed from the branch.
    DEFAULT-OFF  otherwise (no gain where it applies, or a WORSE only in a 0% / 0.01% AUTO cell): it ships with
                 TT_UDP_OFFLOAD defaulting to 0.
    NO VERDICT   fewer than 3 usable runs in an arm a comparison needs.

Exit 0 when the reading is complete (whatever it says), 2 when VOID or NO VERDICT anywhere, 1 on a missing input.
Usage: hybrid_rig_ab.py --zero|--loss OUT   (OUT.runs/ must exist)
       hybrid_rig_ab.py --offload OUT [OUT ...]   (every 0% and loss invocation of the queue)
"""
import math
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from largemsg_rig_summary import load  # noqa: E402

UNITS = {"Array1m": 723, "Array4m": 2889}  # tt_sample_datagrams() of the sample's CDR (tests/test_hybrid_send.c)
N_BUCKETS = ("1", "2-3", "4-7", "8-15", "16-31", "32-44", "45")
VENDORS = ("fastdds", "cyclonedds")
MODE_OF = {"tickle@head": "auto", "tickle@aa": "auto", "tickle@mtu": "mtu", "tickle@ip": "ip",
           "tickle@nooffload": "auto", "tickle@mtunooff": "mtu"}
OFFLOAD_OFF = ("tickle@nooffload", "tickle@mtunooff")
OFFLOAD_PAIRS = (("tickle@head", "tickle@nooffload"), ("tickle@mtu", "tickle@mtunooff"))
TRAFFIC_RE = re.compile(r"traffic: .*?udp_offload=(\d+) gro_reads=\d+ gro_merged=(\d+) .*?gso_sends=(\d+)")
HYBRID_RE = re.compile(r"hybrid: runs=(\d+) run_units=(\d+) mtu_units=(\d+) n_hist=([\d/]+) advertised=(\d+) "
                       r"cap=(\d+) cap_off=(\d) collapses=(\d+) probes=(\d+) path_failures=(\d+)")
RMW_RE = re.compile(r"hybrid transport: large_fragmentation=(\w+) reassembly_cap=(\w+)")


def stem_of(r):
    return f"{r['arm']}_{r['topic']}_l{r['loss']}_r{r['rep']}"


def hybrid_of(path):
    """(rmw (mode, cap) or None, core counters dict or None) from one perf_test log."""
    if not path.exists():
        return None, None
    text = path.read_text(errors="replace")
    rmw = RMW_RE.findall(text)
    core = HYBRID_RE.findall(text)
    counters = None
    if core:
        # The context with the most traffic: perf_test has one rmw context, but a log may hold more than one line.
        best = max(core, key=lambda c: int(c[0]) + int(c[2]))
        counters = {"runs": int(best[0]), "run_units": int(best[1]), "mtu_units": int(best[2]),
                    "n_hist": [int(x) for x in best[3].split("/")], "advertised": int(best[4]), "cap": int(best[5]),
                    "cap_off": int(best[6]), "collapses": int(best[7]), "probes": int(best[8]),
                    "path_failures": int(best[9])}
    return (rmw[-1] if rmw else None), counters


def median_bucket(hist):
    total = sum(hist)
    if total == 0:
        return None
    seen = 0
    for i, c in enumerate(hist):
        seen += c
        if 2 * seen >= total:
            return i
    return len(hist) - 1


def net_of(path):
    """(server rx bytes, server RcvbufErrors, server ReasmFails) deltas, or None for what was not read."""
    if not path.exists():
        return None, None, None
    snaps = {}
    for line in path.read_text(errors="replace").splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] == "server":
            snaps[parts[0]] = dict(p.split("=", 1) for p in parts[2:] if "=" in p)
    b, a = snaps.get("before"), snaps.get("after")

    def delta(key):
        try:
            return int(a[key]) - int(b[key])
        except (TypeError, KeyError, ValueError):
            return None

    if b is None or a is None:
        return None, None, None
    return delta("rx_bytes"), delta("udp_rcvbuf_errors"), delta("ip_reasm_fails")


def offload_of(path):
    """(udp_offload, gro_merged, gso_sends) summed over a log's traffic lines, or None when it has none."""
    if not path.exists():
        return None
    lines = TRAFFIC_RE.findall(path.read_text(errors="replace"))
    if not lines:
        return None
    return (max(int(x[0]) for x in lines), sum(int(x[1]) for x in lines), sum(int(x[2]) for x in lines))


def treatment(runs, r):
    """(why VOID or "", what was realized) for one TickLE run."""
    arm = r["arm"]
    zero = r["loss"] == 0
    p_rmw, p_core = hybrid_of(runs / f"{stem_of(r)}_pub.log")
    s_rmw, s_core = hybrid_of(runs / f"{stem_of(r)}_sub.log")
    if arm == "tickle@pre":
        bad = p_core is not None and p_core["runs"] > 0
        return ("tickle@pre sent runs: A is not main" if bad else ""), "pre (no hybrid transport)"
    want = MODE_OF.get(arm)
    if want is None:
        return f"no treatment rule for {arm}", ""
    if p_rmw is None or s_rmw is None:
        return "no 'hybrid transport:' line from rmw_tickle on both sides", ""
    if p_core is None or s_core is None:
        return "no core 'hybrid:' counters on both sides", ""
    if p_rmw[0] != want or s_rmw[0] != want:
        return f"mode pub {p_rmw[0]} sub {s_rmw[0]}, not {want}", ""
    hist = p_core["n_hist"]
    mb = median_bucket(hist)
    seen = (f"runs={p_core['runs']} run_units={p_core['run_units']} mtu_units={p_core['mtu_units']} "
            f"n_hist={'/'.join(map(str, hist))} median_n={N_BUCKETS[mb] if mb is not None else '-'} "
            f"reader advertised={s_core['advertised']} cap={s_core['cap']} cap_off={s_core['cap_off']} "
            f"collapses={s_core['collapses']} probes={s_core['probes']} path_failures={s_core['path_failures']}")
    total = sum(hist)
    if want == "mtu" and not (p_core["runs"] == 0 and p_core["mtu_units"] > 0 and hist[0] == total and total > 0):
        return "forced MTU arm sent runs or no MTU units", seen
    if want == "ip":
        if p_rmw[1] != "off" or s_rmw[1] != "off":
            return f"forced IP arm with the reader's cap {s_rmw[1]}", seen
        if not (p_core["runs"] > 0 and total > 0 and hist[-1] == total):
            return "forced IP arm whose n is not 45 throughout", seen
    if want == "auto" and zero and not (p_core["runs"] > 0 and mb == len(hist) - 1):
        return "AUTO at 0% loss whose median n is not n_max", seen
    p_off, s_off = offload_of(runs / f"{stem_of(r)}_pub.log"), offload_of(runs / f"{stem_of(r)}_sub.log")
    if p_off is None or s_off is None:
        return "no traffic line (UDP offload counters) on both sides", seen
    seen += f" offload pub {p_off[0]} gso_sends={p_off[2]} sub {s_off[0]} gro_merged={s_off[1]}"
    if arm in OFFLOAD_OFF and (p_off != (0, 0, 0) or s_off != (0, 0, 0)):
        return "offload not off", seen
    if arm == "tickle@mtu" and not (p_off[2] > 0 and s_off[1] > 0):
        return "forced MTU with offload on, but no UDP_SEGMENT send or no merged read", seen
    return "", seen


def welch(a, b):
    d = statistics.mean(a) - statistics.mean(b)
    se = math.sqrt(statistics.variance(a) / len(a) + statistics.variance(b) / len(b))
    return d, se


def fin(xs):
    return [x for x in xs if isinstance(x, (int, float)) and not math.isnan(x)]


def range_rule(head, other):
    if max(head) < min(other):
        return "WIN"
    if min(head) > max(other):
        return "LOSE"
    return "DRAW"


def read_invocation(out, quiet=False):
    """(meta, usable runs by cell and arm, cells LOSE on delivery) of one rmw_keepall_rig.sh invocation, or an exit
    status: 1 missing, 2 VOID overall."""
    runs = Path(f"{out}.runs")
    if not (runs / "index.txt").exists():
        print(f"MISSING: {runs}/index.txt")
        return 1
    meta, _samehost, recs = load(runs)
    reliable = "--reliable" in meta.get("qos_args", "")
    if not quiet:
        print(f"=== hybrid rig A/B: {runs}, qos [{meta.get('qos_args')}] rate {meta.get('pub_rate')} Hz ===")
    # ---- per run: VOID, treatment, net ----------------------------------------------------------------------------
    usable = {}
    for r in sorted(recs, key=lambda r: (r["topic"], r["loss"], r["arm"], r["rep"])):
        rx, rcvbuf, reasm = net_of(runs / f"{stem_of(r)}_net.txt")
        r["rcvbuf"] = rcvbuf
        units = UNITS.get(r["topic"])
        r["wire_per_unit"] = (rx / (r["recv"] * units) if rx is not None and units and r.get("recv") else float("nan"))
        why, seen = ("", "")
        if r["arm"].startswith("tickle@"):
            why, seen = treatment(runs, r)
        if r["void"] or why:
            if not quiet:
                print(f"VOID {stem_of(r)}: {r['void'] or why} {seen}")
            if r["arm"] == "tickle@pre" and why:
                print("VOID overall: tickle@pre is not main")
                return 2
            continue
        if not quiet:
                print(f"RUN {stem_of(r)} recv {r['recv']} lost {r['lost']} delivered {r['delivered_pct']:.2f}% "
                  f"cpu pub {r['cpu_pub']:.3f} sub {r['cpu_sub']:.3f} ms/MB lat med {r['lat_med']:.2f} p99>= "
                  f"{r['p99_lb']:.2f} ms wire/unit {r['wire_per_unit']:.1f} B rcvbuf_errors {rcvbuf} "
                  f"reasm_fails {reasm} "
                  f"| {seen}")
        usable.setdefault((r["topic"], r["loss"]), {}).setdefault(r["arm"], []).append(r)
    # ---- RcvbufErrors per arm (reported) -----------------------------------------------------------------------------
    for cell, arms in sorted(usable.items()):
        for arm, rs in sorted(arms.items()):
            if quiet:
                break
            errs = [x["rcvbuf"] for x in rs if x["rcvbuf"] is not None]
            print(f"RCVBUF {cell[0]} l{cell[1]} {arm}: RcvbufErrors {sum(errs)} over {len(errs)} runs "
                  f"(per run {', '.join(map(str, errs)) or '-'})")
    # ---- delivery -----------------------------------------------------------------------------------------------------
    lose = set()
    if reliable:
        for cell, arms in usable.items():
            for arm, rs in arms.items():
                bad = [x for x in rs if arm.startswith("tickle@") and (x["lost"] > 0 or x["delivered_pct"] < 100.0)]
                if bad:
                    lose.add((cell, arm))
                    if not quiet:
                        print(f"DELIVERY {cell[0]} l{cell[1]} {arm}: LOSE - {len(bad)} run(s) below 100% or with loss")
    return meta, usable, lose


def compare(on, off, metric, floor):
    """BETTER / WORSE / HELD for offload on against off, or None with fewer than 3 usable runs either side."""
    a, b = fin(x[metric] for x in on), fin(x[metric] for x in off)
    if len(a) < 3 or len(b) < 3:
        return None, 0.0, 0.0
    d, se = welch(a, b)
    if metric == "delivered_pct":
        d = -d  # more delivered is better
    if d < -2 * se and -d > floor:
        return "BETTER", d, se
    if d > 2 * se and d > floor:
        return "WORSE", d, se
    return "HELD", d, se


def offload_decision(outs):
    applies, elsewhere, incomplete = [], [], False
    for out in outs:
        got = read_invocation(out, quiet=True)
        if isinstance(got, int):
            return got
        _meta, usable, lose = got
        for cell, arms in sorted(usable.items()):
            topic, loss = cell
            for on_arm, off_arm in OFFLOAD_PAIRS:
                on, off = arms.get(on_arm, []), arms.get(off_arm, [])
                if not on and not off:
                    continue
                where = "applies" if float(loss) >= 1 else "elsewhere"
                if float(loss) == 0.1 or (where == "elsewhere" and on_arm != "tickle@head"):
                    continue  # not in the pre-registration: printed by --loss, not deciding
                for metric in ("cpu_pub", "cpu_sub", "wire_per_unit", "p99_lb", "delivered_pct"):
                    if loss == 0:
                        spans = [max(v) - min(v) for v in (fin(x[metric] for x in arms.get(a, [])) for a in VENDORS)
                                 if v]
                        floor = max(spans) if spans else 0.0
                    else:
                        h, a = fin(x[metric] for x in arms.get("tickle@head", [])), \
                            fin(x[metric] for x in arms.get("tickle@aa", []))
                        floor = abs(statistics.mean(a) - statistics.mean(h)) if h and a else 0.0
                    word, d, se = compare(on, off, metric, floor)
                    if (cell, on_arm) in lose and metric == "delivered_pct":
                        word = "WORSE" if (cell, off_arm) not in lose else word
                    if word is None:
                        print(f"OFFLOAD {topic} l{loss} {on_arm} vs {off_arm} {metric}: NO VERDICT")
                        incomplete = True
                        continue
                    print(f"OFFLOAD {topic} l{loss} {on_arm} vs {off_arm} {metric}: {word} d {d:+.3f} SE {se:.3f} "
                          f"floor {floor:.3f} ({where})")
                    (applies if where == "applies" else elsewhere).append((on_arm, metric, word))
    if incomplete:
        print("OFFLOAD DECISION: NO VERDICT")
        return 2
    pairs = {a for a, _m, _w in applies}
    if any(w == "WORSE" for _a, _m, w in applies):
        decision = "DROP - WORSE where it applies"
    elif len(pairs) == 2 and all(
            any(m in ("cpu_pub", "cpu_sub") and w == "BETTER" for a2, m, w in applies if a2 == a) for a in pairs) \
            and not any(w == "WORSE" for _a, _m, w in elsewhere):
        decision = "KEEP-ON"
    elif len(pairs) < 2:
        print("OFFLOAD DECISION: NO VERDICT (a comparison where it applies is missing)")
        return 2
    else:
        decision = "DEFAULT-OFF - no CPU gain where it applies, or a WORSE in a 0% / 0.01% AUTO cell"
    print(f"OFFLOAD DECISION: {decision}")
    return 0


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--offload":
        return offload_decision([Path(x) for x in sys.argv[2:]])
    if len(sys.argv) != 3 or sys.argv[1] not in ("--zero", "--loss"):
        print(__doc__)
        return 1
    zero = sys.argv[1] == "--zero"
    got = read_invocation(Path(sys.argv[2]))
    if isinstance(got, int):
        return got
    _meta, usable, lose = got
    runs = Path(f"{sys.argv[2]}.runs")
    incomplete = False
    verdicts = []
    for cell, arms in sorted(usable.items()):
        topic, loss = cell
        if zero:
            head = arms.get("tickle@head", [])
            ok_cell = (cell, "tickle@head") not in lose
            for metric in ("cpu_pub", "cpu_sub"):
                h = fin(x[metric] for x in head)
                for v in VENDORS:
                    o = fin(x[metric] for x in arms.get(v, []))
                    if len(h) < 3 or len(o) < 3:
                        print(f"ZERO {topic} {metric} vs {v}: NO VERDICT ({len(h)} head, {len(o)} {v} usable runs)")
                        incomplete = True
                        ok_cell = False
                        continue
                    res = range_rule(h, o)
                    ok_cell = ok_cell and res != "LOSE"
                    print(f"ZERO {topic} {metric} vs {v}: {res} - head {statistics.mean(h):.3f} "
                          f"[{min(h):.3f}..{max(h):.3f}] {v} {statistics.mean(o):.3f} [{min(o):.3f}..{max(o):.3f}]")
            drift = {}
            for metric in ("cpu_pub", "cpu_sub", "lat_med", "p99_lb", "rss_sub"):
                spans = [max(v) - min(v) for v in (fin(x[metric] for x in arms.get(a, [])) for a in VENDORS) if v]
                drift[metric] = max(spans) if spans else 0.0
                h = fin(x[metric] for x in head)
                p = fin(x[metric] for x in arms.get("tickle@pre", []))
                if len(h) >= 3 and len(p) >= 3:
                    d, se = welch(h, p)
                    word = "HELD"
                    if d < -2 * se and -d > drift[metric]:
                        word = "BETTER"
                    elif d > 2 * se and d > drift[metric]:
                        word = "WORSE"
                    print(f"  head vs pre {metric}: {word} d {d:+.3f} SE {se:.3f} drift {drift[metric]:.3f}")
            verdicts.append(f"{topic} l{loss}: {'PASS' if ok_cell else 'FAIL'}")
            continue
        # loss cells
        head, aa = arms.get("tickle@head", []), arms.get("tickle@aa", [])
        results = {}
        for metric in ("wire_per_unit", "p99_lb"):
            h, a = fin(x[metric] for x in head), fin(x[metric] for x in aa)
            if len(h) < 3 or len(a) < 3:
                print(f"LOSS {topic} l{loss} {metric}: NO VERDICT ({len(h)} head, {len(a)} aa usable runs)")
                incomplete = True
                continue
            floor = abs(statistics.mean(a) - statistics.mean(h))
            for forced in ("tickle@mtu", "tickle@ip"):
                f = fin(x[metric] for x in arms.get(forced, []))
                if len(f) < 3:
                    print(f"LOSS {topic} l{loss} {metric} vs {forced}: NO VERDICT ({len(f)} usable runs)")
                    incomplete = True
                    continue
                d, se = welch(h, f)
                res = "DRAW"
                if d < -2 * se and -d > floor:
                    res = "WIN"
                elif d > 2 * se and d > floor:
                    res = "LOSE"
                results[(metric, forced)] = res
                print(f"LOSS {topic} l{loss} {metric} AUTO vs {forced}: {res} - d {d:+.3f} SE {se:.3f} "
                      f"floor {floor:.3f} (AUTO {statistics.mean(h):.3f}, {forced} {statistics.mean(f):.3f})")
        hists = [treatment(runs, x)[1] for x in head]
        print(f"  AUTO's realized n at l{loss}: " + "; ".join(s.split(' reader')[0] for s in hists))
        want_mtu = ("DRAW", "WIN") if float(loss) >= 1 else ("WIN",)
        complete = len(results) == 4
        ok = complete and (cell, "tickle@head") not in lose and all(
            results[(m, "tickle@mtu")] in want_mtu and results[(m, "tickle@ip")] == "WIN"
            for m in ("wire_per_unit", "p99_lb"))
        verdicts.append(f"{topic} l{loss}: {'PASS' if ok else ('FAIL' if complete else 'NO VERDICT')}")
        fals = []
        if float(loss) < 1 and any(results.get((m, "tickle@mtu")) == "LOSE" for m in ("wire_per_unit", "p99_lb")):
            fals.append("AUTO loses to forced MTU")
        if results.get(("p99_lb", "tickle@mtu")) == "LOSE":
            fals.append("AUTO's p99 above forced MTU's beyond the floor")
        if float(loss) == 0.01:
            for x in head + aa:
                _r, s_core = hybrid_of(runs / f"{stem_of(x)}_sub.log")
                if s_core is not None and s_core["collapses"] > 0:
                    fals.append(f"the cap collapsed at 0.01% ({stem_of(x)}: {s_core['collapses']})")
        if fals:
            print(f"FALSIFIED {topic} l{loss}: " + "; ".join(fals))
    for v in verdicts:
        print(f"CRITERION {v}")
    if incomplete or not verdicts:
        print("READING: INCOMPLETE (NO VERDICT above)")
        return 2
    print("READING: COMPLETE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
