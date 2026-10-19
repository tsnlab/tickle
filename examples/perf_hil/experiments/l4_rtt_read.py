#!/usr/bin/env python3
"""Reader of l4_rtt_rig.sh: the L4 large-message cells scored by round trip on the sender's own clock.

Usage:
    l4_rtt_read.py OUT            read OUT.runs/ (index.txt, meta.txt, <stem>_tx.samples) and print the verdicts
    l4_rtt_read.py --runs OUT     also print one line per run
    l4_rtt_read.py --compare BASE TREATED   per cell and arm, the change of the median round trip (the PC fault arm)
    l4_rtt_read.py --prereg       print the reading rules this file enforces
    l4_rtt_read.py --selftest     check the rules against synthetic runs whose verdict is known

The rules (PREREG below) were written before any run, and each one is code here, not a comment.
"""

import math
import os
import statistics
import sys
import tempfile

# --- pre-registered constants -------------------------------------------------------------------------------------
# A run's p50 is usable from N_MIN_P50 replies: P(no sample on one side of the true median) = 0.5^n < 0.001.
# A run's p99 is usable from N_MIN_P99 replies: P(no sample above the true p99) = 0.99^n < 0.001.
P_MISS = 0.001
N_MIN_P50 = math.ceil(math.log(P_MISS) / math.log(0.5))  # 10
N_MIN_P99 = math.ceil(math.log(P_MISS) / math.log(0.99))  # 688
MIN_REPS = 3  # an arm's cell needs this many valid reps, or it is not scored
BE_MIN_DELIVERED = 99.0  # % of scheduled samples answered, best-effort cells that do not saturate the link
SATURATING = {"I4s"}  # offers more than the link carries: delivered % of offered reported, p99 not scored (UNBOUND)
CONTROL_OF = {"reliable": "CR", "best_effort": "CB"}
CONTROLS = set(CONTROL_OF.values())
TICKLE = "tickle"
VENDORS = ["fastdds", "fastdds@mms1472", "cyclonedds"]  # L4's letters: FastDDS as shipped / FastDDS 1472 / CycloneDDS
METRICS = ["p50", "p99"]

PREREG = f"""L4 round trip - reading rules (written before any run; each is enforced by l4_rtt_read.py)
Measured: per run, RTT = reply arrival - the sample's due (creation) time, both on the sender's CLOCK_MONOTONIC; the
  reply is an 8-byte Ack through the same rmw, RELIABLE KEEP_LAST 10. Per run p50 and p99 (nearest rank).
Sample counts first: every verdict line prints each rep's answered-sample count n.
  p50 of a run is usable when n >= {N_MIN_P50} (0.5^n < {P_MISS}); p99 when n >= {N_MIN_P99} (0.99^n < {P_MISS}).
  Below {N_MIN_P50} the run is VOID; below {N_MIN_P99} its p99 is UNDERPOWERED and that arm's p99 is not scored.
VOID run: the driver's identity checks failed (wrong rmw in the RESULT/RX line or in /proc/PID/maps, for rmw_tickle
  not exactly the library built), no RESULT line or no samples file, the sender timed out, or the samples file does not
  hold exactly the scheduled count (rate x duration). A VOID run is listed and never averaged in.
DELIVERY FAILED (per arm and cell, from its valid runs): RELIABLE - any run with a sample unanswered when the sender
  stops (its drain, DRAIN s after the last scheduled sample: 5 s on the rig, i.e. 150 periods at 30 Hz);
  BEST_EFFORT cells that do not saturate the link - any run answering below {BE_MIN_DELIVERED}% of its samples;
  any cell - a run that matched, passed its identity checks and had no warm-up sample answered in 20 s (nodelivery:
  the framework does not run in that cell). No match in 30 s is VOID (a setup failure), not a delivery failure.
  rmw_tickle DELIVERY FAILED = LOSE against every vendor in that cell (the user's incomplete-delivery rule);
  a vendor DELIVERY FAILED is excluded from that cell and listed. {sorted(SATURATING)} offers more than the link
  carries: delivered % of offered is reported, not judged; its p99 is not scored (UNBOUND), its p50 is.
Reps: an arm's cell needs >= {MIN_REPS} valid reps, else it is VOID(reps) and not scored.
Control (the change cannot touch it - a small ping and a small reply, same rmw, link and session): cells CR (RELIABLE)
  and CB (BEST_EFFORT). D(metric, QoS) = the largest, over all arms, of (max - min) of that metric's per-rep values in
  the QoS's control cell - the rig's own rep-to-rep spread for a round trip. No control (a control arm with fewer than
  {MIN_REPS} valid reps) -> the QoS's cells are NO CONTROL, not scored.
Verdict per cell, metric (p50, p99) and vendor, on the per-rep values: rmw_tickle reps [tmin, tmax], vendor [vmin, vmax]:
  WIN  if tmax < vmin and vmin - tmax > D;  LOSE if tmin > vmax and tmin - vmax > D;  DRAW otherwise.
Diagnostic, not scored: per arm, median over reps of p50(large) - median over reps of p50(control of the same QoS).
Falsification: none is claimed in advance; a cell where every rmw_tickle rep is beyond D on the wrong side is a LOSE and
  is published as such."""


