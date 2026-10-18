#!/usr/bin/env python3
"""Which side lost a round trip, for every lossy rep of an rmw_crosshost_rtt.sh run (2026-10-18).

Usage: rmw_loss_attribution.py <run.txt> [<run.txt.logs>]   (the logs directory defaults to <run.txt>.logs)
       rmw_loss_attribution.py --selftest

Question. The J run at 18f16b85 (~/rig_results_safe/creation_rtt_rmwJ_18f16b85_20261018-001954.*) had 13 VOID(loss)
reps, each sent=95 recv=94 (one 500 ms reply wait costs five 100 ms slots, so 95 sent means exactly one unanswered
ping, anywhere in the window). A RELIABLE round trip that never comes back is either a start-up artefact or a loss.

Evidence read per rep, all from the rep's own files:
  lost        sent - recv from the row's RESULT.
  unmatched   rmw_tickle's pong: tx_unmatched_dropped + tx_unmatched_kept from its traffic line - replies it published
              while its publisher had no matched reader (4fbfe459: such a publish sends nothing; a RELIABLE one is
              kept in a history the VOLATILE ping reader is not owed). Absent counters are "could not look", never 0.
  order       whether the pong log's first "no matched reader" refusal comes before "Publisher peer registered"
              (the pong's publisher learning the ping's subscriber) - i.e. at start-up, before the match.
  pong_rx     the pong subscriber's delivered= count: did the pong see every ping that was sent?

Reading rules (enforced below, not only stated):
  STARTUP     rmw_tickle, lost == unmatched >= 1, the refusal precedes the match, and pong_rx == sent: the pong had
              every ping, and the only replies missing are the ones it published before it knew the ping's reader.
              The first ping is the only one that can arrive before that match on a run whose pings are 100 ms
              apart; with one lost it is seq 1.
  UNATTRIBUTED  rmw_tickle with any other combination - a loss this rule does not explain. Exit status 1.
  NO COUNTER  another rmw: no such counter exists, reported with its counts and not attributed.
  COULD NOT LOOK  a log or a counter missing. Exit status 2 (unless an UNATTRIBUTED already made it 1).
Control: every rmw_tickle rep with no loss must have unmatched == 0. A loss-free rep with unmatched > 0 would mean
the counter rises without a loss, so it could not decide; that is reported as CONTROL FAIL and exits 1.
  WARM-UP (added 2026-10-18, after the d0650110 J run, before any run read with it): since d0650110 ping_node sends
  warm-up pings (seq 0, never counted) every 20 ms until one is answered, and prints "WARMUP: warmup=on
  warmup_sent=<n>" in the row. A warm-up ping the pong answers before its publisher has matched the ping's reader is
  refused (unmatched += 1) and the ping sends the next one, so a loss-free rep with warmup=on may carry up to
  warmup_sent - 1 unmatched replies, and those are expected, not a control failure: on that run the 3 reps of 48
  that read unmatched=1 are exactly the 3 with warmup_sent=2. The teeth stay: the allowance needs warmup=on, the
  refusal before the match (as STARTUP), and unmatched <= warmup_sent - 1. Unmatched beyond that, unmatched with no
  warm-up (warmup=off, warmup_sent=1, or a row from before d0650110 with no WARMUP: line), or a refusal after the
  match is still CONTROL FAIL. Every allowed rep is printed as WARMUP OK with its counts.
"""

import re
import sys
from pathlib import Path

ROW = re.compile(r"^(rmw_\w+) (\S+) (best_effort|reliable) rep(\d+)(.*?) \| (.+?) \| (.*)$")  # a verdict can hold spaces
UNMATCHED = re.compile(r"tx_unmatched_dropped=(\d+) tx_unmatched_kept=(\d+)")
DELIVERED = re.compile(r"Subscriber \d+ delivery: delivered=(\d+)")


