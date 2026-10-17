#!/usr/bin/env python3
"""Which side lost a round trip, for every lossy rep of an rmw_crosshost_rtt.sh run (2026-10-18).

Usage: rmw_loss_attribution.py <run.txt> [<run.txt.logs>]   (the logs directory defaults to <run.txt>.logs)

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


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__)
        return 64
    run = Path(argv[1])
    logs = Path(argv[2]) if len(argv) == 3 else Path(str(run) + ".logs")
    status = 0
    counts = {}
    control = {"reps": 0, "fail": 0, "unread": 0}
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
                control["fail"] += 1
                print(f"CONTROL FAIL    {name}: no loss but pong unmatched={unmatched}")
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
        f"control: {control['reps']} loss-free rmw_tickle reps, {control['fail']} with unmatched > 0, "
        f"{control['unread']} without the counter"
    )
    if control["fail"]:
        status = 1
    print("summary: " + (", ".join(f"{k} {v}" for k, v in sorted(counts.items())) or "no lossy rep"))
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
