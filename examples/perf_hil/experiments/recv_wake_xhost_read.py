#!/usr/bin/env python3
"""Reader of recv_wake_xhost_rig.sh: is one io_uring call (wait and read together) worth building into the HAL?

Usage:
    recv_wake_xhost_read.py OUT          read OUT.runs/ (index.txt, <stem>_rx.log, <stem>_tx.log, <stem>.samples)
    recv_wake_xhost_read.py --prereg     print the reading rules this file enforces
    recv_wake_xhost_read.py --selftest   check the rules against synthetic runs whose verdict is known

The rules (PREREG) were written on 2026-10-21, before any run on the rig, and each one is code below, not a comment.
"""

import os
import re
import statistics
import sys
import tempfile

# --- pre-registered constants -------------------------------------------------------------------------------------
MIN_N = 20000  # samples a run must hold (the driver sends ROUNDS = 20100 so that a few lost datagrams leave 20000)
MAX_BAD_STAMP_FRACTION = 0.001  # datagrams whose kernel stamp was missing or implausible, of those received
MIN_REPS = 3  # valid reps an arm needs
GAIN_US = 0.5  # the bar: an io_uring style must return this much earlier at p50 than epoll3 (the 4(b) analysis)
AA_MAX_US = GAIN_US / 2  # the A/A control must agree within half the bar, or the instrument cannot see the bar
# A "no gain" is only read where both compared ranges are at most twice the bar wide. Added 2026-10-21 after the PC
# dry runs, before any rig run: single runs in a slow state (+4..+7 us) on the shared PC widened ranges enough that
# overlap, not the absence of a gain, decided - a DROP that was really "could not see".
NOISE_MAX_US = 2 * GAIN_US
CONTROL = ("recvfrom", "recvfrom_aa")  # the same code under two names, interleaved in the rotation
BASE = "epoll3"  # rmw_tickle's receive today: epoll_pwait2 on a kept set, then recvfrom
BLOCKING = "recvfrom"  # the vendors' wait: the floor a one-call wait could reach
URING = ["uring", "uring_dt", "uring_ms", "uring_ms_dt"]
ARMS = ["recvfrom", "recvfrom_aa", "recvmsg", "epoll3"] + URING

PREREG = f"""recv_wake_xhost - reading rules (written 2026-10-21 before any rig run; enforced by recv_wake_xhost_read.py)
Measured, per datagram, on the receiving Pi's own clock: kernel software receive stamp (skb->tstamp, read with
  SIOCGSTAMPNS after the call returned) -> the receive call returning to user space. Sender on the other Pi, over eth0,
  one datagram at a time, each after the previous echo and a gap, so the receiver is asleep every time.
Sample counts first: every line prints each rep's sample count n.
VOID run: n < {MIN_N}; the reader's count of the samples file differs from the receiver's RXRESULT n (the witness);
  bad stamps above {MAX_BAD_STAMP_FRACTION:.1%} of the datagrams received; the sender reported no receiver or no
  TXRESULT; the driver marked it VOID (timeout, wrong binary). REFUSED run: the kernel refused the style's setup
  (not measured; not a failure). VOID and REFUSED runs are listed and never used.
An arm needs >= {MIN_REPS} valid reps, else it is NOT MEASURED.
Per arm: P50 = the median over its valid reps of the rep's p50; RANGE = [min, max] of the rep p50s. p10/p99 pooled
  over the valid reps' samples are printed with their n; they do not decide.
CONTROL (A/A): {CONTROL[0]} and {CONTROL[1]} are the same code. PASS iff both are measured, |P50 difference| <
  {AA_MAX_US} us and their RANGEs overlap. Otherwise CONTROL FAILED: no verdict (the instrument cannot resolve
  {GAIN_US} us).
BASE: {BASE} must be measured, else NO VERDICT.
Premise (diagnostic, printed): P50({BASE}) - P50({BLOCKING}), the most a one-call wait could recover.
Decision, per io_uring style U in {URING}:
  GAINS iff P50({BASE}) - P50(U) >= {GAIN_US} us AND max RANGE(U) < min RANGE({BASE}) (non-overlapping rep ranges).
  Corroboration (printed, does not decide): the sender's round-trip p50, median over reps, also lower by >= {GAIN_US / 2}
  us. A GAINS without it is flagged "stamp only".
  CPU per datagram of the receiving thread, U against {BASE}, is printed beside it (does not decide).
  Paired per rep, p50({BASE}) - p50(U) in the same rep, is printed (does not decide): a run in a slow state (seen on
  the PC dry run: single runs 4 us above their arm's others) widens a range and is visible there.
  NOISY iff U does not GAIN and RANGE(U) or RANGE({BASE}) is wider than {NOISE_MAX_US} us: overlap there may be a
  slow-state run, not the absence of a gain - the comparison cannot say "no".
VERDICT: WORTH IMPLEMENTING (best U named) if any U GAINS; else NO VERDICT (NOISY, re-run) if any U is NOISY; else
  DROP (every measured U: no gain); NO VERDICT if no U was measured. Falsifies the 4(b) proposal: DROP with the
  control PASS.
"""


