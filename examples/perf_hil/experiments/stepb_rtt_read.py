#!/usr/bin/env python3
"""Reader of stepb_rtt_rig.sh: large-message step B, same host, by round trip and in-process CPU windows.

Usage:
    stepb_rtt_read.py OUT          read OUT.runs/ and print the verdicts (exit 0 PASS, 1 FAIL, 2 VOID)
    stepb_rtt_read.py --runs OUT   also print one line per run
    stepb_rtt_read.py --prereg     print the reading rules this file enforces
    stepb_rtt_read.py --selftest   check the rules against synthetic sessions whose verdict is known

The rules (PREREG) were written before any rig run of this harness; each one is code here, not a comment. They carry
B2's criteria (git show 228da18d:docs/DESIGN.md, "Step B pre-registration") onto an instrument with no spinning thread.
"""

import math
import os
import re
import statistics
import sys
import tempfile

# --- pre-registered constants -------------------------------------------------------------------------------------
P_MISS = 0.001
N_MIN_P50 = math.ceil(math.log(P_MISS) / math.log(0.5))  # 10: P(no reply on one side of the true median) < 0.001
N_MIN_P99 = math.ceil(math.log(P_MISS) / math.log(0.99))  # 688: P(no reply above the true p99) < 0.001
N_MIN_CPU = 100  # samples inside a process's CPU window: below it the run has no per-sample CPU figure
MIN_PAIRS = 3  # pairs (or reps) a comparison needs, or it is NO VERDICT
CONTROL_K = 3.0  # control and A/A' rules: MOVED needs |d| > 3 x SE (and the MATTER_FRAC floor)
TEST_K = 2.0  # B2's hypothesis tests: "beyond 2 x SE", as pre-registered on 2026-10-11
RESOLVE_FRAC = 0.10  # a control must resolve: 2 x SE of its difference <= 10% of the reference arm's mean
MATTER_FRAC = 0.05  # and moves only by a bias that could matter: step B's expected effect is ~-40% CPU/MB (rig, B2)
BE_MIN = 99.9  # B2.1: BEST_EFFORT delivered %, every rep
MIB = 1048576.0
ARM_A, ARM_A2, ARM_B = "tickle@A", "tickle@A2", "tickle@B"
VENDORS = ["fastdds", "cyclonedds"]
CONTROLS = ["CR", "CB"]
LARGE = ["S1", "S1BE", "S4", "S4BE"]
RELIABLE_LARGE = ["S1", "S4"]
CTRL_METRICS = ["rtt_p50", "tx_cpu", "rx_cpu"]  # CPU in us per sample
AA_GATE = {"B2.3": "cpu", "B2.4": "rtt_p50"}  # which A/A' metric gates which comparative verdict

