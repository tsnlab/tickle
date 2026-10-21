#!/usr/bin/env python3
"""service_cell_verdict.py - the reading of one service-cell row (tickle/service_call), written before any rig run.

One implementation for every harness that runs the cell: campaign_sweep.sh (shape S, on the rig) and rig_preflight.sh
(the same cells in two private namespaces on this PC). Each passes the two RESULT lines on stdin, the cell's network
condition, the arguments the client ran with and, at N4, what the shaping dropped; this prints ONE token, no spaces:

  ok                         the row is read
  DEFECT(dup_exec:<n>_first_k:<k>)
                             the server's callback answered some call more than once: the falsifier of the cell
                             (fb065d65's defect - a retry that found no kept answer ran the service again). Never VOID:
                             a duplicate is a finding about the code, whatever else is wrong with the row, and it must
                             not read like "could not look". Only a row without the server's line cannot say it.
  VOID(<why>)                the row cannot be read, for the first of these reasons, in this order:

THE RULES (implemented in verdict() below; the order is the order they are checked):
  R1 lines        a client line (role=client) and a server line (role=server) - else VOID(no_client_line /
                  no_server_line). The server's line is also what the DEFECT check needs.
  R2 bookkeeping  client sent > 0, recv > 0, and unmatched, mismatched, bad_rc, call_errors all 0: an answer handed to
                  no call or to the wrong one, or a call core refused for anything but a full window, means the
                  client's counts are not counts of calls - VOID(bookkeeping:<field>).
  R3 judged       server unjudged == 0: a call whose counter slot a later call had taken cannot be judged for
                  duplicates - VOID(unjudged:<n>).
  R4 control      the duplicate counter's own control (service_call/server.c, probes): client probes >= 1; server
                  probe_ok >= 1, probe_bad == 0, probe_ok >= the client's probes_answered, ignored >= probe_ok. A run in
                  which a second execution of one call, sent as a retry across the link, was not counted on that call
                  cannot show that no call ran twice - VOID(control:<what>).
  R5 cross-role   server recv (calls answered at least once) >= client recv (answers taken): every answer the client
                  took was produced by a callback the server counted - VOID(cross_role:server_<s>_lt_client_<c>).
  R6 count        sample count before any value: measured (round trips in the window) >= the floor - at N0 with a
                  fixed rate, half the calls the schedule makes in -d (a lossless link carries its schedule); at N0
                  with -r 0, 1,000; otherwise 20 (under loss a collapse of the call rate is a result, read from
                  calls_per_s=, not a void) - VOID(count:measured_<n>_lt_<floor>).
  R7 treatment    N4 must show what it did: --tc carries the shaping's own drop counters over the run (egress netem
                  and ingress gact, campaign_sweep.sh / rig_preflight.sh read them), and both must be > 0 -
                  VOID(treatment:<which>_dropped_0). At N0, a nonzero counter is VOID(treatment:shaped_at_N0).
DEFECT is decided first, from the server line alone, and wins over every VOID.

  service_cell_verdict.py --net N0|N4 --args "<client args>" [--tc "egress_dropped=<n> ingress_dropped=<n>"] < lines
  service_cell_verdict.py --selftest
"""
import re
import sys

FLOOR_ANY = 20
FLOOR_MAX_RATE = 1000
FIXED_RATE_SHARE = 0.5


def fields(line):
    return dict(re.findall(r"([A-Za-z_]\w*)=(\S+)", line))


def num(d, key):
    try:
        return float(d[key])
    except (KeyError, ValueError):
        return None


def client_arg(args, flag, default):
    """The last value of flag in args (the clients take the last one given)."""
    found = default
    words = args.split()
    for i, w in enumerate(words[:-1]):
        if w == flag:
            try:
                found = float(words[i + 1])
            except ValueError:
                pass
    return found


def floor_for(net, args):
    rate = client_arg(args, "-r", 1000.0)
    dur = client_arg(args, "-d", 5.0)
    if net == "N0" and rate > 0:
        return max(FLOOR_ANY, int(FIXED_RATE_SHARE * rate * dur))
    if net == "N0":
        return FLOOR_MAX_RATE
    return FLOOR_ANY