def nearest_rank(sorted_values, percent):
    rank = (len(sorted_values) * percent + 99) // 100
    return sorted_values[max(rank, 1) - 1]


def kv(line):
    return dict(re.findall(r"(\w+)=(\S+)", line))


def decide(runs):
    """runs: list of dicts {rep, arm, status, n, p50_us, rtt_p50_us, cpu_us}. Returns (verdict, lines)."""
    lines = []
    valid = {}
    for run in runs:
        if run["status"] == "ok":
            valid.setdefault(run["arm"], []).append(run)
    measured = {arm: reps for arm, reps in valid.items() if len(reps) >= MIN_REPS}

    def p50(arm):
        return statistics.median(r["p50_us"] for r in measured[arm])

    def rng(arm):
        values = [r["p50_us"] for r in measured[arm]]
        return min(values), max(values)

    def med(arm, key):
        values = [r[key] for r in measured[arm] if r.get(key) is not None]
        return statistics.median(values) if values else None

    for arm in ARMS:
        reps = valid.get(arm, [])
        if arm not in measured:
            lines.append(f"  {arm:12s} NOT MEASURED ({len(reps)} valid reps < {MIN_REPS})")
            continue
        low, high = rng(arm)
        counts = ",".join(str(r["n"]) for r in sorted(measured[arm], key=lambda r: r["rep"]))
        lines.append(f"  {arm:12s} n={counts}  P50 {p50(arm):7.2f} us  range [{low:.2f}, {high:.2f}]  "
                     f"rtt P50 {med(arm, 'rtt_p50_us') or 0:7.2f}  cpu {med(arm, 'cpu_us') or 0:6.2f} us/datagram")

    if not all(arm in measured for arm in CONTROL):
        return "NO VERDICT (control not measured)", lines
    a_low, a_high = rng(CONTROL[0])
    b_low, b_high = rng(CONTROL[1])
    aa = abs(p50(CONTROL[0]) - p50(CONTROL[1]))
    overlap = a_low <= b_high and b_low <= a_high
    if aa >= AA_MAX_US or not overlap:
        lines.append(f"CONTROL FAILED: |P50 A/A| = {aa:.2f} us (limit {AA_MAX_US}), ranges overlap: {overlap}")
        return "NO VERDICT (CONTROL FAILED)", lines
    lines.append(f"CONTROL PASS: |P50 A/A| = {aa:.2f} us < {AA_MAX_US}, ranges overlap")
    if BASE not in measured:
        return f"NO VERDICT ({BASE} not measured)", lines
    if BLOCKING in measured:
        lines.append(f"premise: P50({BASE}) - P50({BLOCKING}) = {p50(BASE) - p50(BLOCKING):+.2f} us")
    base_low, _ = rng(BASE)
    gains = []
    noisy_arms = []
    any_measured = False
    for arm in URING:
        if arm not in measured:
            lines.append(f"  {arm}: NOT MEASURED")
            continue
        any_measured = True
        gain = p50(BASE) - p50(arm)
        _, high = rng(arm)
        apart = high < base_low
        ok = gain >= GAIN_US and apart
        width = max(rng(arm)[1] - rng(arm)[0], rng(BASE)[1] - rng(BASE)[0])
        noisy = not ok and width > NOISE_MAX_US
        rtt_b, rtt_u = med(BASE, "rtt_p50_us"), med(arm, "rtt_p50_us")
        rtt_gain = rtt_b - rtt_u if rtt_b is not None and rtt_u is not None else None
        corroborated = rtt_gain is not None and rtt_gain >= GAIN_US / 2
        cpu_b, cpu_u = med(BASE, "cpu_us"), med(arm, "cpu_us")
        cpu_delta = cpu_u - cpu_b if cpu_b is not None and cpu_u is not None else None
        lines.append(
            f"  {arm}: gain {gain:+.2f} us (bar {GAIN_US}), ranges apart: {apart}, widest range {width:.2f} us"
            f" -> {'GAINS' if ok else ('NOISY' if noisy else 'no gain')}"
            f"; rtt gain {rtt_gain if rtt_gain is None else round(rtt_gain, 2)} us"
            f"{'' if not ok else (' (corroborated)' if corroborated else ' (stamp only)')}"
            f"; cpu {'n/a' if cpu_delta is None else f'{cpu_delta:+.2f}'} us/datagram")
        by_rep = {r["rep"]: r["p50_us"] for r in measured[BASE]}
        paired = [round(by_rep[r["rep"]] - r["p50_us"], 2) for r in sorted(measured[arm], key=lambda r: r["rep"])
                  if r["rep"] in by_rep]
        lines.append(f"      paired per rep, p50({BASE}) - p50({arm}) (diagnostic, does not decide): {paired}")
        if ok:
            gains.append((gain, arm, corroborated))
        if noisy:
            noisy_arms.append(arm)
    if not any_measured:
        return "NO VERDICT (no io_uring style measured)", lines
    if not gains and noisy_arms:
        return (f"NO VERDICT (NOISY: {', '.join(noisy_arms)} - a rep range wider than {NOISE_MAX_US} us; re-run)",
                lines)
    if not gains:
        return "DROP (no io_uring style gains >= 0.5 us at p50 over epoll3 with apart rep ranges)", lines
    gain, arm, corroborated = max(gains)
    note = "" if corroborated else " - stamp only, the round trip does not show it: check before building"
    return f"WORTH IMPLEMENTING ({arm}, {gain:+.2f} us at p50){note}", lines