def nearest_rank(values, q):
    """The same percentile large_rtt_tx prints: the ceil(q * n)-th smallest value."""
    if not values:
        return None
    s = sorted(values)
    return s[min(max(math.ceil(q * len(s)) - 1, 0), len(s) - 1)]


class Run:
    """One run: identity from index.txt, numbers from its samples."""

    def __init__(self, fields, rtts_ms=None, sent=0, scheduled=0):
        self.rep = int(fields["rep"])
        self.cell = fields["cell"]
        self.arm = fields["arm"]
        self.stem = fields.get("stem", "")
        self.qos = fields["qos"]
        self.status = fields.get("status", "ok")
        self.rtts = rtts_ms or []
        self.sent = sent
        self.scheduled = scheduled
        self.void = None
        if self.status == "nodelivery":
            # Matched, identity checked, and not one warm-up sample answered in 20 s: every scheduled sample is
            # undelivered. A valid run - not delivering is the finding - and a delivery failure in any cell.
            self.sent = self.scheduled
            self.rtts = []
        elif self.status != "ok":
            self.void = self.status
        elif self.sent != self.scheduled:
            self.void = f"VOID:samples {self.sent} != scheduled {self.scheduled}"
        elif len(self.rtts) < N_MIN_P50 and not self.delivery_failed():
            self.void = f"VOID:n={len(self.rtts)} < {N_MIN_P50}"

    @property
    def n(self):
        return len(self.rtts)

    @property
    def delivered_pct(self):
        return 100.0 * self.n / self.sent if self.sent else 0.0

    def metric(self, name):
        if name == "p50":
            return nearest_rank(self.rtts, 0.50)
        return nearest_rank(self.rtts, 0.99) if self.n >= N_MIN_P99 else None

    def delivery_failed(self):
        if self.status == "nodelivery":
            return True
        if self.cell in SATURATING:
            return False
        if self.qos == "reliable":
            return self.n < self.sent
        return self.delivered_pct < BE_MIN_DELIVERED


def read_samples(path):
    """(answered RTTs in ms, rows). Rows: seq due_ns pub_start_ns pub_return_ns reply_ns."""
    rtts, rows = [], 0
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split()
            rows += 1
            due, reply = int(parts[1]), int(parts[4])
            if reply:
                rtts.append((reply - due) / 1e6)
    return rtts, rows


def load(out):
    runs = []
    with open(out + ".runs/index.txt", encoding="utf-8") as f:
        for line in f:
            if not line.startswith("RUN "):
                continue
            fields = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
            scheduled = int(fields["rate"]) * int(fields["dur"])
            path = f"{out}.runs/{fields['stem']}_tx.samples"
            rtts, rows = [], 0
            if fields["status"] == "ok" and os.path.exists(path):
                rtts, rows = read_samples(path)
            runs.append(Run(fields, rtts, rows, scheduled))
    return runs


def fmt(x):
    return "-" if x is None else f"{x:.3f}"


