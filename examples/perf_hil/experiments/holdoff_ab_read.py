#!/usr/bin/env python3
# Copyright (c) 2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.
"""holdoff_ab_read.py OUT | --selftest - the reader of holdoff_ab.sh (reorder hold-off stage S3, 2026-11-04).

Applies the rules pre-registered in holdoff_ab.sh's header (from REORDER_HOLDOFF_DESIGN_2026-11-03 section 4) IN
CODE: the metrics (4.3), sample counts before values, the VOID rules (4.6: V-I instrument, V-T treatment, V-C A/A and
control), the gates G1-G4 and the best rule (4.4), the fixed-vs-auto premise (4.4.4), and the hypothesis "a gated
hold-off helps the jitter cells without hurting the loss cells". The constants below are the pre-registration; change
them only before a run, and say so in the harness header.

Prints the VOID runs and cells, the A/A table, a metric table per cell, one "VERDICT <arm> ..." line per arm, the
ranked list, "BEST", "AUTO_PREMISE" and "HYPOTHESIS" lines. Exit 0 once OUT is read (verdicts are in the text), 2 when
it cannot be read. --selftest builds synthetic OUTs in a temporary directory and shows every VOID rule and the best
rule firing; it exits 1 if any expectation is not met.
"""

import math
import os
import re
import shutil
import statistics
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import time_varying_read as tvr  # noqa: E402
from wifi_burst_read import kv, num, quantile, read_lines  # noqa: E402

# --- the pre-registration (holdoff_ab.sh header) ---------------------------------------------------------------------
CONTROL = "main"  # the main tree: the A/A control of off
REFERENCE = "off"  # the change's control
GATED = ("B", "D", "E", "auto")  # auto candidates, gated by decision (i)
FIXED_RE = re.compile(r"^f([1-9][0-9]*)(us|ms)$")
STAT_REORDER_CELLS = ("s_r1", "s_r6", "s_j5", "J_p2", "J_p10")  # ranked; V-T "engaged == 0" applies here
JITTER_CELLS = ("s_j5", "J_p2", "J_p10")
LOSS_CELLS = ("s_lb5", "s_lg5", "s_d20")  # G1
RF_CANDIDATES = ("s_l0_pin", "s_j0")  # G2, when measured reorder-free
QUALITATIVE_CELLS = ("s_l0",)  # harness reorder, 1-8 gaps a rep: 0 against non-0 only
VI_CELLS = STAT_REORDER_CELLS + ("s_l0",)
RANK_DC = "rel_large"  # the primary data cell (4.2); rel_small, if run, is regression only
DEADLINE_MS = {"rel_large": 200.0, "rel_small": 100.0}  # V3's deadlines (time_varying_read.py)
UNITS = {"rel_large": tvr.LARGE_UNITS_PER_SAMPLE, "rel_small": 1}
READER = {"rel_large": "sub", "rel_small": "echo"}
WRITER = {"rel_large": "pub", "rel_small": "ping"}
MIN_SAMPLES = {"rel_large": 100, "rel_small": 1000}  # a run offering or delivering fewer is VOID (4.6)
MIN_REPS = 2  # usable reps an (arm, cell) needs before any value of it is read
TOL_MIN = 0.10  # tol = max(TOL_MIN, 2 x A/A difference)
AA_VOID_MAX = 0.10  # A/A difference above this voids the metric in the cell (V-C)
AA_FLOOR = {"m1": 0.001, "p99": 1.0, "goodput": 0.01, "acknack": 10.0, "retx": 100.0}  # denominators' floors
GOODPUT_DROP_MAX = 0.01  # G1: deadline goodput >= off - max(1 point, 2 x A/A)
COUNT_SLACK = 1.0  # G3/G4: a count a rep <= off's x (1 + TOL_MIN) + this
VI_MAX = 0.05  # V-I: |nack_spurious - large_duplicate| / large_duplicate, off, reorder cells
VI_MIN_DUP = 50  # a cell with fewer pooled duplicates cannot check V-I
J_APPLIED_M1_MIN = 0.50  # V-T: off's s_j5 M1 median (T1: 98.5% with GSO)
R1_SHARE = 0.01  # s_r1's netem reorder share
R1_BAND = (0.5, 1.5)  # off's s_r1 gaps_opened per offered unit, as a multiple of R1_SHARE
M2_MIN_DROPS = 10
MIN_RANK_CELLS = 2
MIN_JITTER_CELLS = 2
TIE = 0.10  # scores within 10% of the best tie
HELP_MAX = 0.90  # the hypothesis: jitter-cell M1 ratio at most this
FIXED_SPREAD_MAX = 4.0  # 4.4.4
STUCK_TEXT = "Still waiting on reliable seq_no"
HOLDOFF_RE = re.compile(r"^HOLDOFF: mode=(\S+) policy=(\S+) value_ns=(\d+) env=(\S+)$")
RSTATS_RE = re.compile(r"^RSTATS: role=(\S+) ([a-z_]+) (.*)$")
INV_RE = re.compile(r"^(.+)\.r(\d+)\.(pin|free)$")


def med(vals):
    vals = [v for v in vals if v is not None]
    return (statistics.median(vals) if vals else None), len(vals)


def fmt(v, kind="f"):
    if v is None:
        return "n/a"
    if kind == "%":
        return f"{100.0 * v:.2f}%"
    if kind == "r":
        return f"{v:.3f}"
    return f"{v:.2f}"


# --- treatment --------------------------------------------------------------------------------------------------------


def expected_holdoff(arm):
    """(mode, policy, value_ns, env) the arm's HOLDOFF line must show; None for main (no line at all)."""
    if arm == CONTROL:
        return None
    if arm == "off":
        return ("off", "-", 0, "off")
    if arm in ("B", "D", "E"):
        return ("auto", arm, 0, arm)
    if arm == "auto":
        return ("auto", "E", 0, "auto")  # holdoff_env.h: auto is the default candidate, E
    m = FIXED_RE.match(arm)
    if m:
        return ("fixed", "-", int(m.group(1)) * (1000 if m.group(2) == "us" else 1000000), arm)
    return ("?", "?", -1, arm)


def hold_ns(arm):
    m = FIXED_RE.match(arm)
    return int(m.group(1)) * (1000 if m.group(2) == "us" else 1000000) if m else None


def treatment_void(r):
    """V-T, per run: the arm must report the treatment it names, and show it on the cells where it must act."""
    why = []
    exp = expected_holdoff(r["arm"])
    lines = r["holdoff_lines"]
    if exp is None:
        printed = [n for n, ls in lines.items() if ls]
        if printed:
            why.append(f"main printed a HOLDOFF line ({', '.join(printed)}): not the main tree")
        if r["rd"].get("holdoff") is not None:
            why.append("main printed holdoff RSTATS: not the main tree")
        return why
    for node in ("ping", "echo", "idle"):
        got = lines.get(node, [])
        if len(got) != 1:
            why.append(f"{node}: {len(got)} HOLDOFF lines, expected 1")
        elif got[0] != exp:
            why.append(f"{node}: HOLDOFF {got[0]} is not the arm's {exp}")
    ho = r["rd"].get("holdoff")
    if ho is None:
        why.append("no reader holdoff RSTATS line")
        return why
    engaged = int(num(ho, "engaged", -1))
    if r["arm"] == REFERENCE and engaged != 0:
        why.append(f"off held {engaged} gaps")
    if r["arm"] != REFERENCE and r["cell"] in STAT_REORDER_CELLS and engaged == 0:
        why.append("held no gap on a reorder cell: treatment not applied")
    return why