def read_dir(out):
    runs_dir = out + ".runs"
    runs, listing = [], []
    with open(os.path.join(runs_dir, "index.txt"), encoding="utf-8") as index:
        for line in index:
            if not line.startswith("RUN "):
                continue
            meta = kv(line)
            stem, status = meta["stem"], meta["status"]
            run = {"rep": int(meta["rep"]), "arm": meta["arm"], "status": status, "n": 0, "p50_us": None,
                   "rtt_p50_us": None, "cpu_us": None}
            rx_line = tx_line = None
            for name, prefix in (("_rx.log", "RXRESULT "), ("_tx.log", "TXRESULT ")):
                try:
                    with open(os.path.join(runs_dir, stem + name), encoding="utf-8", errors="replace") as log:
                        for log_line in log:
                            if log_line.startswith(prefix):
                                if prefix == "RXRESULT ":
                                    rx_line = kv(log_line)
                                else:
                                    tx_line = kv(log_line)
                except OSError:
                    pass
            samples = []
            try:
                with open(os.path.join(runs_dir, stem + ".samples"), encoding="utf-8") as sample_file:
                    samples = sorted(int(v) for v in sample_file if v.strip())
            except (OSError, ValueError):
                samples = []
            run["n"] = len(samples)
            run["samples"] = samples
            if status == "ok":
                if rx_line is None:
                    run["status"] = "VOID:no_rxresult"
                elif tx_line is None or tx_line.get("status") != "ok":
                    run["status"] = "VOID:no_txresult"
                elif int(rx_line["n"]) != len(samples):
                    run["status"] = f"VOID:witness(rx n={rx_line['n']}, file {len(samples)})"
                elif len(samples) < MIN_N:
                    run["status"] = f"VOID:n<{MIN_N}"
                elif int(rx_line["bad_stamps"]) > MAX_BAD_STAMP_FRACTION * (len(samples) + int(rx_line["bad_stamps"])):
                    run["status"] = f"VOID:bad_stamps={rx_line['bad_stamps']}"
            if run["status"] == "ok":
                run["p50_us"] = nearest_rank(samples, 50) / 1e3
                run["rtt_p50_us"] = int(tx_line["rtt_p50_ns"]) / 1e3
                run["cpu_us"] = int(rx_line["cpu_ns_per"]) / 1e3
            runs.append(run)
            p50_text = "" if run["p50_us"] is None else f" p50 {run['p50_us']:.2f} us"
            listing.append(f"  rep {run['rep']} {run['arm']:12s} n={run['n']:6d} {run['status']}{p50_text}")
    return runs, listing