def stem_of(rmw, msg, qos, rep, tags):
    # rmw_crosshost_rtt.sh's stem: ${WAITSTEM} (_block when not poll) and then _ps<N>
    wait = re.search(r"wait=(\w+)", tags)
    ps = re.search(r"poll_sleep_us=(\d+)", tags)
    return (
        f"{rmw}_{msg}_{qos}_rep{rep}"
        + (f"_{wait.group(1)}" if wait and wait.group(1) != "poll" else "")
        + (f"_ps{ps.group(1)}" if ps else "")
    )


def pong_facts(path):
    """(unmatched or None, refusal_before_match or None, pong_rx or None) from one pong log."""
    if not path.is_file():
        return None, None, None
    text = path.read_text(errors="replace")
    um = UNMATCHED.search(text)
    unmatched = int(um.group(1)) + int(um.group(2)) if um else None
    refusal = text.find("no matched reader, so this publish sends nothing")
    match = text.find("Publisher peer registered")
    order = None if refusal < 0 or match < 0 else refusal < match
    rx = [int(x) for x in DELIVERED.findall(text)]
    return unmatched, order, (max(rx) if rx else None)


def warmup_allowance(f):
    """Unmatched replies a loss-free rep may carry from its warm-up (WARM-UP above): warmup_sent - 1 when the row's
    WARMUP: line says warmup=on, else 0 (no line, warmup=off, or one warm-up ping)."""
    if f.get("warmup") != "on" or not f.get("warmup_sent", "").isdigit():
        return 0
    return max(0, int(f["warmup_sent"]) - 1)


def main(argv):
    if argv[1:2] == ["--selftest"]:
        return selftest()
    if len(argv) not in (2, 3):
        print(__doc__)
        return 64
    run = Path(argv[1])
    logs = Path(argv[2]) if len(argv) == 3 else Path(str(run) + ".logs")
    status = 0
    counts = {}
    control = {"reps": 0, "fail": 0, "unread": 0, "warmup": 0}
    for line in run.read_text(errors="replace").splitlines():
        m = ROW.match(line)
        if not m:
            continue
        rmw, msg, qos, rep, tags, verdict, rest = m.groups()
        f = dict(re.findall(r"(\w+)=(\S+)", rest))
        if "sent" not in f or "recv" not in f:
            continue
        sent, recv = int(f["sent"]), int(f["recv"])
        lost = sent - recv
        stem = stem_of(rmw, msg, qos, rep, tags)
        name = f"{rmw} {msg} {qos} rep{rep}{tags} [{verdict}]"
        if not rmw.startswith("rmw_tickle"):
            if lost:
                print(f"NO COUNTER      {name}: sent={sent} recv={recv} lost={lost}")
                counts["NO COUNTER"] = counts.get("NO COUNTER", 0) + 1
            continue
        unmatched, order, pong_rx = pong_facts(logs / f"{stem}_pong.log")
        if not lost:
            control["reps"] += 1
            if unmatched is None:
                control["unread"] += 1
            elif unmatched:
                allow = warmup_allowance(f)
                if unmatched <= allow and order:
                    control["warmup"] += 1
                    print(
                        f"WARMUP OK       {name}: no loss, pong unmatched={unmatched} <= warmup_sent-1={allow}, "
                        f"refusal_before_match={order}"
                    )
                else:
                    control["fail"] += 1
                    print(
                        f"CONTROL FAIL    {name}: no loss but pong unmatched={unmatched} (warm-up allows {allow}, "
                        f"refusal_before_match={order})"
                    )
            continue
        facts = f"sent={sent} recv={recv} lost={lost} pong_unmatched={unmatched} refusal_before_match={order} pong_rx={pong_rx}"
        if unmatched is None or order is None or pong_rx is None:
            kind = "COULD NOT LOOK"
            status = status or 2
        elif lost == unmatched and order and pong_rx == sent:
            kind = "STARTUP"
        else:
            kind = "UNATTRIBUTED"
            status = 1
        counts[kind] = counts.get(kind, 0) + 1
        print(f"{kind:<15} {name}: {facts}")
    print(
        f"control: {control['reps']} loss-free rmw_tickle reps, {control['fail']} with unmatched the warm-up does "
        f"not explain, {control['warmup']} with unmatched from the warm-up, {control['unread']} without the counter"
    )
    if control["fail"]:
        status = 1
    print("summary: " + (", ".join(f"{k} {v}" for k, v in sorted(counts.items())) or "no lossy rep"))
    return status


