#!/usr/bin/env python3
"""Reader of c12d1_queue.sh (recv_wake_xhost_rig.sh with echo-socket arms): does a send from the socket the receiver
then waits on make the next wake and return slower?

Usage:
    c12d1_read.py OUT          read OUT.runs/ (index.txt, <stem>_rx.log, <stem>_tx.log, <stem>.samples)
    c12d1_read.py --prereg     print the reading rules this file enforces
    c12d1_read.py --selftest   check the rules against synthetic runs whose verdict is known

Why (C12-D1, 2026-10-29): in the rig's KH2B pong records (libc arm) TickLE's tap -> ring return p50 is 15.0-16.0 us when
the receiving thread sent within 1 ms before waiting - it sends from data_sock, the socket its DEFER_TASKRUN ring waits
on - and 12.5-14.0 us when its last send was >= 20 ms earlier; the vendors are at 12.5-14.0. The micro harness echoed
from a second socket, so it never had that condition. recv_wake_xhost.c's `same` echo mode adds it.

The rules (PREREG) were written on 2026-10-29, before any rig run with the echo mode, and each one is code below.
Run validity (sample count, the samples file against the receiver's own count, bad stamps) is recv_wake_xhost_read.py's
read_dir(), unchanged; this file adds the echo-mode witness and the decision.
"""

import os
import statistics
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recv_wake_xhost_read as base  # noqa: E402  pylint: disable=wrong-import-position

# --- pre-registered constants -------------------------------------------------------------------------------------
MIN_N = base.MIN_N  # 20000 samples a run
MIN_REPS = 3  # valid reps every decided arm needs
AA_MAX_US = 0.3  # |P50(recvfrom) - P50(recvfrom_aa)| above this: the instrument cannot see the effect -> VOID
RISE_US = 1.0  # a rise this large is the effect (the pong split's 15.0-16.0 against 12.5-14.0 is ~2 us)
FLAT_US = 0.3  # a delta below this is no rise
CONTROL = ("recvfrom", "recvfrom_aa")
RING = "prod_dt"  # production's ring wait (DEFER_TASKRUN, IORING_OP_RECV)
READER = "recvmsg"  # a blocking recvmsg (CycloneDDS's receive thread): the non-ring reader
DIAGNOSTIC = ("prod_dt_msg",)  # production's IORING_OP_RECVMSG: reported, does not decide
MECHANISMS = (RING, READER) + DIAGNOSTIC
ARMS = list(CONTROL) + [f"{m}@{e}" for m in MECHANISMS for e in ("same", "second")]
REQUIRED = list(CONTROL) + [f"{m}@{e}" for m in (RING, READER) for e in ("same", "second")]

PREREG = f"""C12-D1 reading rules (pre-registered 2026-10-29, enforced by c12d1_read.py):
Arms: {' '.join(ARMS)}. <style>@same echoes from the socket the receiver waits on, <style>@second from a second socket.
VOID run (recv_wake_xhost_read.py read_dir): not ok, n < {MIN_N}, the samples file's count differs from RXRESULT n,
  bad stamps > 0.1%; and here: an @ arm whose RXRESULT does not say echo=<its mode> (the arm must report its treatment).
1. Counts first: every arm of {', '.join(REQUIRED)} needs >= {MIN_REPS} valid reps, else VERDICT VOID (short counts).
2. Per arm P50 = the median over its valid reps of the rep's p50 (stamp -> return).
3. Control: AA = P50(recvfrom) - P50(recvfrom_aa); |AA| > {AA_MAX_US} us -> VERDICT VOID (A/A).
   |AA| is the floor, printed.
4. Per mechanism delta = P50(<m>@same) - P50(<m>@second) (positive: the same-socket send is slower).
5. {RING} delta >= {RISE_US} and {READER} delta <= {FLAT_US}                 -> RING-SPECIFIC (go to the fix)
   {RING} delta >= {RISE_US} and {READER} delta >= {RISE_US}                 -> ANY-READER (go to the fix)
   {RING} delta < {FLAT_US} and {READER} delta < {FLAT_US}                   -> FALSIFIED (do not build the fix)
   anything else                                                    -> UNDECIDED
6. {', '.join(DIAGNOSTIC)}: delta printed, not read. Paired per-rep deltas printed, not read.
Exit status: 0 for a decided or UNDECIDED verdict, 2 for VOID, 64 usage."""