def arm_state(runs, cell, arm):
    """(valid runs, why not scored or None, delivery failed)."""
    valid = [r for r in runs if r.cell == cell and r.arm == arm and r.void is None]
    failed = any(r.delivery_failed() for r in valid)
    if len(valid) < MIN_REPS:
        return valid, f"VOID(reps: {len(valid)} valid < {MIN_REPS})", failed
    return valid, None, failed


def control_spread(runs, qos, arms, metric):
    """D for this QoS and metric, or (None, why)."""
    cell = CONTROL_OF[qos]
    spreads = []
    for arm in arms:
        valid, why, _ = arm_state(runs, cell, arm)
        if why:
            return None, f"NO CONTROL ({cell} {arm}: {why})"
        vals = [r.metric(metric) for r in valid]
        if any(v is None for v in vals):
            return None, f"NO CONTROL ({cell} {arm}: {metric} underpowered)"
        spreads.append(max(vals) - min(vals))
    return max(spreads), None


def verdict(tvals, vvals, d):
    tmin, tmax, vmin, vmax = min(tvals), max(tvals), min(vvals), max(vvals)
    if tmax < vmin and vmin - tmax > d:
        return "WIN"
    if tmin > vmax and tmin - vmax > d:
        return "LOSE"
    return "DRAW"


def counts(valid):
    return ",".join(str(r.n) for r in sorted(valid, key=lambda r: r.rep))


def score_cell(runs, cell, arms):
    """Lines for one cell, and {(metric, vendor): letter}."""
    lines, letters = [], {}
    qos = next(r.qos for r in runs if r.cell == cell)
    states = {a: arm_state(runs, cell, a) for a in arms}
    for a in arms:
        valid, why, failed = states[a]
        dl = [f"{r.delivered_pct:.1f}" for r in sorted(valid, key=lambda r: r.rep)]
        lines.append(f"  {cell} {a}: n per rep [{counts(valid)}] delivered % [{', '.join(dl)}]"
                     f" p50 [{', '.join(fmt(r.metric('p50')) for r in valid)}]"
                     f" p99 [{', '.join(fmt(r.metric('p99')) for r in valid)}] ms"
                     + (f" {why}" if why else "") + (" DELIVERY FAILED" if failed else ""))
    if cell in CONTROLS or TICKLE not in arms:
        return lines, letters
    tvalid, twhy, tfailed = states[TICKLE]
    for metric in METRICS:
        if cell in SATURATING and metric == "p99":
            lines.append(f"  {cell} p99: UNBOUND (the cell saturates the link), not scored")
            continue
        d, dwhy = control_spread(runs, qos, arms, metric)
        for v in [a for a in VENDORS if a in arms]:
            vvalid, vwhy, vfailed = states[v]
            head = f"  {cell} {metric} vs {v}:"
            nn = f"(n tickle [{counts(tvalid)}], {v} [{counts(vvalid)}])"
            if tfailed:
                letters[(metric, v)] = "L"
                lines.append(f"{head} LOSE - rmw_tickle DELIVERY FAILED {nn}")
                continue
            if vfailed:
                letters[(metric, v)] = "x"
                lines.append(f"{head} not scored - {v} DELIVERY FAILED (excluded, listed) {nn}")
                continue
            if twhy or vwhy:
                letters[(metric, v)] = "-"
                lines.append(f"{head} not scored - {twhy or vwhy} {nn}")
                continue
            tv = [r.metric(metric) for r in tvalid]
            vv = [r.metric(metric) for r in vvalid]
            if any(x is None for x in tv + vv):
                letters[(metric, v)] = "-"
                lines.append(f"{head} not scored - UNDERPOWERED (a rep below n={N_MIN_P99}) {nn}")
                continue
            if d is None:
                letters[(metric, v)] = "-"
                lines.append(f"{head} not scored - {dwhy} {nn}")
                continue
            w = verdict(tv, vv, d)
            letters[(metric, v)] = w[0]
            lines.append(f"{head} {w} {nn} tickle {fmt(min(tv))}-{fmt(max(tv))} ms, {v} {fmt(min(vv))}-{fmt(max(vv))} ms,"
                         f" D {fmt(d)} ms")
    return lines, letters


