#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

"""Reading rules for freertos_frag_q.sh, pre-registered (committed before the main run).

Model: a datagram of n units (n * 1446 B payload) leaves Linux as f(n) = ceil((n*1446 + 8) / 1480) IP fragments
(f(n) = n for every n here); netem drops each fragment independently with p, so q(n) = (1-p)^f(n) when the
receiver reassembles everything it is offered.

The reassembly limit L is read from the guest's own boot line (not typed here):
    L = min(IP_REASS_MAX_PBUFS, PBUF_POOL_SIZE)
because lwIP refuses a fragment once more than IP_REASS_MAX_PBUFS pbufs are queued for reassembly, and the virtio
driver (platform/freertos/net_init.c) copies each received frame into one PBUF_POOL pbuf (PBUF_POOL_BUFSIZE 1600 >
a 1514-byte frame), so a datagram of f fragments needs f pool pbufs held at once.

Per cell: q_hat = got/sent with a Wilson 95% interval [lo, hi]; pred = (1-p)^f(n).

  F4 (VOID, per rate): the rate's n=1, p=0 cell has q_hat < 0.999, or no final line from the guest. Then the guest
     itself drops at that rate (a virtualization artefact: QEMU's TCG CPU and virtual NIC are not an MCU), and F1/F3
     are not judged at that rate. Checked first.
  F1 (low rate, 1 MB/s): every cell with f(n) <= L has pred inside [lo, hi]. FALSIFIED lists the cells that miss.
  F2 (both rates): every cell with f(n) > L has got = 0. FALSIFIED otherwise. Also printed: whether n = L itself
     reassembles at p = 0 (the limit is tight, not merely an upper bound).
  F3 (high rate): (a) some cell with 2 <= f(n) <= L and p > 0 has hi < pred (q below the model beyond its CI), and
     (b) no p = 0 cell with f(n) <= L has hi < pred. HOLDS iff (a) and (b). Supplementary, not part of the verdict:
     per (n, p) whether the high-rate q is below the low-rate q beyond both CIs (hi_high < lo_low), i.e. whether the
     deficit is attributable to rate.
  F3 amendment (b'), added after the rate probes and before the main run: at every rate above 1 MB/s the guest drops
     1-5 datagrams per ~10^4 even at n=1, p=0 (probe3: all frames reached udp_recv; the netconn recvmbox of 8
     overflowed - an application-side drop, not reassembly). Strict (b) is then falsified by n=1 itself. (b') judges
     the p=0 cells against the rate's own n=1, p=0 cell instead of 1: a p=0 cell with f(n) <= L fails (b') when its
     hi < lo of that baseline. Both (b) and (b') are printed; F3' = (a) and (b').
  Control (A/A): the cell (1 MB/s, n=4, p=0.01) runs twice, first and last. Two-proportion z on the two q_hat; |z| >
     1.96 means cell-to-cell variance exceeds binomial and every CI verdict above is overconfident - printed as such.
  Treatment check (every cell): realized fragment loss from the netem counters, dropped / (sent + dropped), during the
     data phase. p = 0 must realize 0; p > 0 must realize within [p/2, 2p] when at least 20 drops are expected. A
     cell failing this is marked TREATMENT? and excluded from F1/F3 (it is not the cell it claims to be).
"""

import math
import re
import sys
from pathlib import Path

UNIT = 1446
LO_RATE = 1000000


def frags(n: int) -> int:
    return math.ceil((n * UNIT + 8) / 1480)


def wilson(k: int, m: int, z: float = 1.96) -> tuple[float, float]:
    if m == 0:
        return 0.0, 1.0
    ph = k / m
    d = 1 + z * z / m
    c = (ph + z * z / (2 * m)) / d
    h = z * math.sqrt(ph * (1 - ph) / m + z * z / (4 * m * m)) / d
    return max(0.0, c - h), min(1.0, c + h)


def plan(hi_rate: float) -> None:
    print("plan: sample sizes (count per cell; 95% Wilson half-width at the predicted q)")
    for rate in [LO_RATE] + ([hi_rate] if hi_rate > 0 else []):
        for p in (0, 0.001, 0.01, 0.05):
            row = []
            for n in (1, 2, 4, 8, 9):
                secs = 10 if (p == 0 or n == 9) else 30
                m = max(50, round(rate * secs / (n * UNIT)))
                q = (1 - p) ** frags(n)
                lo, hi = wilson(round(q * m), m)
                row.append(f"n={n}:N={m},q={q:.3f},+-{(hi - lo) / 2:.4f}")
            print(f"plan: rate={rate:.0f} p={p} " + " ".join(row))


def parse_kv(line: str) -> dict:
    return {k: v for k, v in re.findall(r"(\w+)=([-\w.]+)", line)}