# --- loading ----------------------------------------------------------------------------------------------------------


def parse_rstats(lines):
    """(role, tag) -> merged key=value dict of the RSTATS lines (the hist lines left out)."""
    out = {}
    for line in lines:
        m = RSTATS_RE.match(line)
        if m:
            out.setdefault((m.group(1), m.group(2)), {}).update(kv(m.group(3)))
    return out


def augment(r, run_dir):
    """The hold-off reader's own fields: treatment lines, reader/writer stats and the metrics of 4.3."""
    dc = r["dc"]
    logs = {n: read_lines(os.path.join(run_dir, n + ".log")) for n in ("ping", "echo", "idle")}
    r["holdoff_lines"] = {}
    for n, ls in logs.items():
        r["holdoff_lines"][n] = [(m.group(1), m.group(2), int(m.group(3)), m.group(4))
                                 for m in (HOLDOFF_RE.match(x) for x in ls) if m]
    rs = parse_rstats(logs["ping"] + logs["echo"])
    rd_role, wr_role = READER[dc], WRITER[dc]
    r["rd"] = {tag: rs.get((rd_role, tag)) for tag in ("sub", "reorder", "holdoff", "acknack_tx")}
    r["wr"] = rs.get((wr_role, "pub"))
    stuck = 0
    for name in os.listdir(run_dir):
        if name.endswith((".log", ".err")):
            stuck += sum(1 for x in read_lines(os.path.join(run_dir, name)) if STUCK_TEXT in x)
    m = {"stuck": stuck}
    r["m"] = m
    if r["void"]:
        return
    for tag in ("sub", "reorder", "acknack_tx"):
        if r["rd"][tag] is None:
            r["void"].append(f"no reader RSTATS '{tag}' line")
    if r["void"]:
        return
    if r["offered"] < MIN_SAMPLES[dc] or r["delivered"] < MIN_SAMPLES[dc]:
        r["void"].append(f"samples: offered {r['offered']}, delivered {r['delivered']}, under {MIN_SAMPLES[dc]}")
        return
    rd = r["rd"]
    m["m1"] = r["copies"] / r["copy_den"] if r["copy_den"] else None
    retx = r.get("retransmitted") if dc == "rel_large" else (int(num(r["wr"], "retransmitted", -1)) if r["wr"] else -1)
    m["retx"] = retx if retx is not None and retx >= 0 else None
    drops = r["counters"].get("p", {}).get("bands", {}).get("20", (0, 0))[1]
    m["drops"] = drops
    m["m2"] = (m["retx"] / drops) if m["retx"] is not None and drops >= M2_MIN_DROPS else None
    m["delivered_share"] = min(r["delivered"], r["offered"]) / r["offered"]
    if not r["lat_void"]:
        owd = sorted(tvr.owd_ms(x) for x in r["trace"] if x[0] >= r["first"])
        m["p50"] = quantile(owd, 0.5) if owd else None
        m["p99"] = quantile(owd, 0.99) if owd else None
        m["goodput"] = len(tvr.on_time(r, DEADLINE_MS[dc])) / r["offered"]
    sub, ro, ho, ack = rd["sub"], rd["reorder"], rd["holdoff"], rd["acknack_tx"]
    m["fill"] = int(num(ro, "reorder_fill_original"))
    m["spurious"] = int(num(ro, "nack_spurious"))
    m["absorbed"] = int(num(ro, "reorder_absorbed"))
    m["gaps"] = int(num(sub, "gaps_opened"))
    m["jump_abandoned"] = int(num(sub, "jump_abandoned_seq"))
    m["abandoned"] = m["jump_abandoned"] + int(num(sub, "hb_abandoned_seq")) + int(num(sub, "retry_giveups"))
    m["acknack"] = int(num(ack, "sent"))
    m["acknack_bytes"] = int(num(ack, "bytes_sent"))
    for k in ("engaged", "released", "early", "acknack_held", "gate_opened"):
        m[k] = int(num(ho, k)) if ho is not None else None
    m["large_dup"] = int(num(r["echo"], "large_duplicate")) if dc == "rel_large" else None
    m["cpu_w"] = num(r["ping"], "cpu_ms_per_mb", -1.0)
    m["cpu_r"] = num(r["echo"], "cpu_ms_per_mb", -1.0)
    m["realized"] = tvr.realized_loss(r)
    m["p_final"] = tvr.decode_p(r["peer_p"][-1][1]) if r.get("peer_p") else None
    r["void"].extend(treatment_void(r))


def cell_name(tvarm, part):
    """A pinned run of the clean control is the 4.5 pin control s_l0_pin; everything else keeps its name."""
    return tvarm + "_pin" if part == "pin" and tvarm == "s_l0" else tvarm


def load_out(out):
    runs_dir = os.path.join(out, "runs")
    if not os.path.isdir(runs_dir):
        return None, None, []
    text = " ".join(read_lines(os.path.join(out, "provenance.txt")))
    prov = kv(text)
    quoted = re.search(r"\barms='([^']*)'", text)  # kv() stops at the first space of a quoted list
    prov["arms_list"] = quoted.group(1) if quoted else ""
    nproc = os.cpu_count() or 1
    runs, notes = [], []
    for inv in sorted(os.listdir(runs_dir)):
        m = INV_RE.match(inv)
        tv_out = os.path.join(runs_dir, inv)
        if not m or not os.path.isdir(tv_out):
            continue
        arm, rep, part = m.group(1), int(m.group(2)), m.group(3)
        summary = read_lines(os.path.join(tv_out, "summary.txt"))
        if not any(x.startswith("=== time_varying_netns ended rc=0 ") for x in summary):
            notes.append(f"{inv}: time_varying_netns did not end cleanly (its runs are read as they are)")
        dur = num(kv(" ".join(read_lines(os.path.join(tv_out, "provenance.txt")))), "dur", 30.0)
        inner = os.path.join(tv_out, "runs")
        inv_runs = []
        for name in sorted(os.listdir(inner)) if os.path.isdir(inner) else []:
            r = tvr.load_run(tv_out, name, dur, nproc)
            if r is not None:
                inv_runs.append((r, os.path.join(inner, name)))
        cv = tvr.control_void([r for r, _ in inv_runs])  # the s_l0 control of this invocation lost nothing
        for r, d in inv_runs:
            r["tvarm"], r["dc"] = r["arm"], r["cell"]
            r["arm"], r["rep"], r["part"], r["inv"] = arm, rep, part, inv
            r["cell"] = cell_name(r["tvarm"], part)
            augment(r, d)
            if cv:
                r["void"].append("invocation control: " + "; ".join(cv))
            runs.append(r)
    return runs, prov, notes