def verdict(text, net, args, tc=""):
    client, server = None, None
    for raw in text.splitlines():
        # A row of campaign_sweep.sh carries both lines joined; split them back at each role=.
        for chunk in re.split(r"(?=role=)", raw):
            f = fields(chunk)
            if f.get("role") == "client" and client is None:
                client = f
            elif f.get("role") == "server" and server is None:
                server = f
    if server is not None:
        dup = num(server, "dup_exec")
        if dup is not None and dup > 0:
            return f"DEFECT(dup_exec:{dup:.0f}_first_k:{server.get('dup_first_k', '?')})"
    if client is None:
        return "VOID(no_client_line)"
    if server is None:
        return "VOID(no_server_line)"
    for key, want in (("sent", "pos"), ("recv", "pos"), ("unmatched", 0), ("mismatched", 0), ("bad_rc", 0),
                      ("call_errors", 0)):
        v = num(client, key)
        if v is None or (want == "pos" and v <= 0) or (want != "pos" and v != want):
            return f"VOID(bookkeeping:{key}_{client.get(key, 'absent')})"
    unj = num(server, "unjudged")
    if unj is None or unj != 0:
        return f"VOID(unjudged:{server.get('unjudged', 'absent')})"
    probes, answered = num(client, "probes"), num(client, "probes_answered")
    ok, bad, ignored = num(server, "probe_ok"), num(server, "probe_bad"), num(server, "ignored")
    if None in (probes, answered, ok, bad, ignored):
        return "VOID(control:fields_absent)"
    if probes < 1:
        return "VOID(control:no_probe_made)"
    if ok < 1 or bad != 0:
        return f"VOID(control:probe_ok_{ok:.0f}_probe_bad_{bad:.0f})"
    if ok < answered:
        return f"VOID(control:probe_ok_{ok:.0f}_lt_client_probes_answered_{answered:.0f})"
    if ignored < ok:
        return f"VOID(control:ignored_{ignored:.0f}_lt_probe_ok_{ok:.0f})"
    srecv, crecv = num(server, "recv"), num(client, "recv")
    if srecv is None or srecv < crecv:
        return f"VOID(cross_role:server_{server.get('recv', 'absent')}_lt_client_{crecv:.0f})"
    measured = num(client, "measured")
    floor = floor_for(net, args)
    if measured is None or measured < floor:
        return f"VOID(count:measured_{client.get('measured', 'absent')}_lt_{floor})"
    drops = {k: int(v) for k, v in re.findall(r"(egress|ingress)_dropped=(\d+)", tc)}
    if net == "N4":
        for which in ("egress", "ingress"):
            if drops.get(which, 0) <= 0:
                return f"VOID(treatment:{which}_dropped_{drops.get(which, 'absent')})"
    elif any(v > 0 for v in drops.values()):
        return "VOID(treatment:shaped_at_N0)"
    return "ok"