def load(out: Path) -> tuple[list, dict]:
    rows = []
    cfg = {}
    lines = (out / "cells.tsv").read_text().splitlines()
    head = lines[0].split("\t")
    for line in lines[1:]:
        r = dict(zip(head, line.split("\t")))
        base = Path(r["log"])
        guest = base.with_suffix(base.suffix + ".guest") if False else Path(str(base) + ".guest")
        send = Path(str(base) + ".send")
        gtxt = guest.read_text(errors="replace") if guest.exists() else ""
        stxt = send.read_text(errors="replace") if send.exists() else ""
        for gl in gtxt.splitlines():
            if gl.startswith("frag_rx: config") and r["label"] != "pool16":
                cfg = parse_kv(gl)
        final = next((parse_kv(x) for x in gtxt.splitlines() if x.startswith("frag_rx: final")), None)
        stats = next((parse_kv(x) for x in gtxt.splitlines() if x.startswith("frag_rx: stats")), {})
        start = next((parse_kv(x) for x in stxt.splitlines() if x.startswith("send: start")), {})
        data = next((parse_kv(x) for x in stxt.splitlines() if x.startswith("send: data")), {})
        done = next((parse_kv(x) for x in stxt.splitlines() if x.startswith("send: done")), {})
        r.update(n=int(r["n"]), p=float(r["p"]), rate=float(r["rate_Bps"]), sent=int(r["sent"]), final=final,
                 stats=stats, achieved=float(done.get("achieved_Bps", "nan")))
        r["got"] = int(final["got"]) if final else None
        try:
            ds = int(data["qdisc_sent"]) - int(start["qdisc_sent"])
            dd = int(data["qdisc_dropped"]) - int(start["qdisc_dropped"])
            r["loss_real"] = dd / (ds + dd) if ds + dd > 0 else float("nan")
            r["frames"] = ds + dd
            r["tap_drop"] = int(data["link_tx_dropped"]) - int(start["link_tx_dropped"])
        except (KeyError, ValueError):
            r["loss_real"], r["frames"], r["tap_drop"] = float("nan"), 0, -1
        rows.append(r)
    return rows, cfg


def treatment_ok(r: dict) -> bool:
    p, lr = r["p"], r["loss_real"]
    if math.isnan(lr):
        return False
    if p == 0:
        return lr == 0
    if p * r["frames"] < 20:
        return True
    return p / 2 <= lr <= 2 * p