# --- the reading ------------------------------------------------------------------------------------------------------


class Reading:
    def __init__(self, runs, prov):
        self.runs = runs
        self.prov = prov or {}
        self.lines = []
        self.global_void = []
        self.cell_void = {}  # (cell, dc) -> [reasons]
        self.aa = {}  # (cell, dc, metric) -> A/A difference
        self.aa_void = {}  # (cell, dc, metric) -> reason
        self.verdict = {}  # arm -> (class, text)
        self.gates = {}  # arm -> {gate: (status, [details])}
        self.score = {}
        self.jscore = {}
        self.p99score = {}
        self.ranked = []
        self.best = None
        self.premise = None
        self.hypothesis = None
        self.rf_cells = []
        arms = self.prov.get("arms_list", "").split()
        seen = sorted({r["arm"] for r in runs})
        self.arms = arms + [a for a in seen if a not in arms] if arms else seen
        self.cells = sorted({(r["cell"], r["dc"]) for r in runs})

    def out(self, text):
        self.lines.append(text)

    def usable(self, arm, cell, dc):
        return [r for r in self.runs if r["arm"] == arm and r["cell"] == cell and r["dc"] == dc and not r["void"]]

    def value(self, arm, cell, dc, key):
        return med([r["m"].get(key) for r in self.usable(arm, cell, dc)])

    def present(self, cell, dc=None):
        return any(c == cell and (dc is None or d == dc) for c, d in self.cells)

    # V-I, V-T (cell level), V-C ------------------------------------------------------------------------------------
    def void_rules(self):
        checked = 0
        for cell in VI_CELLS:
            offs = self.usable(REFERENCE, cell, "rel_large")
            sp = sum(r["m"]["spurious"] for r in offs)
            ld = sum(r["m"]["large_dup"] for r in offs)
            if ld < VI_MIN_DUP:
                continue
            checked += 1
            diff = abs(sp - ld) / ld
            ok = diff <= VI_MAX
            self.out(f"V-I {cell}: off nack_spurious {sp} vs large_duplicate {ld}, {fmt(diff, '%')} "
                     f"({'ok' if ok else 'OVER ' + fmt(VI_MAX, '%')})")
            if not ok:
                self.global_void.append(f"V-I: {cell} nack_spurious {sp} vs large_duplicate {ld} ({fmt(diff, '%')})")
        if not checked:
            self.global_void.append(f"V-I could not be checked: no reorder cell where off has >= {VI_MIN_DUP} "
                                    "large duplicates")
        dcs = sorted({d for _, d in self.cells})
        j5, n = self.value(REFERENCE, "s_j5", "rel_large", "m1")
        if n < MIN_REPS or j5 is None:
            why = f"jitter not checked: off s_j5 rel_large has {n} usable reps"
        elif j5 < J_APPLIED_M1_MIN:
            why = f"jitter not applied: off s_j5 M1 {fmt(j5, '%')} < {fmt(J_APPLIED_M1_MIN, '%')}"
        else:
            why = None
            self.out(f"V-T jitter: off s_j5 M1 {fmt(j5, '%')} (applied)")
        if why:
            for cell in JITTER_CELLS:
                for dc in dcs:
                    self.cell_void.setdefault((cell, dc), []).append(why)
        offs = self.usable(REFERENCE, "s_r1", "rel_large")
        shares = [r["m"]["gaps"] / (r["offered"] * UNITS["rel_large"] * R1_SHARE) for r in offs]
        mr, n = med(shares)
        if self.present("s_r1"):
            if n < MIN_REPS:
                why = f"link reorder not checked: off s_r1 rel_large has {n} usable reps"
            elif not R1_BAND[0] <= mr <= R1_BAND[1]:
                why = f"link reorder not as configured: off s_r1 gaps {fmt(mr, 'r')} x the expected 1% of units"
            else:
                why = None
                self.out(f"V-T link reorder: off s_r1 gaps_opened {fmt(mr, 'r')} x 1% of offered units (applied)")
            if why:
                for dc in dcs:
                    self.cell_void.setdefault(("s_r1", dc), []).append(why)
        # V-C: the A/A, main against off, per cell and metric
        self.out("--- A/A (main vs off; |main - off| / max(off, floor); over "
                 f"{fmt(AA_VOID_MAX, '%')} voids the metric in the cell)")
        for cell, dc in self.cells:
            row = []
            for metric in AA_FLOOR:
                mm, nm = self.value(CONTROL, cell, dc, metric)
                mo, no = self.value(REFERENCE, cell, dc, metric)
                key = (cell, dc, metric)
                if nm < MIN_REPS or no < MIN_REPS or mm is None or mo is None:
                    self.aa_void[key] = f"A/A not measurable (main {nm}, off {no} usable reps)"
                    row.append(f"{metric} n/a")
                    continue
                d = abs(mm - mo) / max(abs(mo), AA_FLOOR[metric])
                self.aa[key] = d
                if d > AA_VOID_MAX:
                    self.aa_void[key] = f"A/A {fmt(d, '%')} over {fmt(AA_VOID_MAX, '%')}"
                row.append(f"{metric} {fmt(d, '%')}{'(VOID)' if d > AA_VOID_MAX else ''}")
            self.out(f"AA {cell:9} {dc:9} " + ", ".join(row))
            # M1, the primary metric, not measurable on both arms: nothing in the cell can be compared
            if "not measurable" in self.aa_void.get((cell, dc, "m1"), ""):
                self.cell_void.setdefault((cell, dc), []).append(self.aa_void[(cell, dc, "m1")])

    def tol(self, cell, dc, metric):
        return max(TOL_MIN, 2.0 * self.aa.get((cell, dc, metric), 0.0))

    # one comparison of a candidate with off --------------------------------------------------------------------------
    def compare(self, arm, cell, dc, metric, mode):
        if (cell, dc) in self.cell_void:
            return "VOID", f"{cell}/{dc}: " + "; ".join(self.cell_void[(cell, dc)])
        if (cell, dc, metric) in self.aa_void:
            return "VOID", f"{cell}/{dc} {metric}: {self.aa_void[(cell, dc, metric)]}"
        c, nc = self.value(arm, cell, dc, metric)
        o, no = self.value(REFERENCE, cell, dc, metric)
        if nc < MIN_REPS or no < MIN_REPS or c is None or o is None:
            return "VOID", f"{cell}/{dc} {metric}: usable reps {arm} {nc}, off {no} (< {MIN_REPS})"
        tol = self.tol(cell, dc, metric)
        if mode == "le":
            lim = o * (1.0 + tol)
            ok = c <= lim
            text = f"{cell}/{dc} {metric} {fmt(c)} <= off {fmt(o)} x {1 + tol:.2f} = {fmt(lim)}"
        elif mode == "both":
            room = tol * max(abs(o), AA_FLOOR[metric])
            ok = abs(c - o) <= room
            text = f"{cell}/{dc} {metric} {fmt(c)} within {fmt(room)} of off {fmt(o)}"
        else:  # goodput, absolute points
            lim = o - max(GOODPUT_DROP_MAX, 2.0 * self.aa.get((cell, dc, metric), 0.0))
            ok = c >= lim
            text = f"{cell}/{dc} goodput {fmt(c, '%')} >= {fmt(lim, '%')} (off {fmt(o, '%')})"
        return ("PASS" if ok else "FAIL"), text

    def per_rep_count(self, arm, cell, dc, key):
        rs = self.usable(arm, cell, dc)
        vals = [r["m"].get(key) for r in rs if r["m"].get(key) is not None]
        return (sum(vals) / len(vals) if vals else None), len(vals)

    def gate_results(self, arm):
        g = {}
        dcs = sorted({d for _, d in self.cells})
        # G1: loss cells, NO WORSE first
        res = []
        for cell in LOSS_CELLS:
            for dc in dcs:
                if not self.present(cell, dc):
                    res.append(("VOID", f"{cell}/{dc} not run"))
                    continue
                res.append(self.compare(arm, cell, dc, "p99", "le"))
                res.append(self.compare(arm, cell, dc, "goodput", "ge"))
        g["G1"] = (self.aggregate(res, strict=True), res)
        # G2: measured reorder-free cells
        res = []
        if not self.rf_cells:
            res.append(("VOID", "no measured reorder-free cell (off's reorder_fill_original != 0 or too few reps)"))
        for cell, dc in self.rf_cells:
            if arm in GATED:
                eng = [r["m"].get("engaged") for r in self.usable(arm, cell, dc)]
                if len(eng) < MIN_REPS or any(e is None for e in eng):
                    res.append(("VOID", f"{cell}/{dc} engaged: {len(eng)} usable reps"))
                else:
                    res.append(("PASS" if sum(eng) == 0 else "FAIL",
                                f"{cell}/{dc} engaged {sum(eng)} (decision (i): exactly 0)"))
            for metric in ("acknack", "retx", "p99"):
                res.append(self.compare(arm, cell, dc, metric, "both"))
        g["G2"] = (self.aggregate(res, strict=True), res)
        # G3, G4: every cell
        r3, r4 = [], []
        for cell, dc in self.cells:
            if (cell, dc) in self.cell_void:
                r3.append(("VOID", f"{cell}/{dc} void"))
                r4.append(("VOID", f"{cell}/{dc} void"))
                continue
            rs = self.usable(arm, cell, dc)
            os_ = self.usable(REFERENCE, cell, dc)
            if len(rs) < MIN_REPS or len(os_) < MIN_REPS:
                r3.append(("VOID", f"{cell}/{dc}: usable reps {arm} {len(rs)}, off {len(os_)}"))
                r4.append(("VOID", f"{cell}/{dc}: usable reps {arm} {len(rs)}, off {len(os_)}"))
                continue
            cmin = min(r["m"]["delivered_share"] for r in rs)
            omin = min(r["m"]["delivered_share"] for r in os_)
            ok = cmin >= 1.0 or cmin >= omin
            r3.append(("PASS" if ok else "FAIL", f"{cell}/{dc} delivered min {fmt(cmin, '%')} (off {fmt(omin, '%')})"))
            for key, out in (("stuck", r3), ("jump_abandoned", r4), ("abandoned", r4)):
                c, _ = self.per_rep_count(arm, cell, dc, key)
                o, _ = self.per_rep_count(REFERENCE, cell, dc, key)
                if c is None or o is None:
                    out.append(("VOID", f"{cell}/{dc} {key}: no value"))
                    continue
                lim = o * (1.0 + TOL_MIN) + COUNT_SLACK
                out.append(("PASS" if c <= lim else "FAIL", f"{cell}/{dc} {key} {c:.1f}/rep <= {lim:.1f}"))
        g["G3"] = (self.aggregate(r3, strict=False), r3)
        g["G4"] = (self.aggregate(r4, strict=False), r4)
        return g

    @staticmethod
    def aggregate(res, strict):
        """FAIL beats VOID (a failure seen is a failure). strict (G1, G2: every cell is required): any VOID -> VOID.
        Otherwise (G3, G4: safety nets over every cell): VOID only when nothing could be evaluated."""
        st = [s for s, _ in res]
        if "FAIL" in st:
            return "FAIL"
        if "PASS" not in st or (strict and "VOID" in st):
            return "VOID"
        return "PASS"

    def ratio_score(self, arm, cells, key):
        vals, used = [], []
        for cell in cells:
            if (cell, RANK_DC) in self.cell_void or (cell, RANK_DC, key) in self.aa_void:
                continue
            c, nc = self.value(arm, cell, RANK_DC, key)
            o, no = self.value(REFERENCE, cell, RANK_DC, key)
            if nc < MIN_REPS or no < MIN_REPS or c is None or o is None or o <= 0:
                continue
            vals.append(c / o)
            used.append(cell)
        return vals, used

    def per_cell_ratio(self, arm, cell):
        v, _ = self.ratio_score(arm, (cell,), "m1")
        return v[0] if v else None

    # everything ------------------------------------------------------------------------------------------------------
    def read(self):
        self.out(f"=== holdoff_ab_read: {len(self.runs)} runs; arms {' '.join(self.arms)}; main "
                 f"{self.prov.get('main_sha', '?')[:8]} ab {self.prov.get('ab_sha', '?')[:8]}")
        nv = [r for r in self.runs if r["void"]]
        for r in nv:
            self.out(f"VOID run {r['arm']} r{r['rep']} {r['cell']}/{r['dc']}: {'; '.join(r['void'])}")
        self.out(f"usable runs {len(self.runs) - len(nv)} of {len(self.runs)}")
        self.out("--- sample counts (usable reps per arm and cell, read before any value)")
        for cell, dc in self.cells:
            self.out(f"N {cell:9} {dc:9} " + " ".join(f"{a}={len(self.usable(a, cell, dc))}" for a in self.arms))
        self.void_rules()
        for cell in RF_CANDIDATES:
            for dc in sorted({d for c, d in self.cells if c == cell}):
                offs = self.usable(REFERENCE, cell, dc)
                fills = [r["m"]["fill"] for r in offs]
                rf = len(offs) >= MIN_REPS and all(f == 0 for f in fills) and (cell, dc) not in self.cell_void
                self.out(f"REORDER_FREE {cell}/{dc}: {'yes' if rf else 'no'} (off reorder_fill_original per rep "
                         f"{fills})")
                if rf:
                    self.rf_cells.append((cell, dc))
        for (cell, dc), why in sorted(self.cell_void.items()):
            self.out(f"VOID cell {cell}/{dc}: {'; '.join(why)}")
        for gv in self.global_void:
            self.out(f"VOID ALL: {gv}")
        self.table()
        self.qualitative()
        cands = [a for a in self.arms if a not in (CONTROL, REFERENCE)]
        for arm in cands:
            v, used = self.ratio_score(arm, STAT_REORDER_CELLS, "m1")
            self.score[arm] = sum(v) / len(v) if len(v) >= MIN_RANK_CELLS else None
            jv, _ = self.ratio_score(arm, JITTER_CELLS, "m1")
            self.jscore[arm] = sum(jv) / len(jv) if len(jv) >= MIN_JITTER_CELLS else None
            pv, _ = self.ratio_score(arm, STAT_REORDER_CELLS, "p99")
            self.p99score[arm] = sum(pv) / len(pv) if pv else None
            self.gates[arm] = self.gate_results(arm)
            for gate, (st, res) in self.gates[arm].items():
                bad = [t for s, t in res if s != "PASS"]
                self.out(f"GATE {arm:7} {gate} {st:4} " + ("; ".join(bad[:4]) + (" ..." if len(bad) > 4 else "")
                                                           if bad else f"{len(res)} checks pass"))
        for arm in self.arms:
            if self.global_void:
                self.verdict[arm] = ("VOID", "every verdict VOID: " + "; ".join(self.global_void))
            elif arm == CONTROL:
                self.verdict[arm] = ("CONTROL", "the A/A partner of off (table above)")
            elif arm == REFERENCE:
                self.verdict[arm] = ("REFERENCE", "the change's control")
            else:
                st = {g: s for g, (s, _) in self.gates[arm].items()}
                fails = [g for g, s in st.items() if s == "FAIL"]
                voids = [g for g, s in st.items() if s == "VOID"]
                cls = "DISQUALIFIED" if fails else "UNDECIDED" if voids else "QUALIFIED"
                text = " ".join(f"{g}={s}" for g, s in st.items())
                self.verdict[arm] = (cls, text)
        self.out("--- verdicts (score: mean over the statistical reorder cells of M1 / off's M1; lower is better)")
        for arm in self.arms:
            cls, text = self.verdict[arm]
            extra = ""
            if arm in self.score:
                extra = (f" score={fmt(self.score[arm], 'r')} jitter={fmt(self.jscore[arm], 'r')} "
                         f"p99_ratio={fmt(self.p99score[arm], 'r')}")
            self.out(f"VERDICT {arm:7} {cls:12}{extra} {text}")
        self.rank()
        self.fixed_vs_auto()
        self.judge_hypothesis()
        return self

    def table(self):
        self.out("--- medians per arm and cell: M1 copies/unit | p50 p99 ms | goodput | M2 resend/drop | acknack | "
                 "fill spurious | engaged early | CPU w/r ms/MB")
        for cell, dc in self.cells:
            for arm in self.arms:
                rs = self.usable(arm, cell, dc)
                if not rs:
                    continue

                def v(k, kind="f", rs=rs):
                    return fmt(med([r["m"].get(k) for r in rs])[0], kind)

                self.out(f"T {cell:9} {dc:9} {arm:7} n={len(rs)} M1 {v('m1', '%')} | {v('p50')} {v('p99')} | "
                         f"{v('goodput', '%')} | {v('m2', 'r')} | {v('acknack')} | {v('fill')} {v('spurious')} | "
                         f"{v('engaged')} {v('early')} | {v('cpu_w')}/{v('cpu_r')} | loss {v('realized', '%')} "
                         f"p {v('p_final', 'r')}")

    def qualitative(self):
        for cell in QUALITATIVE_CELLS:
            for dc in sorted({d for c, d in self.cells if c == cell}):
                row = []
                for arm in self.arms:
                    rs = self.usable(arm, cell, dc)
                    if rs:
                        nz = sum(1 for r in rs if (r["m"].get("m1") or 0) > 0)
                        row.append(f"{arm} {nz}/{len(rs)} reps with copies")
                self.out(f"QUALITATIVE {cell}/{dc} (0 against non-0 only): " + ", ".join(row))

    def rank(self):
        if self.global_void:
            self.out("RANKED: none (every verdict VOID)")
            self.out("BEST none")
            return
        q = [a for a in self.arms if self.verdict[a][0] == "QUALIFIED"]
        scored = sorted((a for a in q if self.score[a] is not None), key=lambda a: self.score[a])
        unscored = [a for a in q if self.score[a] is None]
        if scored:
            best = self.score[scored[0]]
            tie = [a for a in scored if self.score[a] <= best * (1.0 + TIE)]
            rest = [a for a in scored if a not in tie]
            tie.sort(key=lambda a: (self.p99score[a] if self.p99score[a] is not None else math.inf))
            self.ranked = tie + rest
            self.best = self.ranked[0]
            self.out("RANKED " + ", ".join(f"{i + 1}. {a} {fmt(self.score[a], 'r')}" for i, a in enumerate(self.ranked))
                     + (f" (tie within {fmt(TIE, '%')}: {', '.join(tie)}, broken by reorder-cell p99 ratio; "
                        "insn/sample and state bytes are S4/S5's)" if len(tie) > 1 else ""))
            self.out(f"BEST {self.best} score {fmt(self.score[self.best], 'r')}")
        else:
            self.out("RANKED: none")
        if unscored:
            self.out(f"QUALIFIED but unranked (under {MIN_RANK_CELLS} usable reorder cells): {', '.join(unscored)}")
        if not q:
            self.out("BEST none: NOTHING LANDS - no candidate qualified (4.4.5); the copies-vs-p99 table to the user:")
            for arm in self.arms:
                if arm in (CONTROL, REFERENCE):
                    continue
                cells = [f"{c} M1x{fmt(self.per_cell_ratio(arm, c), 'r')}" for c in STAT_REORDER_CELLS]
                self.out(f"  {arm:7} {self.verdict[arm][0]:12} p99_ratio {fmt(self.p99score.get(arm), 'r')} "
                         + " ".join(cells))

    def fixed_vs_auto(self):
        fixed = [a for a in self.arms if FIXED_RE.match(a)]
        gated = [a for a in self.arms if a in GATED]
        if self.global_void or not fixed:
            self.premise = "NOT_EVALUATED"
            self.out("AUTO_PREMISE NOT_EVALUATED (every verdict VOID or no fixed arm)")
            return
        best_per_cell = {}
        for cell in STAT_REORDER_CELLS:
            rs = [(self.per_cell_ratio(a, cell), a) for a in fixed]
            rs = [x for x in rs if x[0] is not None]
            if rs:
                best_per_cell[cell] = min(rs)[1]
        scored = [a for a in fixed if self.score.get(a) is not None]
        if len(best_per_cell) < MIN_RANK_CELLS or not scored:
            self.premise = "NOT_EVALUATED"
            self.out(f"AUTO_PREMISE NOT_EVALUATED (best fixed known on {len(best_per_cell)} cells)")
            return
        holds = [hold_ns(a) for a in best_per_cell.values()]
        spread = max(holds) / min(holds)
        single = min(scored, key=lambda a: self.score[a])
        better = [a for a in gated if self.score.get(a) is not None and self.score[a] < self.score[single]]
        per = ", ".join(f"{c} {a}" for c, a in best_per_cell.items())
        if spread > FIXED_SPREAD_MAX and not better:
            self.premise = "NOT_SUPPORTED"
            self.out(f"AUTO_PREMISE NOT_SUPPORTED: the best fixed hold differs {spread:.0f}x across cells ({per}) and "
                     f"no gated candidate scores below the best single fixed {single} ({fmt(self.score[single], 'r')}):"
                     f" recommend fixed {single} as the default, to the user (4.4.4)")
        elif spread > FIXED_SPREAD_MAX:
            self.premise = "SUPPORTED"
            self.out(f"AUTO_PREMISE SUPPORTED: best fixed differs {spread:.0f}x across cells ({per}); "
                     f"{', '.join(better)} score below the best single fixed {single} ({fmt(self.score[single], 'r')})")
        else:
            self.premise = "NOT_TESTED"
            self.out(f"AUTO_PREMISE NOT_TESTED: one fixed range is best on every cell (within {FIXED_SPREAD_MAX:.0f}x:"
                     f" {per}); gated below the best single fixed {single}: {', '.join(better) or 'none'}")

    def judge_hypothesis(self):
        gated = [a for a in self.arms if a in GATED]
        if self.global_void or not gated:
            self.hypothesis = "UNDECIDED"
            self.out("HYPOTHESIS UNDECIDED (every verdict VOID or no gated candidate)")
            return
        support, fals, undecided = [], [], []
        for a in gated:
            cls = self.verdict[a][0]
            js = self.jscore.get(a)
            if cls == "QUALIFIED" and js is not None and js <= HELP_MAX:
                support.append(f"{a} (jitter M1 ratio {fmt(js, 'r')})")
            elif cls == "DISQUALIFIED":
                fails = [g for g, (s, _) in self.gates[a].items() if s == "FAIL"]
                fals.append(f"{a} fails {'+'.join(fails)}")
            elif js is not None and js > HELP_MAX and cls != "VOID":
                fals.append(f"{a} jitter M1 ratio {fmt(js, 'r')} > {HELP_MAX}")
            else:
                undecided.append(f"{a} {cls} jitter {fmt(js, 'r')}")
        if support:
            self.hypothesis = "SUPPORTED"
            text = "; ".join(support)
        elif undecided:
            self.hypothesis = "UNDECIDED"
            text = "; ".join(undecided + fals)
        else:
            self.hypothesis = "FALSIFIED"
            text = "; ".join(fals)
        self.out(f"HYPOTHESIS {self.hypothesis} (a gated hold-off helps the jitter cells without hurting the loss "
                 f"cells): {text}")