PREREG = f"""Step B same-host round trip - reading rules (each is enforced by stepb_rtt_read.py)
Written 2026-10-20 before any rig run; amended the same day after the PC dry run (no rig run yet), which showed rep-to-
  rep drift common to every arm swamping an unpaired control on 3 reps, and step A's same-host BEST_EFFORT 4 MB cell
  delivering 0.5-5%: hence pairing within a rep, controls opening and closing every rep, and a short CPU window voiding
  the CPU figure only.
Measured per run (large_rtt_tx/large_rtt_rx, both on one host): RTT = Ack arrival - the sample's due time, on the
  sender's clock; p50 (from n >= {N_MIN_P50} replies) and p99 (from n >= {N_MIN_P99}) by nearest rank. CPU: each process's
  CLOCK_PROCESS_CPUTIME_ID over its window (sender: first due time to the last publish's return, per published sample;
  receiver: first scheduled callback to the last, per sample taken after the first), in us per sample; for large cells
  also ms per MiB. "cpu" = tx + rx per sample. Peak RSS of each process (ru_maxrss). Delivered % = answered / scheduled.
Sample counts first: the COUNTS block prints, per cell and arm, the valid runs and each run's n (answered) and k
  (receiver window samples), before any verdict.
VOID run (listed, never averaged in): the driver's identity checks failed (status != ok); the samples file does not hold
  exactly the scheduled count; the RESULT or RX line lacks a CPU field; the sender published fewer than scheduled in its
  window; n < {N_MIN_P50} unless the run is a delivery failure; the treatment is not the arm's (below); or a treatment
  read could not look at a process (gone) - "could not look" is not "no region".
  A receiver window below {N_MIN_CPU} samples leaves the run valid for delivery and latency, without a CPU figure.
  A run the driver marks nodelivery (matched, identity checked, not one warm-up sample answered in 20 s) is valid and
  delivered 0% of the scheduled samples - not delivering is the finding (step A, BEST_EFFORT 4 MB, on the PC).
Treatment (/proc/PID/maps read twice in the schedule): tickle@B in a large cell maps a /dev/shm/tickle-large-* region in
  BOTH processes (the receiver maps it only on taking a descriptor); every other tickle run - tickle@A, tickle@A2, and
  every tickle arm in a control cell - maps none in either. Vendors are not read for it.
Comparisons between tickle arms are PAIRED: the two runs of one rep and slot (the arms run back to back), d_i = the
  difference per pair, d = mean(d_i), SE = sd(d_i) / sqrt(pairs); >= {MIN_PAIRS} pairs or NO VERDICT. Against vendors,
  unpaired: d = difference of means, SE = sqrt(SE_a^2 + SE_b^2), >= {MIN_PAIRS} valid runs each.
CONTROL (the change cannot touch it: Ack cells {CONTROLS}, run at the opening and the close of every rep, where tickle@A
  and tickle@B execute the same code because rmw_tickle_large_region_options() returns before it reads
  RMW_TICKLE_LARGE_REGION): for each control cell and each of {CTRL_METRICS}, d = B - A paired.
  MOVED if |d| > {CONTROL_K:g} x SE AND |d| > {MATTER_FRAC:.0%} of mean(A) (a bias below {MATTER_FRAC:.0%} cannot make the ~40% CPU/MB
  effect B2 expects; six tests at 3 x SE alone would void about one true-null session in ten);
  CANNOT RESOLVE if 2 x SE > the larger of {RESOLVE_FRAC:.0%} of mean(A) and half the smallest relative |B - A| the session
  reads in that metric (paired, over the large cells): a control's floor must lie under every effect it licenses, and a
  control that holds by being noisy decides nothing (perf_test's re-run held at 2 x SE 4.2 ms/MB on a +3.3 ms/MB
  difference). MOVED, CANNOT RESOLVE or NO VERDICT -> SESSION VOID.
INSTRUMENT (A/A', each large cell): tickle@A2 - tickle@A paired, cpu and rtt_p50, the MOVED rule as the control's; it
  resolves when its 2 x SE <= the larger of {RESOLVE_FRAC:.0%} of mean(A) and half the |B - A| difference it gates (the
  instrument's floor must lie under the effect being read; a null B - A needs the full {RESOLVE_FRAC:.0%}). Not HELD -> the
  comparative verdict it gates is NO VERDICT in that cell (cpu gates B2.3, rtt_p50 gates B2.4).
B2.1 BEST_EFFORT large cells: tickle@B delivered >= {BE_MIN}% in every valid run, else FAIL (step B is broken).
B2.2 RELIABLE large cells: tickle@B delivered 100% in every valid run, else FAIL (a LOSE by the incomplete-delivery rule).
  B2.1 and B2.2 need >= {MIN_PAIRS} valid tickle@B runs and nothing else.
B2.3 RELIABLE large cells: d = cpu(B) - cpu(A) paired; d < -{TEST_K:g} x SE -> PASS; otherwise FAIL, which FALSIFIES step
  B's premise: the copies were not where the same-host cost is (L3: step B dropped, the ring path stays).
  BEST_EFFORT cells: printed, not decisive (tickle@A delivers less: not like-for-like).
B2.4 every large cell: d = rtt_p50(B) - rtt_p50(A) paired; d <= {TEST_K:g} x SE -> PASS, else FAIL.
B2.6 peak RSS, tx and rx: mean(B) - mean(A) <= one sample's bytes -> ok, else LOSE (reported, not a block).
B2.5 tickle@B against each vendor, per large cell: delivered, rtt_p50, rtt_p99, tx_cpu, rx_cpu, tx_rss, rx_rss:
  WIN / DRAW / LOSE by {TEST_K:g} x SE (higher is better for delivered, lower otherwise). tickle@B failing B2.1/B2.2 is
  LOSE on delivered whatever the vendor; a vendor failing delivery is excluded from that cell and listed.
  The design's second falsifier: in S4, tickle@B's rx_cpu not below fastdds's by {TEST_K:g} x SE -> "DESIGN FALSIFIER S4":
  the remaining cost is rmw's decode, and the next step is rmw loans, not core.
SESSION: VOID (exit 2) if the control is not HELD, or one of B2.1-B2.4 has no verdict in every cell it covers; FAIL
  (exit 1) if any of B2.1-B2.4 fails; PASS (exit 0) otherwise, with every NO VERDICT listed."""