def pooled(runs):
    lines = []
    for arm in ARMS:
        merged = sorted(v for r in runs if r["arm"] == arm and r["status"] == "ok" for v in r["samples"])
        if merged:
            lines.append(f"  {arm:12s} n={len(merged):7d}  p10 {nearest_rank(merged, 10) / 1e3:7.2f}  "
                         f"p50 {nearest_rank(merged, 50) / 1e3:7.2f}  p99 {nearest_rank(merged, 99) / 1e3:7.2f} us")
    return lines


def report(out):
    runs, listing = read_dir(out)
    print("runs (sample counts first):")
    print("\n".join(listing))
    print("pooled over valid reps (stamp -> return):")
    print("\n".join(pooled(runs)))
    verdict, lines = decide(runs)
    print("per arm, over valid reps:")
    print("\n".join(lines))
    print(f"VERDICT: {verdict}")
    return 0


def synthetic(p50s, aa_shift=0.0, reps=3, n=MIN_N, refused=(), rtt_follow=True):
    """p50s: arm -> centre (us); each rep's p50 is centre + (-0.05, 0, +0.05)."""
    runs = []
    for arm in ARMS:
        centre = p50s.get(arm, p50s["recvfrom"]) + (aa_shift if arm == "recvfrom_aa" else 0.0)
        for rep in range(1, reps + 1):
            if arm in refused:
                runs.append({"rep": rep, "arm": arm, "status": "REFUSED", "n": 0})
                continue
            value = centre + (rep - 2) * 0.05
            status = "ok" if n >= MIN_N else f"VOID:n<{MIN_N}"
            rtt = 50.0 + (value if rtt_follow else p50s["epoll3"])
            runs.append({"rep": rep, "arm": arm, "status": status, "n": n, "p50_us": value, "rtt_p50_us": rtt,
                         "cpu_us": 5.0})
    return runs