def analyze(out: Path) -> int:
    rows, cfg = load(out)
    if not cfg:
        print("analyze: no guest config line - nothing ran")
        return 1
    print("config (guest boot line): " + " ".join(f"{k}={v}" for k, v in cfg.items()))
    lim = min(int(cfg["IP_REASS_MAX_PBUFS"]), int(cfg["PBUF_POOL_SIZE"]))
    print(f"L = min(IP_REASS_MAX_PBUFS, PBUF_POOL_SIZE) = {lim}")
    print("cell label  rate      n  f  p      sent    got     q_hat   [lo, hi]          pred    in_CI loss_real tap_drop "
          "pool_err reass_memerr ipfrag_drop reassdata_err achieved_Bps")
    for r in rows:
        if r["got"] is None:
            print(f"{r['cell']} {r['label']:6} {r['rate']:.0f} n={r['n']} p={r['p']} NO FINAL LINE")
            continue
        q = r["got"] / r["sent"]
        lo, hi = wilson(r["got"], r["sent"])
        pred = (1 - r["p"]) ** frags(r["n"])
        r.update(q=q, lo=lo, hi=hi, pred=pred, inci=lo <= pred <= hi, tok=treatment_ok(r))
        st = r["stats"]
        print(f"{r['cell']} {r['label']:6} {r['rate']:<9.0f} {r['n']:<2} {frags(r['n']):<2} {r['p']:<6} {r['sent']:<7} "
              f"{r['got']:<7} {q:.4f} [{lo:.4f}, {hi:.4f}] {pred:.4f} {'yes' if r['inci'] else 'NO ':5} "
              f"{r['loss_real']:.4f}{'' if r['tok'] else ' TREATMENT?'} {r['tap_drop']} {st.get('pool_err')} "
              f"{st.get('ipfrag_memerr')} {st.get('ipfrag_drop')} {st.get('reassdata_err')} {r['achieved']:.0f}")
    grid = [r for r in rows if r["label"] in ("grid", "aa1", "aa2") and r["got"] is not None]
    rates = sorted({r["rate"] for r in grid})
    void = {}
    for rate in rates:
        ctl = [r for r in grid if r["rate"] == rate and r["n"] == 1 and r["p"] == 0]
        void[rate] = not ctl or any(r["q"] < 0.999 for r in ctl)
        print(f"F4 rate={rate:.0f}: {'VOID (guest drops at n=1 p=0: ' + ', '.join(f'{r[chr(113)]:.4f}' for r in ctl) + ')' if void[rate] else 'not void (n=1 p=0 q=' + ', '.join(f'{r[chr(113)]:.4f}' for r in ctl) + ')'}")
    missing = [r for r in rows if r["label"] in ("grid", "aa1", "aa2") and r["got"] is None]
    if missing:
        print(f"note: {len(missing)} grid cells have no final line: " + ", ".join(r["cell"] for r in missing))

    def judged(rate):
        return [r for r in grid if r["rate"] == rate and r["tok"]]

    lo_rate = LO_RATE
    hi_rates = [x for x in rates if x != lo_rate]
    if lo_rate in void and not void[lo_rate]:
        bad = [r for r in judged(lo_rate) if frags(r["n"]) <= lim and not r["inci"]]
        print(f"F1 (1 MB/s, f(n) <= {lim}): {'HOLDS' if not bad else 'FALSIFIED'}"
              + "".join(f"\n   miss: cell {r['cell']} n={r['n']} p={r['p']} q={r['q']:.4f} [{r['lo']:.4f},{r['hi']:.4f}] pred={r['pred']:.4f}" for r in bad))
    else:
        print("F1: not judged (low rate VOID or absent)")
    above = [r for r in grid if frags(r["n"]) > lim]
    bad2 = [r for r in above if r["got"] != 0]
    tight = [r for r in grid if r["rate"] == lo_rate and frags(r["n"]) == lim and r["p"] == 0]
    print(f"F2 (f(n) > {lim} gives q = 0, {len(above)} cells): {'HOLDS' if above and not bad2 else ('FALSIFIED' if bad2 else 'NO CELLS')}"
          + "".join(f"\n   nonzero: cell {r['cell']} n={r['n']} got={r['got']}" for r in bad2)
          + f"\n   tightness: n=L={lim} at p=0, 1 MB/s: " + ", ".join(f"q={r['q']:.4f}" for r in tight))
    for rate in hi_rates:
        if void[rate]:
            print(f"F3 rate={rate:.0f}: not judged (VOID)")
            continue
        j = judged(rate)
        a = [r for r in j if 2 <= frags(r["n"]) <= lim and r["p"] > 0 and r["hi"] < r["pred"]]
        b = [r for r in j if frags(r["n"]) <= lim and r["p"] == 0 and r["hi"] < r["pred"]]
        base = [r for r in j if r["n"] == 1 and r["p"] == 0]
        b2 = [r for r in j if frags(r["n"]) <= lim and r["p"] == 0 and base and r["hi"] < base[0]["lo"]]
        print(f"F3' rate={rate:.0f}: {'HOLDS' if a and base and not b2 else 'FALSIFIED'} (b': {len(b2)} p=0 cells below the n=1 p=0 baseline)"
              + "".join(f"\n   p=0 below baseline: n={r['n']} q={r['q']:.4f} hi={r['hi']:.4f}" for r in b2))
        print(f"F3 rate={rate:.0f}: {'HOLDS' if a and not b else 'FALSIFIED'} (a: {len(a)} lossy cells below model beyond CI; b: {len(b)} p=0 cells below)"
              + "".join(f"\n   below: n={r['n']} p={r['p']} q={r['q']:.4f} hi={r['hi']:.4f} pred={r['pred']:.4f}" for r in a)
              + "".join(f"\n   p=0 below: n={r['n']} q={r['q']:.4f}" for r in b))
        for r in j:
            if r["p"] > 0 and 2 <= frags(r["n"]) <= lim:
                low = [x for x in judged(lo_rate) if x["n"] == r["n"] and x["p"] == r["p"]]
                if low:
                    lw = low[0]
                    print(f"   F3-rate n={r['n']} p={r['p']}: high q={r['q']:.4f} [{r['lo']:.4f},{r['hi']:.4f}] vs low q={lw['q']:.4f} [{lw['lo']:.4f},{lw['hi']:.4f}] -> {'rate-attributable' if r['hi'] < lw['lo'] else 'not beyond CIs'}")
    aa = [r for r in rows if r["label"] in ("aa1", "aa2") and r["got"] is not None]
    if len(aa) == 2:
        k1, m1, k2, m2 = aa[0]["got"], aa[0]["sent"], aa[1]["got"], aa[1]["sent"]
        pp = (k1 + k2) / (m1 + m2)
        se = math.sqrt(pp * (1 - pp) * (1 / m1 + 1 / m2)) or float("nan")
        z = (k1 / m1 - k2 / m2) / se
        print(f"A/A (1 MB/s n=4 p=0.01): q1={k1 / m1:.4f} q2={k2 / m2:.4f} z={z:.2f} -> "
              + ("agree" if abs(z) <= 1.96 else "DISAGREE: variance exceeds binomial, CI verdicts overconfident"))
    else:
        print("A/A: incomplete")
    pool = [r for r in rows if r["label"] == "pool16" and r["got"] is not None]
    if pool:
        print("pool16 diagnostic (PBUF_POOL_SIZE=16, IP_REASS_MAX_PBUFS unchanged, p=0, 1 MB/s): "
              + " ".join(f"n={r['n']}:q={r['got'] / r['sent']:.4f}" for r in pool))
    return 0


if __name__ == "__main__":
    if sys.argv[1] == "plan":
        plan(float(sys.argv[2]) if len(sys.argv) > 2 else 0)
        sys.exit(0)
    sys.exit(analyze(Path(sys.argv[1])))