def nearest_rank(values, q):
    if not values:
        return None
    s = sorted(values)
    return s[min(max(math.ceil(q * len(s)) - 1, 0), len(s) - 1)]


KV = re.compile(r"(\w+)=(\S+)")


def kv(line):
    return dict(KV.findall(line))


class Run:
    def __init__(self, idx, tx_line, rx_line, samples):
        """idx: the index.txt fields; tx_line/rx_line: RESULT/RX lines (or None); samples: [(due, reply)] or None."""
        self.rep = int(idx["rep"])
        self.slot = int(idx.get("slot", "1"))
        self.cell = idx["cell"]
        self.arm = idx["arm"]
        self.qos = idx["qos"]
        self.bytes = int(idx["bytes"])
        self.scheduled = int(idx["rate"]) * int(idx["dur"])
        self.status = idx.get("status", "ok")
        self.rtts = []
        self.sent = 0
        tx = kv(tx_line) if tx_line else {}
        rx = kv(rx_line) if rx_line else {}
        if self.status == "nodelivery":
            samples = [(0, 0)] * self.scheduled  # every scheduled sample unanswered
        if samples is not None:
            self.sent = len(samples)
            self.rtts = [(r - d) / 1e6 for d, r in samples if r != 0]
        self.n = len(self.rtts)
        self.delivered = 100.0 * self.n / self.sent if self.sent else 0.0
        self.fields = True
        try:
            self.tx_k = int(tx["cpu_window_samples"])
            self.rx_k = int(rx["cpu_window_takes"])
            tx_ms, rx_ms = float(tx["cpu_window_ms"]), float(rx["cpu_window_ms"])
            self.tx_rss = int(tx["maxrss_kb"])
            self.rx_rss = int(rx["maxrss_kb"])
        except (KeyError, ValueError):
            self.fields = False
            self.tx_k = self.rx_k = 0
            tx_ms = rx_ms = 0.0
            self.tx_rss = self.rx_rss = None
        self.tx_cpu = 1000.0 * tx_ms / self.tx_k if self.tx_k >= N_MIN_CPU else None
        self.rx_cpu = 1000.0 * rx_ms / self.rx_k if self.rx_k >= N_MIN_CPU else None
        self.cpu = self.tx_cpu + self.rx_cpu if self.tx_cpu is not None and self.rx_cpu is not None else None
        self.rtt_p50 = nearest_rank(self.rtts, 0.50) if self.n >= N_MIN_P50 else None
        self.rtt_p99 = nearest_rank(self.rtts, 0.99) if self.n >= N_MIN_P99 else None
        self.void = self._void(idx)

    def delivery_failed(self):
        if self.qos == "reliable":
            return self.n < self.sent
        return self.delivered < BE_MIN

    def _void(self, idx):
        if self.status not in ("ok", "nodelivery"):
            return self.status
        if self.status == "nodelivery":
            return self._treatment(idx)
        if self.sent != self.scheduled:
            return f"VOID:samples {self.sent} != scheduled {self.scheduled}"
        if not self.fields:
            return "VOID:no CPU fields in RESULT/RX"
        if self.tx_k != self.scheduled:
            return f"VOID:sender window {self.tx_k} != scheduled {self.scheduled}"
        if self.n < N_MIN_P50 and not self.delivery_failed():
            return f"VOID:n={self.n} < {N_MIN_P50}"
        return self._treatment(idx)

    def _treatment(self, idx):
        if self.arm.startswith("tickle@"):
            if idx.get("tx_looked") != "2" or idx.get("rx_looked") != "2":
                return f"VOID:treatment not seen (looked tx {idx.get('tx_looked')}/2 rx {idx.get('rx_looked')}/2)"
            want = "1" if (self.arm == ARM_B and self.cell in LARGE) else "0"
            got = (idx.get("tx_region"), idx.get("rx_region"))
            if got != (want, want):
                return f"VOID:treatment tx_region={got[0]} rx_region={got[1]}, {self.arm} in {self.cell} wants {want}"
        return None

    def value(self, metric):
        return getattr(self, metric)