def diagnostic(runs, cell, arms):
    qos = next(r.qos for r in runs if r.cell == cell)
    ctl = CONTROL_OF[qos]
    parts = []
    for a in arms:
        big = [r.metric("p50") for r in runs if r.cell == cell and r.arm == a and r.void is None and r.n]
        small = [r.metric("p50") for r in runs if r.cell == ctl and r.arm == a and r.void is None and r.n]
        if big and small:
            parts.append(f"{a} {statistics.median(big) - statistics.median(small):.3f}")
    return f"  {cell} diagnostic, median p50 - median p50 of {ctl} (not scored): " + "; ".join(parts)


def report(runs, show_runs=False):
    out = []
    cells = list(dict.fromkeys(r.cell for r in runs))
    arms = list(dict.fromkeys(r.arm for r in runs))
    voids = [r for r in runs if r.void]
    out.append(f"runs: {len(runs)}, VOID: {len(voids)}; n_min p50 {N_MIN_P50}, p99 {N_MIN_P99}; min reps {MIN_REPS}")
    for r in voids:
        out.append(f"  VOID {r.stem}: {r.void}")
    if show_runs:
        for r in runs:
            out.append(f"  run {r.stem}: n={r.n}/{r.sent} delivered={r.delivered_pct:.1f}% p50={fmt(r.metric('p50'))}"
                       f" p99={fmt(r.metric('p99'))} {r.void or ''}")
    table = []
    for cell in [c for c in cells if c in CONTROLS] + [c for c in cells if c not in CONTROLS]:
        out.append(f"--- {cell} ---")
        lines, letters = score_cell(runs, cell, arms)
        out.extend(lines)
        if cell not in CONTROLS:
            out.append(diagnostic(runs, cell, arms))
            vs = [v for v in VENDORS if v in arms]
            table.append(f"  {cell:5} p50 {'/'.join(letters.get(('p50', v), '-') for v in vs)}"
                         f"  p99 {'/'.join(letters.get(('p99', v), '-') for v in vs)}")
    for qos in CONTROL_OF:
        if any(r.cell == CONTROL_OF[qos] for r in runs):
            for metric in METRICS:
                d, why = control_spread(runs, qos, arms, metric)
                out.append(f"control D({metric}, {qos}) = {fmt(d)} ms {why or ''}")
    out.append("letters (vs " + " / ".join(v for v in VENDORS if v in arms) + "): W WIN, D DRAW, L LOSE, x vendor"
               " delivery failed, - not scored")
    out.extend(table)
    return "\n".join(out)


def compare(base, treated):
    a, b = load(base), load(treated)
    lines = ["cell arm: median over reps of p50, base -> treated (delta) ms"]
    for cell in dict.fromkeys(r.cell for r in a):
        for arm in dict.fromkeys(r.arm for r in a):
            x = [r.metric("p50") for r in a if r.cell == cell and r.arm == arm and r.void is None and r.n]
            y = [r.metric("p50") for r in b if r.cell == cell and r.arm == arm and r.void is None and r.n]
            if x and y:
                mx, my = statistics.median(x), statistics.median(y)
                lines.append(f"  {cell} {arm}: {mx:.3f} -> {my:.3f} ({my - mx:+.3f}) n {len(x)}/{len(y)} reps")
    return "\n".join(lines)


# --- selftest -----------------------------------------------------------------------------------------------------
def synth(cell, arm, rep, qos, center, n=900, sent=None, status="ok", spread=0.0):
    rtts = [center + spread * ((i % 100) / 100.0) for i in range(n)]
    sent = n if sent is None else sent
    return Run({"rep": rep, "cell": cell, "arm": arm, "qos": qos, "status": status, "stem": f"r{rep}_{cell}_{arm}"},
               rtts, sent, sent)