def read_out(out):
    runs, prov, notes = load_out(out)
    if runs is None:
        return None
    rd = Reading(runs, prov)
    for n in notes:
        rd.out(f"NOTE {n}")
    return rd.read()


# --- selftest ---------------------------------------------------------------------------------------------------------

ST_ARMS = ["main", "off", "f250us", "f1ms", "f4ms", "f12ms", "B", "D", "E"]
ST_PIN = ["s_l0", "s_j0", "s_j5", "J_p2", "J_p10", "s_lb5", "s_lg5", "s_d20", "s_r1", "s_r6"]
ST_REPS = 2
ST_OFFERED = 150
# M1 of a candidate as a share of off's, per statistical reorder cell group (r1, r6, jitter)
ST_FACTOR = {"E": (0.05, 0.05, 0.05), "D": (0.97, 0.97, 0.97), "B": (0.5, 0.5, 0.5), "f250us": (0.2, 0.8, 0.9),
             "f1ms": (0.5, 0.3, 0.7), "f4ms": (0.6, 0.5, 0.4), "f12ms": (0.9, 0.7, 0.1)}


def st_truth(arm, cell, rep):
    """The baseline synthetic world: E helps everywhere, f12ms hurts the loss cells' p99, B holds on a reorder-free
    cell, D does nothing; main == off."""
    s = {"offered": ST_OFFERED, "delivered": ST_OFFERED, "m1": 0.0, "owd": 2.0, "tail": 3.0, "retx": 0, "drops": 0,
         "engaged": 0, "fill": 0, "gaps": 0, "acknack": 50, "spurious": None, "jump": 0, "stuck": 0, "ctl_drop": 0,
         "holdoff": "auto", "load": 0.5}
    base = cell.replace("_pin", "")
    if base in STAT_REORDER_CELLS:
        off_m1 = 0.9 if base in JITTER_CELLS else 0.3
        f = 1.0
        if arm in ST_FACTOR:
            f = ST_FACTOR[arm][0 if base == "s_r1" else 1 if base == "s_r6" else 2]
        s.update(m1=off_m1 * f, owd=12.0 if base in JITTER_CELLS else 6.0, tail=15.0, acknack=400, retx=1000,
                 fill=int(off_m1 * ST_OFFERED * 723), engaged=0 if arm in ("main", "off") else 100)
        s["gaps"] = int(ST_OFFERED * 723 * R1_SHARE) if base == "s_r1" else 500
    elif base in LOSS_CELLS:
        s.update(owd=1.5 if base != "s_d20" else 20.5, tail=30.0, drops=5400, retx=5500, gaps=3000, acknack=3000)
        if arm == "f12ms":
            s["tail"] = 45.0
    elif cell == "s_l0":
        s.update(m1=0.0003, fill=3, gaps=3)
    if cell in ("s_l0_pin", "s_j0") and arm == "B":
        s["engaged"] = 3
    if cell == "s_j0":
        s.update(owd=7.0, tail=8.0)
    return s