def load(out):
    runs_dir = out + ".runs"
    runs = []
    with open(os.path.join(runs_dir, "index.txt")) as f:
        for line in f:
            if not line.startswith("RUN "):
                continue
            idx = kv(line)
            stem = os.path.join(runs_dir, idx["stem"])
            tx_line = rx_line = samples = None
            try:
                with open(stem + "_tx.log") as g:
                    tx_line = next((x for x in g if x.startswith("RESULT: ")), None)
            except OSError:
                pass
            try:
                with open(stem + "_rx.log") as g:
                    rx_line = next((x for x in g if x.startswith("RX: ")), None)
            except OSError:
                pass
            try:
                samples = []
                with open(stem + "_tx.samples") as g:
                    for x in g:
                        if x.startswith("#") or not x.strip():
                            continue
                        p = x.split()
                        samples.append((int(p[1]), int(p[4])))
            except (OSError, IndexError, ValueError):
                samples = None
            runs.append(Run(idx, tx_line, rx_line, samples))
    return runs


def mean_se(xs):
    m = statistics.fmean(xs)
    return m, (statistics.stdev(xs) / math.sqrt(len(xs)) if len(xs) > 1 else float("inf"))


def unpaired(a, b):
    """mean(b) - mean(a), SE of that difference, mean(a)."""
    (ma, sa), (mb, sb) = mean_se(a), mean_se(b)
    return mb - ma, math.sqrt(sa * sa + sb * sb), ma