def check_echo(out, runs):
    """The arm reports its treatment: an @ arm's RXRESULT must end in echo=<its mode>."""
    runs_dir = out + ".runs"
    for run in runs:
        if run["status"] != "ok" or "@" not in run["arm"]:
            continue
        mode = run["arm"].rsplit("@", 1)[1]
        seen = None
        try:
            with open(os.path.join(runs_dir, f"r{run['rep']}_{run['arm']}_rx.log"), encoding="utf-8",
                      errors="replace") as log:
                for line in log:
                    if line.startswith("RXRESULT "):
                        seen = base.kv(line).get("echo")
                        break
        except OSError:
            pass
        if seen != mode:
            run["status"] = f"VOID:echo({seen} for {mode})"
            run["p50_us"] = None


def decide(runs):
    """runs: list of dicts {rep, arm, status, p50_us}. Returns (verdict, lines)."""
    lines = []
    valid = {}
    for run in runs:
        if run["status"] == "ok" and run.get("p50_us") is not None:
            valid.setdefault(run["arm"], []).append(run)
    for arm in ARMS:
        reps = sorted(valid.get(arm, []), key=lambda r: r["rep"])
        if reps:
            values = ", ".join(f"{r['p50_us']:.2f}" for r in reps)
            lines.append(f"  {arm:20s} valid reps {len(reps)}  P50 {statistics.median(r['p50_us'] for r in reps):7.2f}"
                         f" us  rep p50s [{values}]")
        else:
            lines.append(f"  {arm:20s} valid reps 0")
    short = [arm for arm in REQUIRED if len(valid.get(arm, [])) < MIN_REPS]
    if short:
        return f"VOID (short counts: {', '.join(short)} with < {MIN_REPS} valid reps)", lines

    def p50(arm):
        return statistics.median(r["p50_us"] for r in valid[arm])

    aa = p50("recvfrom") - p50("recvfrom_aa")
    lines.append(f"  A/A recvfrom - recvfrom_aa = {aa:+.2f} us (floor |AA| = {abs(aa):.2f}, limit {AA_MAX_US})")
    if abs(aa) > AA_MAX_US:
        return f"VOID (A/A control {aa:+.2f} us, |AA| > {AA_MAX_US})", lines
    deltas = {}
    for mech in MECHANISMS:
        same, second = f"{mech}@same", f"{mech}@second"
        if len(valid.get(same, [])) < MIN_REPS or len(valid.get(second, [])) < MIN_REPS:
            lines.append(f"  {mech}: NOT MEASURED (diagnostic)")
            continue
        deltas[mech] = p50(same) - p50(second)
        by_rep = {r["rep"]: r["p50_us"] for r in valid[second]}
        paired = [round(r["p50_us"] - by_rep[r["rep"]], 2) for r in sorted(valid[same], key=lambda r: r["rep"])
                  if r["rep"] in by_rep]
        role = " (diagnostic, does not decide)" if mech in DIAGNOSTIC else ""
        lines.append(f"  {mech}: delta same - second = {deltas[mech]:+.2f} us{role}; paired per rep {paired}")
    ring, reader = deltas[RING], deltas[READER]
    text = f"{RING} {ring:+.2f} us, {READER} {reader:+.2f} us, floor {abs(aa):.2f}"
    if ring >= RISE_US and reader <= FLAT_US:
        return f"RING-SPECIFIC ({text})", lines
    if ring >= RISE_US and reader >= RISE_US:
        return f"ANY-READER ({text})", lines
    if ring < FLAT_US and reader < FLAT_US:
        return f"FALSIFIED ({text})", lines
    return f"UNDECIDED ({text})", lines


def report(out):
    runs, listing = base.read_dir(out)
    check_echo(out, runs)
    print("runs (sample counts first):")
    for run in runs:
        p50_text = "" if run["p50_us"] is None else f" p50 {run['p50_us']:.2f} us"
        print(f"  rep {run['rep']} {run['arm']:20s} n={run['n']:6d} {run['status']}{p50_text}")
    verdict, lines = decide(runs)
    print("per arm, over valid reps:")
    print("\n".join(lines))
    print(f"VERDICT: {verdict}")
    return 2 if verdict.startswith("VOID") else 0


def synthetic(centres, reps=3, drop=()):
    """centres: arm -> p50 (us), default 8.40; each rep's p50 is centre + (-0.05, 0, +0.05). drop: (rep, arm) VOID."""
    runs = []
    for arm in ARMS:
        for rep in range(1, reps + 1):
            p50 = centres.get(arm, 8.40) + (-0.05, 0.0, 0.05)[(rep - 1) % 3]
            status = "VOID:n<20000" if (rep, arm) in drop else "ok"
            runs.append({"rep": rep, "arm": arm, "status": status, "n": MIN_N,
                         "p50_us": None if status != "ok" else p50})
    return runs