def selftest():
    """Fixture rows and pong logs with known answers (no rig), through main() itself."""
    import contextlib
    import io
    import tempfile

    fails = []
    pong = (
        "[t] [INFO] Whole-record send refused: no matched reader, so this publish sends nothing\n"
        "[t] [INFO] Publisher peer registered: node 3 at 192.168.10.3:1 for endpoint 1\n"
        "[t] [INFO] Subscriber 1 delivery: delivered={rx} out_of_order=0\n"
        "[t] [INFO] Node 2 traffic: tx_unmatched_dropped=0 tx_unmatched_kept={um} x=0\n"
    )
    pong_late = (  # the same lines with the refusal AFTER the match: not a start-up refusal
        "[t] [INFO] Publisher peer registered: node 3 at 192.168.10.3:1 for endpoint 1\n"
        "[t] [INFO] Whole-record send refused: no matched reader, so this publish sends nothing\n"
        "[t] [INFO] Subscriber 1 delivery: delivered={rx} out_of_order=0\n"
        "[t] [INFO] Node 2 traffic: tx_unmatched_dropped=0 tx_unmatched_kept={um} x=0\n"
    )
    # (name, warmup tail of the row, sent, recv, pong unmatched, pong log template, expected status, expected text)
    cases = [
        ("warm-up 2 pings, unmatched 1: expected", "WARMUP: warmup=on warmup_sent=2 warmup_ms=22.4", 100, 100, 1,
         pong, 0, "WARMUP OK"),
        ("warm-up 3 pings, unmatched 2: expected", "WARMUP: warmup=on warmup_sent=3 warmup_ms=42.0", 100, 100, 2,
         pong, 0, "WARMUP OK"),
        ("warm-up 2 pings, unmatched 2: beyond it", "WARMUP: warmup=on warmup_sent=2 warmup_ms=22.4", 100, 100, 2,
         pong, 1, "CONTROL FAIL"),
        ("warm-up 1 ping, unmatched 1: not explained", "WARMUP: warmup=on warmup_sent=1 warmup_ms=2.3", 100, 100, 1,
         pong, 1, "CONTROL FAIL"),
        ("warmup=off, unmatched 1", "WARMUP: warmup=off warmup_sent=0 warmup_ms=0.0", 100, 100, 1, pong, 1,
         "CONTROL FAIL"),
        ("no WARMUP: line (before d0650110), unmatched 1", "", 100, 100, 1, pong, 1, "CONTROL FAIL"),
        ("warm-up 2 pings, unmatched 1, refusal after the match", "WARMUP: warmup=on warmup_sent=2", 100, 100, 1,
         pong_late, 1, "CONTROL FAIL"),
        ("no loss, unmatched 0", "WARMUP: warmup=on warmup_sent=1", 100, 100, 0, pong, 0, "0 with unmatched the"),
        ("a STARTUP loss is still STARTUP", "", 95, 94, 1, pong, 0, "STARTUP"),
    ]
    for name, tail, sent, recv, um, tmpl, want_status, want_text in cases:
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / "run.txt"
            logs = Path(tmp) / "run.txt.logs"
            logs.mkdir()
            run.write_text(
                f"rmw_tickle bench reliable rep1 poll_sleep_us=0 | ok | framework=rmw_tickle sent={sent} recv={recv} "
                f"loss_pct=0 {tail} pong_unmatched={um}\n"
            )
            (logs / "rmw_tickle_bench_reliable_rep1_ps0_pong.log").write_text(tmpl.format(rx=sent, um=um))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                got = main(["x", str(run)])
            ok = got == want_status and want_text in out.getvalue()
            print(f"  {'ok  ' if ok else 'FAIL'} {name}: exit {got} (want {want_status}), '{want_text}' printed")
            if not ok:
                print("    " + out.getvalue().replace("\n", "\n    "))
                fails.append(name)
    print(f"selftest: {'PASS' if not fails else 'FAIL (' + str(len(fails)) + ')'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