class Reader:
    def __init__(self, runs, out):
        self.out = out
        self.cells = {}
        for r in runs:
            self.cells.setdefault(r.cell, {}).setdefault(r.arm, []).append(r)

    def p(self, s):
        print(s, file=self.out)

    def valid(self, cell, arm):
        return [r for r in self.cells.get(cell, {}).get(arm, []) if r.void is None]

    def vals(self, runs, metric):
        return [r.value(metric) for r in runs if r.value(metric) is not None]

    def paired(self, cell, a, b, metric):
        """(d, SE, mean(a), pairs) of b - a over the (rep, slot) pairs where both have the metric; None if too few."""
        va = {(r.rep, r.slot): r.value(metric) for r in self.valid(cell, a) if r.value(metric) is not None}
        vb = {(r.rep, r.slot): r.value(metric) for r in self.valid(cell, b) if r.value(metric) is not None}
        keys = sorted(set(va) & set(vb))
        if len(keys) < MIN_PAIRS:
            return None, len(keys)
        ds = [vb[k] - va[k] for k in keys]
        d, se = mean_se(ds)
        return (d, se, statistics.fmean(va[k] for k in keys), len(keys)), len(keys)

    def counts(self):
        self.p("COUNTS (valid runs; per run r<rep>.<slot>: n = answered, k = receiver window samples):")
        for cell in sorted(self.cells):
            for arm in sorted(self.cells[cell]):
                runs = sorted(self.cells[cell][arm], key=lambda r: (r.rep, r.slot))
                v = [r for r in runs if r.void is None]
                each = " ".join(f"r{r.rep}.{r.slot}:n={r.n},k={r.rx_k}" for r in v)
                self.p(f"  {cell:5} {arm:11} {len(v)}/{len(runs)} valid  {each}")
                for r in runs:
                    if r.void is not None:
                        self.p(f"  {cell:5} {arm:11} r{r.rep}.{r.slot} {r.void}")

    def held(self, cell, a, b, metric, label, effect=0.0):
        """The control rules on b - a, paired: 'HELD', 'MOVED', 'CANNOT RESOLVE' or 'NO VERDICT'. effect: the |B - A|
        an instrument check gates (0 for the control)."""
        res, n = self.paired(cell, a, b, metric)
        if res is None:
            self.p(f"{label} {cell} {metric}: NO VERDICT ({n} pairs of {a} and {b})")
            return "NO VERDICT"
        d, se, ma, n = res
        moved = abs(d) > CONTROL_K * se and abs(d) > MATTER_FRAC * abs(ma)
        unresolved = 2 * se > max(RESOLVE_FRAC * abs(ma), 0.5 * abs(effect))
        word = "MOVED" if moved else ("CANNOT RESOLVE" if unresolved else "HELD")
        rel = 2 * se / abs(ma) * 100 if ma else float("inf")
        self.p(f"{label} {cell} {metric}: {b} - {a} = {d:+.3f} on {ma:.3f}, {n} pairs ({CONTROL_K:g} x SE "
               f"{CONTROL_K * se:.3f}; 2 x SE {rel:.1f}% of mean) -> {word}")
        return word

    def read(self):
        self.counts()
        words = []
        for m in CTRL_METRICS:
            rel = [abs(r[0]) / abs(r[2]) for r in (self.paired(c, ARM_A, ARM_B, m)[0] for c in LARGE) if r and r[2]]
            smallest = min(rel, default=0.0)
            for c in CONTROLS:
                res, _ = self.paired(c, ARM_A, ARM_B, m)
                words.append(self.held(c, ARM_A, ARM_B, m, "CONTROL", smallest * abs(res[2]) if res else 0.0))
        if any(w != "HELD" for w in words):
            self.p("SESSION: VOID - the control moved, could not resolve, or could not be read")
            return 2
        fails, no_verdict = [], []
        covered = {"B2.1": 0, "B2.2": 0, "B2.3": 0, "B2.4": 0}
        decided = dict.fromkeys(covered, 0)
        for cell in LARGE:
            if cell not in self.cells:
                continue
            reliable = cell in RELIABLE_LARGE
            for c in (["B2.2", "B2.3", "B2.4"] if reliable else ["B2.1", "B2.4"]):
                covered[c] += 1
            b = self.valid(cell, ARM_B)
            nb = self.bytes_of(cell)
            mb = nb / MIB
            # B2.1 / B2.2: absolute, tickle@B alone.
            if len(b) < MIN_PAIRS:
                self.p(f"B2.{2 if reliable else 1} {cell}: NO VERDICT ({len(b)} valid {ARM_B} runs)")
                no_verdict.append(f"B2.{2 if reliable else 1} {cell}")
            elif reliable:
                bad = [f"r{r.rep}" for r in b if r.n < r.sent]
                w = "PASS" if not bad else f"FAIL ({', '.join(bad)} incomplete)"
                self.p(f"B2.2 {cell}: {ARM_B} delivered min {min(r.delivered for r in b):.2f}% (needs 100%) -> {w}")
                decided["B2.2"] += 1
                if bad:
                    fails.append(f"B2.2 {cell}")
            else:
                worst = min(r.delivered for r in b)
                w = "PASS" if worst >= BE_MIN else "FAIL - step B is broken"
                self.p(f"B2.1 {cell}: {ARM_B} delivered min {worst:.2f}% (needs >= {BE_MIN}%) -> {w}")
                decided["B2.1"] += 1
                if worst < BE_MIN:
                    fails.append(f"B2.1 {cell}")
            # Comparatives, each gated by the A/A' of its metric.
            gate = {}
            for c, m in AA_GATE.items():
                eff, _ = self.paired(cell, ARM_A, ARM_B, m)
                gate[c] = self.held(cell, ARM_A, ARM_A2, m, "INSTRUMENT", eff[0] if eff else 0.0)
            res, n = self.paired(cell, ARM_A, ARM_B, "cpu")
            amin = min((r.delivered for r in self.valid(cell, ARM_A)), default=0.0)
            if res is None:
                self.p(f"B2.3 {cell}: NO VERDICT ({n} pairs with a CPU figure)")
                if reliable:
                    no_verdict.append(f"B2.3 {cell}")
            else:
                d, se, ma, n = res
                line = (f"cpu/sample {ma:.1f} -> {ma + d:.1f} us ({ma / 1000 / mb:.3f} -> {(ma + d) / 1000 / mb:.3f} "
                        f"ms/MiB; {d:+.1f}, {TEST_K:g} x SE {TEST_K * se:.1f}, {n} pairs)")
                if not reliable:
                    self.p(f"     {cell}: {line}; {ARM_A} delivered min {amin:.2f}%: reported, not decisive")
                elif gate["B2.3"] != "HELD":
                    self.p(f"B2.3 {cell}: {line} -> NO VERDICT (instrument {gate['B2.3']})")
                    no_verdict.append(f"B2.3 {cell}")
                else:
                    ok = d < -TEST_K * se
                    self.p(f"B2.3 {cell}: {line} -> "
                           f"{'PASS' if ok else 'FAIL - FALSIFIES step B (L3: dropped, the ring path stays)'}")
                    decided["B2.3"] += 1
                    if not ok:
                        fails.append(f"B2.3 {cell}")
            res, n = self.paired(cell, ARM_A, ARM_B, "rtt_p50")
            if res is None or gate["B2.4"] != "HELD":
                why = f"{n} pairs" if res is None else f"instrument {gate['B2.4']}"
                self.p(f"B2.4 {cell}: NO VERDICT ({why})")
                no_verdict.append(f"B2.4 {cell}")
            else:
                d, se, ma, n = res
                ok = d <= TEST_K * se
                self.p(f"B2.4 {cell}: rtt p50 {ma:.3f} -> {ma + d:.3f} ms ({d:+.3f}, {TEST_K:g} x SE {TEST_K * se:.3f}, "
                       f"{n} pairs) -> {'PASS' if ok else 'FAIL'}")
                decided["B2.4"] += 1
                if not ok:
                    fails.append(f"B2.4 {cell}")
            a = self.valid(cell, ARM_A)
            for side in ("tx_rss", "rx_rss"):
                xa, xb = self.vals(a, side), self.vals(b, side)
                if len(xa) < MIN_PAIRS or len(xb) < MIN_PAIRS:
                    self.p(f"B2.6 {cell} {side}: n/a")
                    continue
                d, _, ma = unpaired(xa, xb)
                self.p(f"B2.6 {cell} {side}: {ma:.0f} -> {ma + d:.0f} kB ({d:+.0f}; one sample {nb / 1024:.0f} kB) -> "
                       f"{'ok' if d <= nb / 1024 else 'LOSE'}")
            if len(b) >= MIN_PAIRS:
                self.vendors(cell, b)
        for c, n in covered.items():
            if n and not decided[c]:
                self.p(f"SESSION: VOID - {c} has no verdict in any cell it covers")
                return 2
        nv = f"; NO VERDICT: {', '.join(no_verdict)}" if no_verdict else ""
        if fails:
            self.p(f"SESSION: FAIL - {', '.join(fails)}{nv}")
            return 1
        self.p(f"SESSION: PASS{nv}")
        return 0

    def bytes_of(self, cell):
        return next(r.bytes for runs in self.cells[cell].values() for r in runs)

    def vendors(self, cell, b):
        ours_failed = any(r.delivery_failed() for r in b)
        for v in VENDORS:
            vr = self.valid(cell, v)
            if len(vr) < MIN_PAIRS:
                self.p(f"B2.5 {cell} vs {v}: NO VERDICT ({len(vr)} valid runs)")
                continue
            if any(r.delivery_failed() for r in vr) and not ours_failed:
                self.p(f"B2.5 {cell} vs {v}: {v} DELIVERY FAILED (min {min(r.delivered for r in vr):.1f}%) - "
                       f"excluded from the cell and listed")
                continue
            words = []
            for m in ("delivered", "rtt_p50", "rtt_p99", "tx_cpu", "rx_cpu", "tx_rss", "rx_rss"):
                if m == "delivered" and ours_failed:
                    words.append(f"{m} LOSE")
                    continue
                xb, xv = self.vals(b, m), self.vals(vr, m)
                if len(xb) < MIN_PAIRS or len(xv) < MIN_PAIRS:
                    words.append(f"{m} n/a")
                    continue
                d, se, _ = unpaired(xv, xb)  # ours - theirs
                if abs(d) <= TEST_K * se:
                    words.append(f"{m} DRAW")
                else:
                    better = d > 0 if m == "delivered" else d < 0
                    words.append(f"{m} {'WIN' if better else 'LOSE'}")
                if cell == "S4" and v == "fastdds" and m == "rx_cpu" and not d < -TEST_K * se:
                    self.p(f"DESIGN FALSIFIER S4: {ARM_B} rx_cpu not below fastdds ({d:+.1f} us, {TEST_K:g} x SE "
                           f"{TEST_K * se:.1f}): the remaining cost is rmw's decode; next step rmw loans, not core")
            self.p(f"B2.5 {cell} vs {v}: {', '.join(words)}")