def st_write_run(base, arm, rep, part, tvarm, s):
    inv = os.path.join(base, "runs", f"{arm}.r{rep}.{part}")
    d = os.path.join(inv, "runs", f"{tvarm}-rel_large-1")
    os.makedirs(d, exist_ok=True)
    if not os.path.exists(os.path.join(inv, "provenance.txt")):
        with open(os.path.join(inv, "provenance.txt"), "w") as f:
            f.write("tree=synthetic dirty_paths=0 profile=ethernet reps=1 dur=30 d_bg_loss=2\n")
        with open(os.path.join(inv, "summary.txt"), "w") as f:
            f.write("=== time_varying_netns ended rc=0 synthetic\n")

    def w(name, lines):
        with open(os.path.join(d, name), "w") as f:
            f.write("\n".join(lines) + "\n")
    tv = "_p" in tvarm
    w("meta.txt", [f"load={s['load']} nproc=16 start=x tv={int(tv)} phase_s=0"])
    ph = ["PHASE idx=0 key=x t_before_ns=0 t_after_ns=0 ok=1"]
    if tv:
        ph += ["PHASE idx=1 key=j5 t_before_ns=10000000000 t_after_ns=10000000001 ok=1",
               "PHASE idx=2 key=j0 t_before_ns=20000000000 t_after_ns=20000000001 ok=1"]
    w("phases.txt", ph)
    cnt = []
    for label in ("p", "e", "i"):
        pk, dr = (ST_OFFERED * 723 + s["retx"], s["drops"]) if label == "p" else (500, 0)
        if label == "e" and s["ctl_drop"]:
            dr = s["ctl_drop"]
        cnt += [f"== {label} tx_bytes=1000 tx_packets={pk} tx_dropped=0 rx_dropped=0",
                "qdisc prio 1: root refcnt 2 bands 2", f" Sent 1000 bytes {pk} pkt (dropped {dr}, overlimits 0)",
                "qdisc netem 10: parent 1:1 limit 10000", " Sent 100 bytes 10 pkt (dropped 0, overlimits 0)",
                "qdisc netem 20: parent 1:2 limit 10000", f" Sent 900 bytes {pk - 10} pkt (dropped {dr}, overlimits 0)"]
    w("counters.txt", cnt)
    ho = []
    if arm != "main":
        e = expected_holdoff(arm) if s["holdoff"] == "auto" else s["holdoff"]
        ho = [f"HOLDOFF: mode={e[0]} policy={e[1]} value_ns={e[2]} env={e[3]}"]
    dl = s["delivered"]
    large_dup = int(round(s["m1"] * dl * 723))
    spurious = large_dup if s["spurious"] is None else s["spurious"]
    w("ping.log", ho + [f"RESULT: role=pub size=1048576 reliable=1 published={s['offered']} retransmitted={s['retx']} "
                        f"cpu_ms=1.0 cpu_ms_per_mb=0.5 large_published={s['offered']} hybrid_runs=1 "
                        f"hybrid_run_units={s['offered'] * 723} hybrid_mtu_units=0",
                        f"RSTATS: role=pub pub acknack_received=1 retransmitted={s['retx']}"])
    lat = []
    for i in range(dl):
        sent = 1000000000 + i * 200000000
        owd = s["tail"] if i >= dl - 5 else s["owd"]
        lat.append(f"LAT index={i} sent_ns={sent} arrived_ns={sent + int(owd * 1e6)}")
    rst = [f"RSTATS: role=sub sub gaps_opened={s['gaps']} missing_opened={s['gaps']} jump_abandoned_seq={s['jump']} "
           f"retry_giveups=0 hb_abandoned_seq=0 late_below_ack=0 duplicates=0",
           f"RSTATS: role=sub reorder reorder_fill_original={s['fill']} nack_spurious={spurious} "
           f"reorder_absorbed=0 reorder_fill_before_rtt=0",
           f"RSTATS: role=sub acknack_tx immediate=1 timer=0 new_gap=1 reveal=0 sent={s['acknack']} bits_sent=1 "
           f"bytes_sent={s['acknack'] * 40}"]
    if arm != "main":
        rst.append(f"RSTATS: role=sub holdoff engaged={s['engaged']} released={s['engaged']} early=0 acknack_held=0 "
                   "gate_opened=0")
    w("echo.log", ho + [f"RESULT: role=sub size=1048576 reliable=1 delivered={dl} cpu_ms_per_mb=0.4 "
                        f"large_duplicate={large_dup} first_index=0"] + lat + rst)
    w("idle.log", ho)
    w("echo.err", [f"[W] {STUCK_TEXT} 5 from node 1 after 9 retries"] * s["stuck"])