def selftest():
    base = {"recvfrom": 13.0, "recvmsg": 13.1, "epoll3": 17.5, "uring": 17.4, "uring_dt": 16.6, "uring_ms": 17.3,
            "uring_ms_dt": 17.2}
    cases = [
        ("uring_dt 0.9 us faster, apart", synthetic(base), "WORTH IMPLEMENTING (uring_dt"),
        ("every uring within 0.4 us", synthetic(dict(base, uring_dt=17.1)), "DROP"),
        ("0.55 us but ranges touch", None, "DROP"),
        ("A/A apart by 0.3 us", synthetic(base, aa_shift=0.3), "NO VERDICT (CONTROL FAILED)"),
        ("A/A within 0.2 but ranges apart", None, "NO VERDICT (CONTROL FAILED)"),
        ("only 2 reps", synthetic(base, reps=2), "NO VERDICT (control not measured)"),
        ("19,999 samples a run", synthetic(base, n=MIN_N - 1), "NO VERDICT (control not measured)"),
        ("every uring refused", synthetic(base, refused=tuple(URING)), "NO VERDICT (no io_uring style measured)"),
        ("epoll3 refused", synthetic(base, refused=("epoll3",)), "NO VERDICT (epoll3 not measured)"),
        ("gain only in the stamp", synthetic(base, rtt_follow=False), "WORTH IMPLEMENTING (uring_dt"),
        ("uring slower than epoll3", synthetic(dict(base, uring_dt=18.5)), "DROP"),
        ("a slow-state run widens uring_dt's range, nothing gains", None, "NO VERDICT (NOISY: uring_dt"),
        ("a slow-state run in epoll3, uring_dt gains anyway", None, "WORTH IMPLEMENTING (uring_dt"),
    ]
    # uring_dt 0.9 us faster in two reps, one rep +5 us (a slow state): median gain 0.85 but ranges overlap and the
    # range is 5 us wide - not "no gain", "could not see".
    slow = synthetic(base)
    for run in slow:
        if run["arm"] == "uring_dt" and run["rep"] == 3:
            run["p50_us"] = 21.6
    cases[-2] = (cases[-2][0], slow, cases[-2][2])
    # epoll3 has a slow rep (range wide) but every uring_dt rep is below epoll3's lowest by >= 0.5: GAINS stands.
    slow_base = synthetic(base)
    for run in slow_base:
        if run["arm"] == "epoll3" and run["rep"] == 3:
            run["p50_us"] = 22.0
    cases[-1] = (cases[-1][0], slow_base, cases[-1][2])
    # Ranges that touch: uring_dt centred 0.55 below epoll3, but one rep reaches into epoll3's lowest rep.
    touching = synthetic(dict(base, uring_dt=16.95))
    for run in touching:
        if run["arm"] == "uring_dt" and run["rep"] == 3:
            run["p50_us"] = 17.5  # max(uring_dt) == min(epoll3) region: not apart
    cases[2] = (cases[2][0], touching, "DROP")
    # A/A medians within 0.2 us but the rep ranges apart (tight reps, 0.2 shift): the control must still fail.
    tight = synthetic(base, aa_shift=0.2)
    for run in tight:
        if run["arm"] in CONTROL:
            run["p50_us"] = 13.0 + (0.2 if run["arm"] == "recvfrom_aa" else 0.0) + (run["rep"] - 2) * 0.01
    cases[4] = (cases[4][0], tight, "NO VERDICT (CONTROL FAILED)")
    failed = 0
    for name, runs, expected in cases:
        verdict, _ = decide(runs)
        ok = verdict.startswith(expected)
        if name == "gain only in the stamp":
            ok = ok and "stamp only" in verdict
        failed += 0 if ok else 1
        print(f"{'PASS' if ok else 'FAIL'}  {name}: {verdict}")
    # The file path end to end: a directory written as the driver writes it, one arm's samples file cut short (the
    # witness must void it) - then the verdict must still come out of the remaining reps.
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "x")
        os.makedirs(out + ".runs")
        with open(os.path.join(out + ".runs", "index.txt"), "w", encoding="utf-8") as index:
            for run in synthetic(base, reps=4):
                stem = f"r{run['rep']}_{run['arm']}"
                index.write(f"RUN rep={run['rep']} arm={run['arm']} stem={stem} status=ok\n")
                count = MIN_N
                centre_ns = int(run["p50_us"] * 1000)
                values = [centre_ns - 500 + (i % 1001) for i in range(count)]
                written = values if not (run["arm"] == "uring_dt" and run["rep"] == 1) else values[:-5]
                with open(os.path.join(out + ".runs", stem + ".samples"), "w", encoding="utf-8") as sample_file:
                    sample_file.write("\n".join(str(v) for v in written) + "\n")
                with open(os.path.join(out + ".runs", stem + "_rx.log"), "w", encoding="utf-8") as rx_log:
                    rx_log.write(f"RXRESULT style={run['arm']} n={count} bad_stamps=0 cpu_ns_per=5000\n")
                with open(os.path.join(out + ".runs", stem + "_tx.log"), "w", encoding="utf-8") as tx_log:
                    tx_log.write(f"TXRESULT status=ok rtt_p50_ns={int(run['rtt_p50_us'] * 1000)}\n")
        runs, _ = read_dir(out)
        voided = [r for r in runs if r["status"].startswith("VOID:witness")]
        verdict, _ = decide(runs)
        ok = len(voided) == 1 and verdict.startswith("WORTH IMPLEMENTING (uring_dt")
        failed += 0 if ok else 1
        print(f"{'PASS' if ok else 'FAIL'}  files end to end, one cut samples file voided: {len(voided)} void, {verdict}")
    print(f"selftest: {'PASS' if failed == 0 else f'FAIL ({failed})'}")
    return 1 if failed else 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--prereg":
        print(PREREG)
        return 0
    if len(argv) == 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) == 2 and not argv[1].startswith("-"):
        return report(argv[1])
    print(__doc__, file=sys.stderr)
    return 64


if __name__ == "__main__":
    sys.exit(main(sys.argv))