def print_runs(runs, out):
    for r in sorted(runs, key=lambda r: (r.cell, r.arm, r.rep, r.slot)):
        print(f"run {r.cell:5} {r.arm:11} r{r.rep}.{r.slot} n={r.n}/{r.sent} rtt_p50={r.rtt_p50} tx_cpu={r.tx_cpu} "
              f"rx_cpu={r.rx_cpu} k={r.tx_k}/{r.rx_k} rss={r.tx_rss}/{r.rx_rss} {r.void or 'ok'}", file=out)


# --- selftest -----------------------------------------------------------------------------------------------------
CELL_DEF = {"CR": ("reliable", 8), "CB": ("best_effort", 8), "S1": ("reliable", 1048576),
            "S1BE": ("best_effort", 1048576), "S4": ("reliable", 4194304), "S4BE": ("best_effort", 4194304)}
BASE = {"CR": (0.20, 60.0, 70.0), "CB": (0.20, 60.0, 70.0), "S1": (1.40, 900.0, 1900.0),
        "S1BE": (1.40, 900.0, 1900.0), "S4": (6.0, 3000.0, 6000.0), "S4BE": (6.0, 3000.0, 6000.0)}


def synth(root, effects, reps=5, scheduled=900, region=None, missing=None, noise=0.01, drift=0.0, spin=0.0,
          nodeliv=()):
    """A synthetic session: effects[(cell, arm)] = (rtt factor, tx cpu factor, rx cpu factor, lost samples).
    noise: per-run relative jitter; drift: per-rep relative shift shared by every arm in that rep (the PC dry run's);
    spin: the control cells' receiver CPU gains a uniform 0..spin share per run (a benchmark thread that busy-waits, as
    perf_test's AnalyzeRunner); nodeliv: (cell, arm, rep) runs the driver marked nodelivery."""
    import random
    rnd = random.Random(7)
    out = os.path.join(root, "s")
    runs_dir = out + ".runs"
    os.makedirs(runs_dir)
    with open(os.path.join(runs_dir, "index.txt"), "w") as idx:
        for rep in range(1, reps + 1):
            shift = 1 + rnd.gauss(0, drift)
            plan = [(c, 1, [ARM_A, ARM_B]) for c in CONTROLS] + [(c, 1, [ARM_A, ARM_A2, ARM_B] + VENDORS) for c in LARGE]
            plan += [(c, 2, [ARM_A, ARM_B]) for c in CONTROLS]
            for cell, slot, arms in plan:
                qos, nbytes = CELL_DEF[cell]
                for arm in arms:
                    f_rtt, f_tx, f_rx, lost = effects.get((cell, arm), (1.0, 1.0, 1.0, 0))
                    rtt, tx, rx = BASE[cell]

                    def jit(x):
                        return x * shift * (1 + rnd.gauss(0, noise))

                    stem = f"r{rep}s{slot}_{cell}_{arm.replace('@', '_')}"
                    status = "nodelivery" if (cell, arm, rep) in nodeliv else "ok"
                    spun = 1 + rnd.uniform(0, spin) if cell in CONTROLS else 1.0
                    want = 1 if (arm == ARM_B and cell in LARGE) else 0
                    if region is not None and (cell, arm, rep) in region:
                        want = region[(cell, arm, rep)]
                    looked = 1 if missing is not None and (cell, arm, rep) in missing else 2
                    idx.write(f"RUN rep={rep} slot={slot} cell={cell} arm={arm} stem={stem} status={status} msg=x qos={qos} "
                              f"depth=10 rate=30 dur={scheduled // 30} bytes={nbytes} tx_region={want} "
                              f"rx_region={want} tx_looked={looked} rx_looked={looked}\n")
                    r_med = jit(rtt * f_rtt)
                    if status == "nodelivery":
                        with open(os.path.join(runs_dir, stem + "_rx.log"), "w") as s:
                            s.write("RX: framework=x cpu_window_ms=0.000 cpu_window_takes=0 maxrss_kb=21000\n")
                        continue
                    with open(os.path.join(runs_dir, stem + "_tx.samples"), "w") as s:
                        s.write("# seq due pub_start pub_return reply\n")
                        for k in range(1, scheduled + 1):
                            due = k * 33_333_333
                            reply = 0 if k <= lost else due + int(r_med * 1e6 * (1 + 0.05 * rnd.random()))
                            s.write(f"{k} {due} {due} {due} {reply}\n")
                    with open(os.path.join(runs_dir, stem + "_tx.log"), "w") as s:
                        s.write(f"RESULT: framework=x sent={scheduled} cpu_window_ms={jit(tx * f_tx) * scheduled / 1000:.3f} "
                                f"cpu_window_samples={scheduled} maxrss_kb=20000\n")
                    with open(os.path.join(runs_dir, stem + "_rx.log"), "w") as s:
                        k = max(scheduled - lost - 1, 0)
                        s.write(f"RX: framework=x cpu_window_ms={jit(rx * f_rx) * spun * k / 1000:.3f} "
                                f"cpu_window_takes={k} maxrss_kb=21000\n")
    return out