def st_build(base, tweak=None):
    os.makedirs(base)
    with open(os.path.join(base, "provenance.txt"), "w") as f:
        f.write(f"main_sha=synthetic ab_sha=synthetic arms='{' '.join(ST_ARMS)}' reps={ST_REPS} dur=30\n")
    for rep in range(1, ST_REPS + 1):
        for arm in ST_ARMS:
            for part, cells in (("pin", ST_PIN), ("free", ["s_l0"])):
                for tvarm in cells:
                    cell = cell_name(tvarm, part)
                    s = st_truth(arm, cell, rep)
                    if tweak:
                        tweak(arm, cell, rep, s)
                    st_write_run(base, arm, rep, part, tvarm, s)


def selftest():
    cases = []

    def case(name, tweak, checks):
        cases.append((name, tweak, checks))

    def void_runs(rd, arm, text):
        return [r for r in rd.runs if r["arm"] == arm and any(text in v for v in r["void"])]

    case("baseline: the best rule picks E; G1 and G2 disqualify; premise and hypothesis supported", None, [
        ("no run VOID", lambda rd: not [r for r in rd.runs if r["void"]]),
        ("no VOID ALL", lambda rd: not rd.global_void),
        ("BEST is E", lambda rd: rd.best == "E"),
        ("f12ms DISQUALIFIED by G1", lambda rd: rd.verdict["f12ms"][0] == "DISQUALIFIED"
         and rd.gates["f12ms"]["G1"][0] == "FAIL"),
        ("B DISQUALIFIED by G2 (engaged on a reorder-free cell)", lambda rd: rd.gates["B"]["G2"][0] == "FAIL"),
        ("D QUALIFIED and ranked last", lambda rd: rd.verdict["D"][0] == "QUALIFIED" and rd.ranked[-1] == "D"),
        ("both reorder-free cells measured", lambda rd: len(rd.rf_cells) == 2),
        ("AUTO_PREMISE SUPPORTED", lambda rd: rd.premise == "SUPPORTED"),
        ("HYPOTHESIS SUPPORTED", lambda rd: rd.hypothesis == "SUPPORTED"),
    ])

    def vi(arm, cell, rep, s):
        if arm == "off" and cell in STAT_REORDER_CELLS:
            s["spurious"] = int(s["m1"] * s["delivered"] * 723 * 0.8)
    case("V-I: off's nack_spurious 20% off its large_duplicate voids everything", vi, [
        ("VOID ALL names V-I", lambda rd: any(g.startswith("V-I") for g in rd.global_void)),
        ("every candidate VOID", lambda rd: all(rd.verdict[a][0] == "VOID" for a in ST_ARMS)),
        ("no BEST", lambda rd: rd.best is None),
    ])

    def vt(arm, cell, rep, s):
        if arm == "D" and rep == 1:
            s["holdoff"] = ("auto", "E", 0, "D")  # the bench ran E while the arm names D
        if arm == "off" and rep == 2 and cell == "s_lb5":
            s["engaged"] = 2
        if arm == "f1ms" and rep == 1 and cell == "s_r6":
            s["engaged"] = 0

    def vt_main(rd_base):
        # main with a HOLDOFF line: write one into a main run directly (st_write_run never prints one for main)
        p = os.path.join(rd_base, "runs", "main.r1.pin", "runs", "s_j0-rel_large-1", "echo.log")
        with open(p, "a") as f:
            f.write("HOLDOFF: mode=off policy=- value_ns=0 env=-\n")
    case("V-T: a wrong HOLDOFF line, main printing one, off holding, a fixed arm holding nothing", vt, [
        ("D rep 1 VOID on its HOLDOFF line", lambda rd: len(void_runs(rd, "D", "is not the arm's")) == len(ST_PIN)
         + 1),
        ("D UNDECIDED (one usable rep left)", lambda rd: rd.verdict["D"][0] == "UNDECIDED"),
        ("D's G3 VOID, not PASS: nothing it could evaluate", lambda rd: rd.gates["D"]["G3"][0] == "VOID"),
        ("main's HOLDOFF line voids its run", lambda rd: len(void_runs(rd, "main", "not the main tree")) == 1),
        ("off holding voids its run", lambda rd: len(void_runs(rd, "off", "off held")) == 1),
        ("f1ms holding nothing on s_r6 is VOID", lambda rd: len(void_runs(rd, "f1ms", "not applied")) == 1),
    ])
    cases[-1] = cases[-1] + (vt_main,)

    def jit(arm, cell, rep, s):
        if arm in ("main", "off") and cell == "s_j5":
            s["m1"] = 0.2
    case("V-T jitter: off's s_j5 M1 under 50% voids the J cells", jit, [
        ("s_j5, J_p2, J_p10 VOID", lambda rd: all((c, "rel_large") in rd.cell_void for c in JITTER_CELLS)),
        ("E's jitter score unreadable", lambda rd: rd.jscore["E"] is None),
    ])

    def r1(arm, cell, rep, s):
        if arm == "off" and cell == "s_r1":
            s["gaps"] = 100
    case("V-T link reorder: off's s_r1 gaps far from 1% of units voids s_r1", r1, [
        ("s_r1 VOID", lambda rd: ("s_r1", "rel_large") in rd.cell_void),
        ("s_r6 not VOID", lambda rd: ("s_r6", "rel_large") not in rd.cell_void),
    ])

    def aa(arm, cell, rep, s):
        if arm == "main" and cell == "s_r6":
            s["m1"] *= 1.3
    case("V-C: main 30% above off on s_r6 voids M1 there", aa, [
        ("A/A void on s_r6 m1", lambda rd: ("s_r6", "rel_large", "m1") in rd.aa_void),
        ("E still BEST on the other cells", lambda rd: rd.best == "E"),
    ])

    def samples(arm, cell, rep, s):
        if arm == "E" and cell == "s_r1" and rep == 1:
            s["delivered"] = 50
    case("sample count: 50 delivered samples void the run", samples, [
        ("E s_r1 rep 1 VOID on samples", lambda rd: len(void_runs(rd, "E", "samples")) == 1),
    ])

    def ctl(arm, cell, rep, s):
        if arm == "off" and rep == 1 and cell == "s_l0_pin":
            s["ctl_drop"] = 3
    case("control: the invocation's s_l0 dropping voids every run of that invocation", ctl, [
        ("all of off.r1.pin VOID", lambda rd: len([r for r in rd.runs if r["inv"] == "off.r1.pin" and any(
            "invocation control" in v for v in r["void"])]) == len(ST_PIN)),
        ("off.r1.free untouched", lambda rd: not [r for r in rd.runs if r["inv"] == "off.r1.free" and r["void"]]),
    ])

    def none_q(arm, cell, rep, s):
        if arm not in ("main", "off") and cell in LOSS_CELLS:
            s["tail"] = 45.0
    case("nothing qualifies: every candidate's loss-cell p99 is worse", none_q, [
        ("no BEST", lambda rd: rd.best is None),
        ("NOTHING LANDS printed", lambda rd: any("NOTHING LANDS" in x for x in rd.lines)),
        ("HYPOTHESIS FALSIFIED", lambda rd: rd.hypothesis == "FALSIFIED"),
    ])

    def goodput(arm, cell, rep, s):
        if arm == "E" and cell == "s_lb5":
            s["delivered"] = ST_OFFERED - 2  # 98.67% within the deadline, p99 unchanged: G1 fails on goodput alone
    case("G1 goodput: E 1.3 points below off on s_lb5 is DISQUALIFIED", goodput, [
        ("E's G1 FAIL names goodput only", lambda rd: rd.gates["E"]["G1"][0] == "FAIL" and all(
            "goodput" in t for s, t in rd.gates["E"]["G1"][1] if s == "FAIL")),
        ("E not BEST", lambda rd: rd.best != "E"),
    ])

    def premise(arm, cell, rep, s):
        if arm in ("E", "D", "B") and cell in STAT_REORDER_CELLS:
            s["m1"] = (0.9 if cell in JITTER_CELLS else 0.3) * 0.95
    case("fixed vs auto: best fixed differs > 4x and no auto beats it", premise, [
        ("AUTO_PREMISE NOT_SUPPORTED", lambda rd: rd.premise == "NOT_SUPPORTED"),
        ("recommends f12ms", lambda rd: any("recommend fixed f12ms" in x for x in rd.lines)),
        ("HYPOTHESIS FALSIFIED (jitter ratio 0.95)", lambda rd: rd.hypothesis == "FALSIFIED"),
    ])

    def no_rf(arm, cell, rep, s):
        if cell in RF_CANDIDATES:
            s["fill"] = 2
    case("G2 cannot look: no measured reorder-free cell leaves the candidates UNDECIDED, not QUALIFIED", no_rf, [
        ("no reorder-free cell", lambda rd: not rd.rf_cells),
        ("E UNDECIDED with G2 VOID", lambda rd: rd.verdict["E"][0] == "UNDECIDED"
         and rd.gates["E"]["G2"][0] == "VOID"),
        ("HYPOTHESIS UNDECIDED", lambda rd: rd.hypothesis == "UNDECIDED"),
    ])

    def tie(arm, cell, rep, s):
        if arm == "f4ms" and cell in STAT_REORDER_CELLS:
            s["m1"] = (0.9 if cell in JITTER_CELLS else 0.3) * 0.052
            s["tail"] = 10.0  # lower reorder-cell p99 than E's 15 ms
    case("tie: f4ms within 10% of E's score wins on reorder-cell p99", tie, [
        ("BEST is f4ms", lambda rd: rd.best == "f4ms"),
        ("E second", lambda rd: rd.ranked[1] == "E"),
    ])

    failed = 0
    root = tempfile.mkdtemp(prefix="holdoff_ab_selftest.")
    try:
        for i, c in enumerate(cases):
            name, tweak, checks = c[0], c[1], c[2]
            base = os.path.join(root, f"case{i}")
            st_build(base, tweak)
            if len(c) > 3:
                c[3](base)
            rd = read_out(base)
            print(f"--- selftest {i + 1}: {name}")
            for label, fn in checks:
                try:
                    ok = bool(fn(rd))
                except (KeyError, IndexError, TypeError) as e:
                    ok = False
                    label += f" ({type(e).__name__}: {e})"
                failed += not ok
                print(f"  {'ok  ' if ok else 'FAIL'} {label}")
            if os.environ.get("HOLDOFF_SELFTEST_VERBOSE"):
                print("\n".join("    | " + x for x in rd.lines))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    total = sum(len(c[2]) for c in cases)
    print(f"selftest: {total - failed} of {total} expectations met")
    return 1 if failed else 0


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        return selftest()
    if len(sys.argv) != 2:
        print("usage: holdoff_ab_read.py OUT | holdoff_ab_read.py --selftest", file=sys.stderr)
        return 2
    rd = read_out(sys.argv[1])
    if rd is None:
        print(f"holdoff_ab_read: no runs directory in {sys.argv[1]}", file=sys.stderr)
        return 2
    print("\n".join(rd.lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