def selftest():
    fails = []

    def check(name, got, want):
        if got != want:
            fails.append(f"{name}: got {got!r}, want {want!r}")

    arms = [TICKLE] + VENDORS
    check("n_min_p50", N_MIN_P50, 10)
    check("n_min_p99", N_MIN_P99, 688)
    check("nearest_rank", nearest_rank(list(range(1, 101)), 0.99), 99)
    base = []
    for rep in range(1, 4):
        for a in arms:
            base.append(synth("CR", a, rep, "reliable", 0.40 + 0.01 * rep))  # D = 0.02
            base.append(synth("CB", a, rep, "best_effort", 0.40 + 0.01 * rep))

    def cell(c, qos, centers, **kw):
        return [synth(c, a, rep, qos, centers[a] + 0.001 * rep, **kw.get(a, {})) for rep in range(1, 4) for a in arms]

    centers = {TICKLE: 9.0, "fastdds": 9.5, "fastdds@mms1472": 9.01, "cyclonedds": 8.0}
    runs = base + cell("I1", "reliable", centers)
    _, letters = score_cell(runs, "I1", arms)
    check("WIN", letters[("p50", "fastdds")], "W")
    check("DRAW within D", letters[("p50", "fastdds@mms1472")], "D")
    check("LOSE", letters[("p50", "cyclonedds")], "L")
    # Overlapping ranges are a DRAW even with a large mean gap.
    ov = base + [synth("I1", TICKLE, 1, "reliable", 9.0), synth("I1", TICKLE, 2, "reliable", 9.0),
                 synth("I1", TICKLE, 3, "reliable", 12.0)] + \
        [synth("I1", v, rep, "reliable", 10.0) for v in VENDORS for rep in range(1, 4)]
    _, letters = score_cell(ov, "I1", arms)
    check("overlap DRAW", letters[("p50", "fastdds")], "D")
    # rmw_tickle missing one RELIABLE reply in one rep: LOSE everywhere, whatever the latency.
    df = base + cell("I4", "reliable", {TICKLE: 1.0, "fastdds": 9.0, "fastdds@mms1472": 9.0, "cyclonedds": 9.0})
    df[-4] = synth("I4", TICKLE, 3, "reliable", 1.0, n=899, sent=900)
    _, letters = score_cell(df, "I4", arms)
    check("tickle delivery failed -> LOSE", letters[("p50", "cyclonedds")], "L")
    # A vendor failing is excluded and listed.
    vf = base + cell("I4", "reliable", {TICKLE: 1.0, "fastdds": 9.0, "fastdds@mms1472": 9.0, "cyclonedds": 9.0})
    vf[-1] = synth("I4", "cyclonedds", 3, "reliable", 9.0, n=850, sent=900)
    _, letters = score_cell(vf, "I4", arms)
    check("vendor delivery failed -> x", letters[("p50", "cyclonedds")], "x")
    check("other vendors still scored", letters[("p50", "fastdds")], "W")
    # A run that delivered nothing at all (nodelivery) is a delivery failure, not a VOID: vendor excluded, tickle LOSE.
    nd = base + cell("I1L", "reliable", {TICKLE: 9.0, "fastdds": 9.0, "fastdds@mms1472": 9.0, "cyclonedds": 9.0},
                     fastdds={"status": "nodelivery"})
    _, letters = score_cell(nd, "I1L", arms)
    check("vendor nodelivery -> x", letters[("p50", "fastdds")], "x")
    nd = base + cell("I1L", "reliable", {TICKLE: 9.0, "fastdds": 9.0, "fastdds@mms1472": 9.0, "cyclonedds": 9.0},
                     **{TICKLE: {"status": "nodelivery"}})
    _, letters = score_cell(nd, "I1L", arms)
    check("tickle nodelivery -> L", letters[("p50", "fastdds")], "L")
    check("report over a nodelivery cell", "I1L diagnostic" in report(nd), True)
    # BEST_EFFORT: 99.5% delivered is fine, 98% fails.
    be = base + cell("I1BE", "best_effort", {TICKLE: 1.0, "fastdds": 9.0, "fastdds@mms1472": 9.0, "cyclonedds": 9.0},
                     fastdds={"n": 896, "sent": 900}, cyclonedds={"n": 882, "sent": 900})
    _, letters = score_cell(be, "I1BE", arms)
    check("BE 99.6% scored", letters[("p50", "fastdds")], "W")
    check("BE 98% failed", letters[("p50", "cyclonedds")], "x")
    # p99 underpowered below 688 answered samples; p50 still scored.
    up = base + cell("I1BE", "best_effort", {TICKLE: 1.0, "fastdds": 9.0, "fastdds@mms1472": 9.0, "cyclonedds": 9.0},
                     **{TICKLE: {"n": 600, "sent": 600}})
    for r in up:
        if r.cell == "I1BE" and r.arm == TICKLE:
            r.scheduled = 600
    _, letters = score_cell(up, "I1BE", arms)
    check("p99 underpowered", letters[("p99", "fastdds")], "-")
    check("p50 at n=600 scored", letters[("p50", "fastdds")], "W")
    # Fewer than MIN_REPS valid reps: not scored. A run whose sample count differs from the schedule is VOID.
    vr = base + cell("I4", "reliable", centers)
    vr[-4] = synth("I4", TICKLE, 3, "reliable", 9.0, status="VOID:maps")
    _, letters = score_cell(vr, "I4", arms)
    check("VOID reps", letters[("p50", "fastdds")], "-")
    short = Run({"rep": 1, "cell": "I4", "arm": TICKLE, "qos": "reliable"}, [9.0] * 900, 900, 901)
    check("short samples VOID", bool(short.void), True)
    # No control: the cells of that QoS are not scored.
    nc = [r for r in base if not (r.cell == "CR" and r.arm == "cyclonedds")] + cell("I1", "reliable", centers)
    _, letters = score_cell(nc, "I1", arms)
    check("no control", letters[("p50", "fastdds")], "-")
    # I4s: the saturating cell is scored on p50 and never on p99, and short delivery is not a failure there.
    sat = base + cell("I4s", "best_effort", {TICKLE: 50.0, "fastdds": 60.0, "fastdds@mms1472": 60.0, "cyclonedds": 60.0},
                      **{TICKLE: {"n": 860, "sent": 900}})
    _, letters = score_cell(sat, "I4s", arms)
    check("I4s p50 scored", letters[("p50", "fastdds")], "W")
    check("I4s p99 unscored", ("p99", "fastdds") in letters, False)
    # The whole path: index.txt and samples files on disk, read back.
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "x")
        os.makedirs(out + ".runs")
        with open(out + ".runs/index.txt", "w", encoding="utf-8") as idx:
            for rep in range(1, 4):
                for c, qos, ctr in (("CR", "reliable", 0.4), ("I1", "reliable", 9.0)):
                    for a in arms:
                        stem = f"r{rep}_{c}_{a.replace('@', '_')}"
                        idx.write(f"RUN rep={rep} cell={c} arm={a} stem={stem} status=ok msg=m qos={qos} depth=10 "
                                  f"rate=30 loss=0 dur=30\n")
                        bump = 0.5 if a == "fastdds" else 0.0
                        with open(f"{out}.runs/{stem}_tx.samples", "w", encoding="utf-8") as s:
                            s.write("# seq due_ns pub_start_ns pub_return_ns reply_ns\n")
                            for i in range(1, 901):
                                due = 10**12 + i * 33_333_333
                                s.write(f"{i} {due} {due} {due} {due + int((ctr + bump + rep * 1e-3) * 1e6)}\n")
        text = report(load(out))
        check("disk path WIN line", "I1 p50 vs fastdds: WIN" in text, True)
        check("disk path counts", "n tickle [900,900,900]" in text, True)
    if fails:
        print("SELFTEST FAILED:\n  " + "\n  ".join(fails))
        return 1
    print("SELFTEST PASS (all synthetic verdicts as registered)")
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "--prereg":
        print(PREREG)
        return 0
    if len(argv) >= 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) == 4 and argv[1] == "--compare":
        print(compare(argv[2], argv[3]))
        return 0
    show = len(argv) >= 2 and argv[1] == "--runs"
    args = argv[2:] if show else argv[1:]
    if len(args) != 1:
        print(__doc__)
        return 64
    print(PREREG)
    print()
    print(report(load(args[0]), show_runs=show))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