def run_reader(out):
    import io
    buf = io.StringIO()
    rc = Reader(load(out), buf).read()
    return rc, buf.getvalue()


def selftest():
    good = {(cell, ARM_B): (0.45, 0.7, 0.55, 0) for cell in LARGE}
    good[("S4", "fastdds")] = (1.2, 1.1, 1.3, 0)
    cases = []

    def case(name, want_rc, want_text, **kw):
        with tempfile.TemporaryDirectory() as d:
            rc, text = run_reader(synth(d, **kw))
        ok = rc == want_rc and all(t in text for t in want_text)
        cases.append(ok)
        print(f"selftest {'ok  ' if ok else 'FAIL'} {name}: rc {rc} (want {want_rc})")
        if not ok:
            print(text)

    case("step B as hoped", 0, ["COUNTS", "CONTROL CR rx_cpu", "10 pairs", "-> HELD", "B2.3 S4", "SESSION: PASS"],
         effects=good)
    case("drift common to every arm in a rep: pairing removes it", 0, ["SESSION: PASS"], effects=good, drift=0.10)
    flat = {**good, ("S1", ARM_B): (0.45, 1.0, 1.0, 0)}
    case("no CPU gain in S1", 1, ["B2.3 S1", "FALSIFIES", "SESSION: FAIL"], effects=flat)
    case("control rx_cpu moved +20%", 2, ["CONTROL CR rx_cpu", "MOVED", "SESSION: VOID"],
         effects={**good, ("CR", ARM_B): (1.0, 1.0, 1.2, 0)})
    case("a spinner in the control's receiver", 2, ["CONTROL CR rx_cpu", "CANNOT RESOLVE", "SESSION: VOID"],
         effects=good, spin=6.0)  # perf_test: its main thread was 87% of the subscriber's CPU
    case("a small spinner under half the smallest effect", 0, ["CONTROL CR rx_cpu", "SESSION: PASS"], effects=good,
         spin=0.2)
    case("step A delivers nothing in S4BE (nodelivery)", 0,
         ["S4BE  tickle@A    5/5 valid  r1.1:n=0,k=0", "B2.1 S4BE: tickle@B delivered min 100.00%", "SESSION: PASS"],
         effects=good, nodeliv={("S4BE", ARM_A, r) for r in range(1, 6)})
    case("A/A' cpu moved in S4", 0, ["INSTRUMENT S4 cpu", "B2.3 S4", "NO VERDICT (instrument MOVED)", "B2.4 S4: rtt p50",
                                     "SESSION: PASS; NO VERDICT: B2.3 S4"],
         effects={**good, ("S4", ARM_A2): (1.0, 1.3, 1.3, 0)})
    case("RELIABLE incomplete", 1, ["B2.2 S1", "incomplete", "SESSION: FAIL"],
         effects={**good, ("S1", ARM_B): (0.45, 0.7, 0.55, 3)})
    case("BE below 99.9%", 1, ["B2.1 S4BE", "step B is broken"], effects={**good, ("S4BE", ARM_B): (0.45, 0.7, 0.55, 5)})
    case("step A delivers 2% in S4BE: B2.1 still decided", 0,
         ["B2.1 S4BE: tickle@B delivered min 100.00%", "S4BE  tickle@A    5/5 valid", "SESSION: PASS"],
         effects={**good, ("S4BE", ARM_A): (1.0, 1.0, 1.0, 882), ("S4BE", ARM_A2): (1.0, 1.0, 1.0, 885)})
    case("latency worse", 1, ["B2.4 S4", "-> FAIL"], effects={**good, ("S4", ARM_B): (1.5, 0.7, 0.55, 0)})
    noreg = {(c, ARM_B, r): 0 for c in LARGE for r in (1, 2, 3)}
    case("step B not applied in 3 of 5 reps", 2, ["VOID:treatment", "NO VERDICT", "SESSION: VOID"], effects=good,
         region=noreg)
    leak = {("CR", ARM_B, r): 1 for r in range(1, 6)}
    case("a region in the control", 2, ["VOID:treatment", "CONTROL CR", "NO VERDICT", "SESSION: VOID"], effects=good,
         region=leak)
    gone = {("S1", ARM_B, r): 1 for r in range(1, 6)}
    case("treatment could not be looked at", 0, ["VOID:treatment not seen", "B2.2 S1: NO VERDICT"], effects=good,
         missing=gone)
    case("too few samples for a CPU figure", 2, ["NO VERDICT", "SESSION: VOID"], effects=good, scheduled=60)
    case("falsifier: rx not below fastdds", 0, ["DESIGN FALSIFIER S4"],
         effects={**good, ("S4", "fastdds"): (1.0, 1.0, 0.4, 0)})
    print("SELFTEST PASS" if all(cases) else "SELFTEST FAIL")
    return 0 if all(cases) else 1


def main(argv):
    if argv[1:] == ["--prereg"]:
        print(PREREG)
        return 0
    if argv[1:] == ["--selftest"]:
        return selftest()
    args = argv[1:]
    show_runs = "--runs" in args
    args = [a for a in args if a != "--runs"]
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 64
    runs = load(args[0])
    print(PREREG.splitlines()[0])
    if show_runs:
        print_runs(runs, sys.stdout)
    return Reader(runs, sys.stdout).read()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