def selftest():
    def arms(ring_same=8.40, reader_same=8.40, aa=0.0):
        return {"prod_dt@second": 8.40, "prod_dt@same": ring_same, "recvmsg@second": 8.40, "recvmsg@same": reader_same,
                "recvfrom_aa": 8.40 - aa}

    cases = [
        ("ring rises 2.0, recvmsg flat", synthetic(arms(10.40, 8.50)), "RING-SPECIFIC"),
        ("both rise 1.5", synthetic(arms(9.90, 9.90)), "ANY-READER"),
        ("neither rises", synthetic(arms(8.55, 8.30)), "FALSIFIED"),
        ("ring rises 0.6 only", synthetic(arms(9.00, 8.40)), "UNDECIDED"),
        ("ring 2.0, recvmsg 0.6", synthetic(arms(10.40, 9.00)), "UNDECIDED"),
        ("recvmsg rises 2.0, ring flat", synthetic(arms(8.40, 10.40)), "UNDECIDED"),
        ("A/A 0.5 apart", synthetic(arms(10.40, 8.40, aa=0.5)), "VOID (A/A"),
        ("A/A 0.25 apart, ring rises", synthetic(arms(10.40, 8.40, aa=0.25)), "RING-SPECIFIC"),
        ("2 reps only", synthetic(arms(10.40, 8.40), reps=2), "VOID (short"),
        ("one recvmsg@same rep VOID of 3", synthetic(arms(10.40, 8.40), drop={(2, "recvmsg@same")}), "VOID (short"),
        ("prod_dt_msg missing: diagnostic only",
         [r for r in synthetic(arms(10.40, 8.40)) if not r["arm"].startswith("prod_dt_msg")], "RING-SPECIFIC"),
        ("prod_dt_msg rises alone: does not decide", synthetic({**arms(), "prod_dt_msg@same": 11.0}), "FALSIFIED"),
    ]
    failed = 0
    for name, runs, expect in cases:
        verdict, _ = decide(runs)
        ok = verdict.startswith(expect)
        failed += 0 if ok else 1
        print(f"{'PASS' if ok else 'FAIL'}  {name}: {verdict} (expected {expect})")
    # End to end through the files: one @same run whose receiver reports echo=second must be VOID, and with 3 reps
    # that leaves its arm short.
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "c12d1_selftest")
        os.makedirs(out + ".runs")
        runs = synthetic(arms(10.40, 8.40))
        with open(os.path.join(out + ".runs", "index.txt"), "w", encoding="utf-8") as index:
            for run in runs:
                stem = f"r{run['rep']}_{run['arm']}"
                index.write(f"RUN rep={run['rep']} arm={run['arm']} stem={stem} status=ok\n")
                centre = int(run["p50_us"] * 1000)
                values = [centre] * MIN_N
                with open(os.path.join(out + ".runs", stem + ".samples"), "w", encoding="utf-8") as samples:
                    samples.write("\n".join(str(v) for v in values) + "\n")
                mode = run["arm"].rsplit("@", 1)[1] if "@" in run["arm"] else "second"
                if stem == "r2_prod_dt@same":
                    mode = "second"  # the treatment not applied
                style = run["arm"].split("@")[0]
                with open(os.path.join(out + ".runs", stem + "_rx.log"), "w", encoding="utf-8") as rx_log:
                    rx_log.write(f"RXRESULT style={style} n={MIN_N} bad_stamps=0 cpu_ns_per=5000 echo={mode}\n")
                with open(os.path.join(out + ".runs", stem + "_tx.log"), "w", encoding="utf-8") as tx_log:
                    tx_log.write("TXRESULT status=ok rtt_p50_ns=180000\n")
        read, _ = base.read_dir(out)
        clean_verdict, _ = decide([dict(r) for r in read])
        check_echo(out, read)
        voided = [r for r in read if r["status"].startswith("VOID:echo")]
        verdict, _ = decide(read)
        ok = clean_verdict.startswith("RING-SPECIFIC") and len(voided) == 1 and verdict.startswith("VOID (short")
        failed += 0 if ok else 1
        print(f"{'PASS' if ok else 'FAIL'}  files end to end: before the echo witness {clean_verdict}; "
              f"{len(voided)} echo-VOID; after {verdict}")
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