def selftest():
    c = ("RESULT: framework=tickle scenario=service_call role=client rate_hz=1000.0 sent=9000 recv=9000 timeouts=0 "
         "refused=0 call_errors=0 mismatched=0 unmatched=0 bad_rc=0 probes=250 probes_answered=250 measured=5000 "
         "calls_per_s=1000.000 rtt_avg_ms=0.04")
    s = ("RESULT: framework=tickle scenario=service_call role=server recv=9000 runs=9250 ignored=250 dup_exec=0 "
         "dup_first_k=0 exec_max=1 probe_ok=250 probe_bad=0 unjudged=0 win_recv=5000")
    a = "-r 1000 -d 5 --warmup-s 2 --cooldown-s 2"
    tc4 = "egress_dropped=300 ingress_dropped=280"
    cases = [
        ("base N0", f"{c}\n{s}", "N0", a, "", "ok"),
        ("base, both lines joined as campaign_sweep.sh prints them", f"{c} {s}", "N0", a, "", "ok"),
        ("base N4 with both drop counters", f"{c}\n{s}", "N4", a, tc4, "ok"),
        ("dup_exec 2 -> DEFECT", f"{c}\n{s.replace('dup_exec=0 dup_first_k=0', 'dup_exec=2 dup_first_k=64')}",
         "N0", a, "", "DEFECT(dup_exec:2_first_k:64)"),
        ("DEFECT wins over a VOID (no client line)", s.replace("dup_exec=0", "dup_exec=1"), "N0", a, "",
         "DEFECT(dup_exec:1_first_k:0)"),
        ("no server line", c, "N0", a, "", "VOID(no_server_line)"),
        ("no client line", s, "N0", a, "", "VOID(no_client_line)"),
        ("an unmatched callback", f"{c.replace('unmatched=0', 'unmatched=1')}\n{s}", "N0", a, "",
         "VOID(bookkeeping:unmatched_1)"),
        ("unjudged", f"{c}\n{s.replace('unjudged=0', 'unjudged=3')}", "N0", a, "", "VOID(unjudged:3)"),
        ("control: a probe answered on a third run", f"{c}\n{s.replace('probe_bad=0', 'probe_bad=1')}", "N0", a, "",
         "VOID(control:probe_ok_250_probe_bad_1)"),
        ("control: no probe made", f"{c.replace('probes=250 probes_answered=250', 'probes=0 probes_answered=0')}\n{s}",
         "N0", a, "", "VOID(control:no_probe_made)"),
        ("control: the server saw fewer second runs than the client got answers",
         f"{c}\n{s.replace('probe_ok=250', 'probe_ok=249')}", "N0", a, "",
         "VOID(control:probe_ok_249_lt_client_probes_answered_250)"),
        ("cross-role", f"{c}\n{s.replace('recv=9000', 'recv=8999')}", "N0", a, "",
         "VOID(cross_role:server_8999_lt_client_9000)"),
        ("count at N0 1 kHz: 2499 < 2500", f"{c.replace('measured=5000', 'measured=2499')}\n{s}", "N0", a, "",
         "VOID(count:measured_2499_lt_2500)"),
        ("count under loss: a collapse to 61 is read, not voided",
         f"{c.replace('measured=5000', 'measured=61')}\n{s}", "N4", a, tc4, "ok"),
        ("count at N0 max rate", f"{c.replace('measured=5000', 'measured=999')}\n{s}", "N0", "-r 0 -d 5", "",
         "VOID(count:measured_999_lt_1000)"),
        ("count at N0 10 Hz -d 10: 50 is enough", f"{c.replace('measured=5000', 'measured=50')}\n{s}", "N0",
         "-r 10 -d 10", "", "ok"),
        ("preflight's later -d wins", f"{c.replace('measured=5000', 'measured=500')}\n{s}", "N0", a + " -d 1", "",
         "ok"),
        ("N4 without the ingress drops", f"{c}\n{s}", "N4", a, "egress_dropped=300 ingress_dropped=0",
         "VOID(treatment:ingress_dropped_0)"),
        ("N4 without counters at all", f"{c}\n{s}", "N4", a, "", "VOID(treatment:egress_dropped_absent)"),
        ("N0 with drops", f"{c}\n{s}", "N0", a, "egress_dropped=1 ingress_dropped=0", "VOID(treatment:shaped_at_N0)"),
    ]
    bad = 0
    for name, text, net, args, tc, want in cases:
        got = verdict(text, net, args, tc)
        ok = got == want and " " not in got
        bad += not ok
        print(f"{'ok  ' if ok else 'BAD '} {name}: {got}" + ("" if ok else f" (want {want})"))
    print(f"SELFTEST {'OK' if not bad else 'FAILED'} ({len(cases) - bad}/{len(cases)})")
    return 1 if bad else 0


def main(argv):
    if argv[1:] == ["--selftest"]:
        return selftest()
    opt = {"--net": None, "--args": "", "--tc": ""}
    rest = argv[1:]
    while rest:
        if rest[0] not in opt or len(rest) < 2:
            print(__doc__, file=sys.stderr)
            return 2
        opt[rest[0]] = rest[1]
        rest = rest[2:]
    if opt["--net"] not in ("N0", "N4"):
        print("--net N0 or N4 (the service cells' conditions)", file=sys.stderr)
        return 2
    print(verdict(sys.stdin.read(), opt["--net"], opt["--args"], opt["--tc"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
