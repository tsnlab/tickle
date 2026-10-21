#!/usr/bin/env python3
"""Summarise campaign_sweep.sh output: per cell and framework, each metric's min/mean/max over the
repetitions, the payload-boundary gate, and a per-metric verdict against both DDS vendors.

Implements OPTIMIZATION_PLAN.md section 9's reading rules, so the verdict is computed rather than
eyeballed:

  WIN   TickLE is better than BOTH vendors and its range does not overlap either of theirs
  TIE   TickLE's mean equals a vendor's - not a draw by overlap, an actual equality
  DRAW  TickLE is better than both but a range overlaps, or it is better than only one
  LOSE  a vendor is better than TickLE with no overlap
  VOID  the cell was void for that framework (instrument, leftover, no RESULT), or the
        payload-boundary gate failed for that payload

"Better" has a direction per metric: higher for throughput, lower for everything else. A draw is
reported as a draw and never rounded into a win - the plan's rule is that inside the spread of three
repetitions is not a result.

GOODPUT (the user, 2026-10-17; TESTING.md section 4). The vendor verdict scores throughput as goodput and nothing
else: server.goodput_mbps, the payload of the samples the receiving application took complete and unique, inside the
measured window. The send rate (client.send_mbps) and the server's own rate (server.recv_mbps) are printed as
"diagnostic" and never decide a verdict or count in the totals. CPU and wire bytes per sample are per DELIVERED sample.
There is no drop-free rule: every ok repetition counts at its goodput (this summary never had one; the same-host
summary did). The reading rules, implemented in score() below:
  G1 Goodput needs the receiver's count: a throughput rep whose server has no recv= (a latency rep whose client has
     no recv=) is VOID for scoring, never estimated from the send rate.
  G2 Uniqueness: delivered > sent means something was counted twice; the rep is VOID.
  G3 The window: goodput is the server's win_recv_mbps, taken by BenchWindow over the sender's send_ns with the same
     edges as the client's window. If the two window lengths differ by more than one bucket (2^22 ns) the rep is VOID.
  G4 A file from before the window (no win_send_mbps on the client) is read on the whole run: goodput = the client's
     send_mbps x recv/sent, the delivered count over the client's own elapsed time.
  G5 Per delivered sample: the client's cpu_s_per_Msample, cpu_s_per_MB and wire_bytes_per_sample are printed by the
     bench over its own sent=; the rep is VOID if they do not reproduce from utime_s + stime_s and wire_bytes_total
     over sent=. They are rescaled by sent / delivered, delivered being the server's recv= (throughput) or the
     client's own recv= (latency: round trips answered). The server's figures are over its own recv= already and are
     checked the same way, not moved. delivered = 0 makes the rep VOID. A file already rewritten by goodput_rescore.py
     (diag_* fields) is refused outright: its figures would be rescaled twice.
  G6 A VOID rep is excluded from every scored metric of its framework and listed with its reason. The scored rep
     count per framework is printed for every cell before any value.
The RELIABLE incomplete-delivery rule (delivery_verdict) is kept and now checks every rep, not only the last one.
The TickLE-vs-TickLE A/B readers (ab_compare.py, reps_needed.py) read parse()'s vals with DIRECTION, which this left
as it was: per SENT sample, send_mbps and recv_mbps both present. Only the vendor verdict moved to goodput.

Usage: campaign_summary.py /tmp/tickle_campaign_<stamp>.txt
"""
import os
import re
import statistics
import sys
from collections import OrderedDict

# Metrics are keyed "<role>.<name>" because BenchStats.h emits the same names on BOTH the client and
# the server RESULT line. Collecting them without the role prefix put two values per repetition into
# one list, so the "spread" became the client-server difference rather than the repetition spread -
# and every verdict computed from it would have been wrong in a way no real data would reveal. Found
# by a fabricated cell carrying both roles.
# metric -> True when a higher value is better. This is the A/B metric set (ab_compare.py, reps_needed.py, gate v2's
# witnesses): TickLE against itself, per SENT sample, the send rate included. Since 2026-10-17 it is not the vendor
# verdict's set (that is VENDOR_DIRECTION below), and it must not change meaning, or every earlier A/B reads otherwise.
DIRECTION = {
    "send_mbps": True,
    "recv_mbps": True,
    "rtt_avg_ms": False,
    "cpu_s_per_Msample": False,
    "cpu_s_per_MB": False,
    "peak_rss_kb": False,
    "wire_bytes_per_sample": False,
    "loss_pct": False,
    # The service cells' rate (shape S, tickle/service_call, 2026-10-21): calls answered per second of the measured
    # window. Added, not changed: no earlier cell carries it, so no earlier A/B reads differently.
    "calls_per_s": True,
}
# utime_s and stime_s are deliberately NOT verdict metrics, and this is a rule CHANGED AFTER SEEING
# DATA, which is worth stating plainly rather than burying. They were in DIRECTION when the campaign
# started; the first four cells showed why they cannot be.
#
# Every throughput cell runs for a FIXED DURATION (-d 5), not a fixed sample count. A framework that
# pushes more samples in those 5 seconds necessarily burns more CPU seconds, so absolute CPU time
# measures how much work was done at least as much as how efficiently. At c1 it inverts the ranking
# outright:
#
#     fw            samples  stime_s  us/sample
#     tickle         869468    3.943       4.53
#     cyclonedds     741843    3.529       4.76
#     fastdds        286155    3.634      12.70
#
#   by absolute stime_s (lower better): cyclonedds, fastdds, tickle
#   by stime per sample (lower better): tickle, cyclonedds, fastdds
#
# FastDDS "beats" TickLE on absolute stime while doing a third of the work in the same 5 seconds.
# The metric rewards being slow, so it answers a different question from the one section 2 asks.
#
# The reason to trust the change despite its timing: the argument does not depend on which way it
# fell. Had TickLE been the slow one, the same metric would have handed TickLE a free WIN by the
# same mechanism, and it would have been just as wrong. It happens to remove a TickLE LOSE, which is
# exactly the direction that should invite suspicion - so the numbers above are printed here, and
# both values are still reported per cell, marked not-comparable rather than dropped.
#
# cpu_s_per_Msample and cpu_s_per_MB are the normalised forms and stay verdict metrics; they are
# what section 2's "less CPU than both DDS vendors" has to mean in a fixed-duration test.
INFORMATIONAL = ("utime_s", "stime_s", "whole_send_mbps", "whole_recv_mbps")
# The vendor verdict's metrics since 2026-10-17 (goodput; rules G1-G6 in the module docstring, code in score()).
# goodput_mbps is derived per rep and keyed server.goodput_mbps; the others are read per rep, the client's per-sample
# figures rescaled to per delivered sample.
GOODPUT = "goodput_mbps"
VENDOR_DIRECTION = {GOODPUT: True, **{k: v for k, v in DIRECTION.items() if k not in ("send_mbps", "recv_mbps")}}
# Printed beside goodput and never scored: the client's send rate and the server's own receive rate.
DIAGNOSTIC = ("client.send_mbps", "server.recv_mbps")
PER_DELIVERED = ("cpu_s_per_Msample", "cpu_s_per_MB", "wire_bytes_per_sample")
BUCKET_S = (1 << 22) / 1e9   # BenchWindow's bucket: two windows this close are the same window (G3)
# Kept for A/B comparisons of TickLE against itself (ab_compare.py), never for a vendor verdict here: the latency
# tails. Lower is better. Absent until 2026-10-09, so a pre-registered rtt_p99_ms primary matched nothing and the
# large-message L2 (stage 1) ended in NO VERDICT on it.
AB_ONLY = {"rtt_p50_ms": False, "rtt_p99_ms": False}
# wire_packets_per_sample is deliberately NOT in DIRECTION. It is the boundary GATE, not a metric to
# win: at P1/P2 all three are meant to read 1.0, and calling an intended three-way equality a draw
# (or worse, a win) would be a verdict on the test design rather than on TickLE. The controlled test
# with equal values is what caught that.
# The gate reads the CLIENT's own transmitted packets per sample. wire_packets_per_sample counts the
# whole interface in both directions, so on a RELIABLE publisher it carries the returning ACKNACKs
# and can never read 1.0 (TickLE Dev's correction - I had named the wrong field for this job). A
# publisher also sends heartbeats, so a correct single datagram reads slightly above 1.0: the gate is
# a band, not an equality.
# The history policy each side actually ran, as opposed to the one the cell asked for. A cell whose
# whole premise is "all three made the same promise" has to check that from the output, because the
# alternative is comparing KEEP_LAST against KEEP_ALL and calling it like-for-like - which is the
# defect that made the first Q0 baseline unrunnable (f1311d5a), caught by reading the harnesses
# rather than by any measurement.
POLICY = ("keep_all", "keep_last_depth")
BEST_EFFORT_QOS = "Q1"
KEEP_LAST_QOS = "Q2"   # RELIABLE + KEEP_LAST 64 (campaign_sweep.sh matrix)

GATE_METRIC = "wire_role_packets_per_sample"
# Gate v2 A2 (2026-10-11): executed instructions per sample, per process, from the insn_u=/insn_k= fields
# tickle/run_scenario.sh appends under INSN_COUNT=1 (perf stat over the process's whole life). Lower is better. The
# divisor is the process's own samples - the client's sent=, the server's recv= - or, for a line that has neither (the
# latency server), the client's sent= of the same row, which on a latency cell counts every round trip including the
# warm-up and cool-down edges, exactly the round trips the counted process served. Never in DIRECTION: the cross-vendor
# verdicts must not read a metric only TickLE carries; ab_compare.py reads INSN_METRICS.
INSN_METRICS = {"insn_u_per_sample": "insn_u", "insn_k_per_sample": "insn_k"}
GATE_ROLE = "client"
ONE_DATAGRAM_MAX = 1.5   # below this is one datagram per sample
TWO_DATAGRAM_MIN = 2.0   # at or above this is two
VENDORS = ("cyclonedds", "fastdds")

# What wire_packets_per_sample must read at N0, per OPTIMIZATION_PLAN.md section 9. None = not gated.
# "one" = below ONE_DATAGRAM_MAX, "two" = at or above TWO_DATAGRAM_MIN.
BOUNDARY = {
    "p1": {"tickle": "one", "cyclonedds": "one", "fastdds": "one"},
    "p2": {"tickle": "one", "cyclonedds": "one", "fastdds": "one"},
    "p3": {"tickle": "one", "cyclonedds": "two", "fastdds": "two"},
    "p4": {"tickle": "two", "cyclonedds": "two", "fastdds": "two"},
}

LINE = re.compile(
    r"^c(?P<num>\d+)\s+(?P<shape>[TLS])\s+(?P<payload>p\d)\s+(?P<qos>Q\d)\s+"
    r"(?P<net>.+?)\s+(?P<fw>tickle|cyclonedds|fastdds)\s+rep(?P<rep>\d+)\s+\|\s+"
    r"(?P<verdict>[^|]+?)\s+\|\s*(?P<fields>.*)$"
)
# The verdict may contain spaces (VOID(no RESULT line)); before 2026-10-06 it was \S+, so such a row did not match
# and vanished instead of counting as VOID. A row of this shape that still does not match is reported, not dropped.
# S: the service cells (campaign_sweep.sh c18-c21, TickLE only), read like a latency cell - a round trip per call.
ROW_START = re.compile(r"^c\d+\s+[TLS]\s+p\d\s+Q\d\s")
DIAG_FIELD = re.compile(r"(?<![\w])diag_\w+=")


def parse(path):
    cells = OrderedDict()
    for line in open(path, encoding="utf-8"):
        m = LINE.match(line.rstrip())
        if not m:
            if ROW_START.match(line) and " rep" in line:
                print(f"UNPARSED ROW (not counted - fix the parser): {line.rstrip()[:160]}", file=sys.stderr)
            continue
        key = (int(m["num"]), m["shape"], m["payload"], m["qos"], m["net"].strip())
        fw = cells.setdefault(key, OrderedDict()).setdefault(
            m["fw"], {"void": [], "vals": {}, "policy": {}, "delivery": {}, "reps": []})
        if not m["verdict"].startswith("ok"):
            # fail:samples is a fact about the cell (nothing was delivered, so every per-sample
            # figure is 0 by construction), not about the instrument. Both void the cell, and the
            # distinction is kept so the write-up can say which (TickLE Dev).
            fw["void"].append(m["verdict"])
            continue
        if DIAG_FIELD.search(m["fields"]):
            # Rewritten by goodput_rescore.py: the client's figures are goodput / per delivered sample already, and
            # score() would rescale them a second time (G5). The original file is the one to summarise.
            sys.exit(f"{path}: rows carry diag_* fields (rewritten by goodput_rescore.py); this summary scores "
                     "goodput from the original file - summarise that one")
        rep = {}   # role -> {"raw": the fields as printed, "own": after the window renaming}, read by score()
        fw["reps"].append((int(m["rep"]), rep))
        # Split the concatenated RESULT lines back into their roles before reading any number.
        insn = []  # (role, {insn_u, insn_k}, own divisor or None), resolved once the client's sent= is known
        row_sent = None
        for chunk in re.split(r"(?=role=)", m["fields"]):
            role_m = re.match(r"role=(\w+)", chunk)
            role = role_m.group(1) if role_m else "client"
            pairs = re.findall(r"([A-Za-z_]\w*)=([-\d.]+)", chunk)
            # Warm-up and cool-down (2026-10-06, TESTING.md section 5): a rate is scored over the measured window.
            # A line that carries win_send_mbps/win_recv_mbps has its whole-run send_mbps/recv_mbps renamed to
            # whole_*, kept but not scored; a line from before the window existed is read as it always was.
            # (Latency needs nothing here: rtt_* are already taken over the measured round trips only.)
            have = {k for k, _ in pairs}
            renamed = []
            for k, v in pairs:
                if k in ("send_mbps", "recv_mbps") and f"win_{k}" in have:
                    renamed.append((f"whole_{k}", v))
                elif k in ("win_send_mbps", "win_recv_mbps"):
                    renamed.append((k[len("win_"):], v))
                else:
                    renamed.append((k, v))
            own = dict(renamed)
            if pairs:   # the text before the first role= is empty, and must not take the client's place
                rep.setdefault(role, {"raw": dict(pairs), "own": own})
            counts = {k: own[k] for k in INSN_METRICS.values() if k in own}
            if counts:
                insn.append((role, counts, own.get("sent") if role == "client" else own.get("recv")))
            if role == "client" and "sent" in own:
                row_sent = own["sent"]
            for k, v in renamed:
                if k in ("sent", "recv"):
                    fw["delivery"][k] = float(v)
                if k in POLICY:
                    fw["policy"][k] = v
                if k in DIRECTION or k in INFORMATIONAL or k in AB_ONLY or k in ("sample_bytes", GATE_METRIC):
                    try:
                        fw["vals"].setdefault(f"{role}.{k}", []).append(float(v))
                    except ValueError:
                        pass
        for role, counts, div in insn:
            try:
                d = float(div if div is not None else row_sent)
            except (TypeError, ValueError):
                continue
            if d <= 0:
                continue
            for metric, field in INSN_METRICS.items():
                if field in counts:
                    fw["vals"].setdefault(f"{role}.{metric}", []).append(float(counts[field]) / d)
    return cells


def rng(vals):
    lo, hi = min(vals), max(vals)
    return lo, hi, statistics.mean(vals)


def fmt(vals):
    lo, hi, mean = rng(vals)
    f = (lambda x: f"{x:.0f}") if abs(hi) >= 1000 else (lambda x: f"{x:.3g}")
    return f(mean) if lo == hi else f"{f(mean)}[{f(lo)}..{f(hi)}]"


def _num(d, key):
    try:
        return float(d[key])
    except (KeyError, TypeError, ValueError):
        return None


def sent_delivered(shape, roles):
    """(sent, delivered) of one rep: the client's sent= and the server's recv= (throughput) or the client's own recv=
    (latency, round trips answered). Either may be None when the line does not carry it."""
    client = roles.get("client", {}).get("raw", {})
    if shape == "T":
        return _num(client, "sent"), _num(roles.get("server", {}).get("raw", {}), "recv")
    return _num(client, "sent"), _num(client, "recv")


def divisor_mismatch(raw, divisor):
    """G5: the bench's per-sample CPU and wire figures reproduce from its raw totals over divisor (utime_s and stime_s
    are printed to the millisecond, so that rounding is allowed beside 1%). A figure the line does not carry is not
    checked. Returns what failed."""
    bad = []
    cpu = (_num(raw, "utime_s") or 0.0) + (_num(raw, "stime_s") or 0.0)
    pub = _num(raw, "cpu_s_per_Msample")
    if pub is not None and "utime_s" in raw and "stime_s" in raw:
        if abs(cpu / divisor * 1e6 - pub) > pub * 0.01 + 0.002 / divisor * 1e6:
            bad.append(f"cpu {cpu / divisor * 1e6:.4f} != {pub}")
    wire, pub = _num(raw, "wire_bytes_total"), _num(raw, "wire_bytes_per_sample")
    if wire is not None and pub is not None and abs(wire / divisor - pub) > pub * 0.01 + 0.01:
        bad.append(f"wire {wire / divisor:.4f} != {pub}")
    return bad


def score_rep(shape, roles):
    """One rep's scored values {role.metric: value} under G1-G5, or (None, why) when the rep is VOID for scoring."""
    client = roles.get("client", {}).get("raw", {})
    server = roles.get("server", {}).get("raw", {})
    sent, delivered = sent_delivered(shape, roles)
    if sent is None:
        return None, "G1: no sent= on the client"
    if delivered is None:
        return None, "G1: no receive count" if shape == "T" else "G1: no round-trip count"
    if delivered > sent:
        return None, f"G2: delivered {delivered:.0f} > sent {sent:.0f}"
    if delivered <= 0:
        return None, "G5: nothing delivered"
    bad = divisor_mismatch(client, sent)
    if shape == "T" and server:
        bad += [f"server {b}" for b in divisor_mismatch(server, delivered)]
    if bad:
        return None, "G5: divisor does not reproduce: " + "; ".join(bad)
    out = {}
    if shape == "T":
        if "win_send_mbps" in client:
            if "win_recv_mbps" not in server or "win_s" not in server or "win_s" not in client:
                return None, "G3: the server has no window"
            if abs(float(server["win_s"]) - float(client["win_s"])) > BUCKET_S:
                return None, f"G3: windows {client['win_s']} s vs {server['win_s']} s"
            out[f"server.{GOODPUT}"] = float(server["win_recv_mbps"])
        elif "send_mbps" in client:
            out[f"server.{GOODPUT}"] = float(client["send_mbps"]) * delivered / sent   # G4
    for role, d in roles.items():
        for k, v in d["own"].items():
            if k not in VENDOR_DIRECTION:
                continue
            try:
                x = float(v)
            except ValueError:
                continue
            if role == "client" and k in PER_DELIVERED:
                x *= sent / delivered
            out[f"{role}.{k}"] = x
    return out, None


def score(shape, per_fw):
    """The vendor verdict's values: per framework {"vals": {role.metric: [per rep]}, "void": [(rep, why)], "n": reps
    scored}. A rep VOID under G1-G5 contributes to no metric of its framework (G6)."""
    got = {}
    for fw, d in per_fw.items():
        s = got[fw] = {"vals": {}, "void": [], "n": 0}
        for rep, roles in d["reps"]:
            vals, why = score_rep(shape, roles)
            if vals is None:
                s["void"].append((rep, why))
                continue
            s["n"] += 1
            for k, x in vals.items():
                s["vals"].setdefault(k, []).append(x)
    return got


def delivery_verdict(shape, qos, per_fw):
    """VOID a cell where any framework's server did not receive everything its client sent.

    loss_pct does not catch this. At c6 all three frameworks reported loss_pct=0.0 while FastDDS had
    delivered 1641 of 4987 samples - 67% missing - and TickLE 13802 of 14002. loss_pct measures gaps
    between the sequence numbers that *arrived*; samples that never arrived at all, because the
    server stopped before them, leave no gap to count. A metric reading a clean zero on both sides
    of a 67%-vs-1.4% difference is worse than no metric, because it reads as a TIE.

    All three servers capped themselves at -d + 15 s absolute (TickLE Dev, fixed in 0da3cad5 for all
    three at once), so this is a harness truncation rather than a middleware property, and the cell
    cannot be compared until it is re-run.

    It does NOT apply to the BEST_EFFORT cell, where sent != recv is the whole point of the QoS
    rather than a fault - the first version of this check voided c8 for delivering exactly what
    BEST_EFFORT promises. Same over-reach as the policy rule's first version, caught the same way,
    by running it over cells whose answers were already known.

    SCORING RULE CHANGED 2026-09-26 by the user's decision, made before the first run it applies to
    was read ("완전한 벤더끼리 비교"): an incomplete framework no longer voids the whole cell.
    - An incomplete VENDOR is excluded from every per-sample verdict in that cell - its figures are
      over a truncated run and cannot be trusted in either direction - and is reported on its own
      DELIVERY FAILED line. TickLE is scored against the vendors that delivered everything.
    - An incomplete TickLE makes every metric of the cell a LOSE. Applying the vendor rule to
      TickLE would let TickLE's own delivery failure pass unscored, so the rule is deliberately
      asymmetric, in the direction that costs TickLE.
    Before this, c6 was VOID in the campaign, the baseline and FRAG step 1 alike, the last two
    because FastDDS alone delivered 28-29% while TickLE and CycloneDDS delivered 100%.
    """
    if qos == BEST_EFFORT_QOS:
        return None                       # samples are meant to be droppable here
    if shape == "S":
        # A service call that ended unanswered is reported by the bench itself (timeouts=, loss_pct=) and read by
        # service_cell_verdict.py; there is no vendor to exclude and no sample stream to truncate.
        return None
    if qos == KEEP_LAST_QOS:
        # KEEP_LAST lets a writer replace unacknowledged samples once its history is full, so
        # sent != recv is what the QoS promises, not a fault - the same reasoning as BEST_EFFORT.
        # Added 2026-09-26 AFTER seeing data, and said so: the aligned campaign excluded CycloneDDS
        # from c9 for 216 of 715,733 samples (0.03%) that KEEP_LAST 64 was allowed to drop. Including
        # it changed no verdict there (TickLE led every metric either way), and the change includes a
        # vendor rather than excluding one, so it cannot have been chosen to favour TickLE.
        return None
    # Every rep is checked (since 2026-10-17). Until then only the last rep's counts were kept, so a short rep 1 or 2
    # passed unseen; sent_delivered() is the same count score() divides by.
    short = []   # (framework, description)
    for fw, d in per_fw.items():
        for rep, roles in d["reps"]:
            sent, recv = sent_delivered(shape, roles)
            if sent is None or recv is None or sent == recv:
                continue
            short.append((fw, f"{fw} rep{rep} delivered {recv:.0f} of {sent:.0f} "
                              f"({(sent - recv) / sent * 100:.1f}% missing)"))
    return short or None


def policy_verdict(per_fw):
    """VOID a cell whose frameworks did not run the same history policy.

    Not the same check as the boundary gate, and the first version of it was wrong in a way its own
    control caught: "the vendor did not report keep_all" is not grounds to void, because at this
    campaign's SHA both DDS reliable_throughput harnesses are hard-coded KEEP_ALL with no option -
    verified by reading them, which is how the unrunnable first Q0 baseline was found (f1311d5a).
    A silent vendor therefore *means* KEEP_ALL, and that is exactly what a Q0 cell wants.

    So the mismatch is specifically: TickLE ran KEEP_LAST (keep_all=0) while a vendor reported
    nothing and is therefore KEEP_ALL. That is cell 9, whose premise was "the default configuration
    users get" and which compares two different promises. Once both harnesses report keep_all and
    keep_last_depth (TickLE Dev, 5fddc985), the reported values are compared directly instead and
    this source-derived assumption stops being load-bearing.
    """
    seen = {fw: d["policy"].get("keep_all") for fw, d in per_fw.items() if d["vals"]}
    if "tickle" not in seen or seen.get("tickle") is None:
        return None
    reported = {fw: v for fw, v in seen.items() if v is not None}
    if len(reported) > 1 and len(set(reported.values())) > 1:
        return [", ".join(f"{fw} keep_all={v}" for fw, v in reported.items()) + " - not the same policy"]
    if seen["tickle"] == "0":
        silent = [fw for fw, v in seen.items() if fw != "tickle" and v is None]
        if silent:
            return [f"tickle ran KEEP_LAST (keep_all=0) while {', '.join(silent)} report no policy "
                    f"and are hard-coded KEEP_ALL at this SHA - two different promises"]
    return None


def boundary_verdict(shape, payload, net, per_fw):
    """The section-9 gate. Only meaningful at N0, where no tc is shaping anything, and only on the
    throughput cells.

    It does NOT apply to the latency cells, and applying it there VOIDed all three of them - the
    whole latency metric, one of the five the campaign exists to answer - for a reason that is not
    real. A latency cell sends 100 samples, so a per-sample packet count is dominated by fixed
    discovery traffic instead of by datagram splitting, which is the only thing this gate is about:

        c10, a 76-byte sample that nothing can possibly fragment
          tickle      sent=100  wire_tx_packets=112  ->  1.12 per sample
          cyclonedds  sent=100  wire_tx_packets=225  ->  2.25
          fastdds     sent=100  wire_tx_packets=228  ->  2.28

        c1, the same 76-byte sample in a throughput cell
          cyclonedds  sent=741843  wire_tx_packets=741888  ->  1.000

    The same fixed overhead amortises to nothing over 741k samples and to 125 extra packets over
    100. There is no n=100 measurement that would substitute, so the latency cells are simply not
    gated; their payload boundary is checked by the throughput cell at the same payload.
    """
    if shape != "T":
        return None
    if net != "none":
        return None
    bad = []
    for fw, data in per_fw.items():
        got = data["vals"].get(f"{GATE_ROLE}.{GATE_METRIC}")
        if not got:
            bad.append(f"{fw} has no {GATE_ROLE}.{GATE_METRIC}")
            continue
        observed = statistics.mean(got)
        want = BOUNDARY.get(payload, {}).get(fw)
        if want == "one" and observed >= ONE_DATAGRAM_MAX:
            bad.append(f"{fw} {observed:.2f} >= {ONE_DATAGRAM_MAX}, wanted one datagram")
        elif want == "two" and observed < TWO_DATAGRAM_MIN:
            bad.append(f"{fw} {observed:.2f} < {TWO_DATAGRAM_MIN}, wanted two")
    return bad or None


def verdict(metric, tickle, vendors):
    """Section 9's rule, with two cases the controlled test showed it needed: an actual equality is
    a TIE rather than a draw-by-overlap, and a verdict from a single repetition per side is
    provisional, because n=1 gives a zero spread that no overlap can ever be detected against."""
    hi_better = VENDOR_DIRECTION[metric]
    t_lo, t_hi, t_mean = rng(tickle)
    provisional = len(tickle) < 2 or any(len(v) < 2 for v in vendors.values())
    suffix = " [provisional: n<2 on some side, so a zero spread is an artefact]" if provisional else ""
    beats, overlaps, ties = [], [], []
    for name, vals in vendors.items():
        v_lo, v_hi, v_mean = rng(vals)
        ties.append(t_mean == v_mean)
        better = t_mean > v_mean if hi_better else t_mean < v_mean
        overlap = not (t_hi < v_lo or v_hi < t_lo)
        beats.append(better)
        overlaps.append(overlap)
        if not better and not overlap and t_mean != v_mean:
            return "LOSE", f"{name} is better with no overlap" + suffix
    if any(ties):
        return "TIE", "equal to " + ", ".join(n for n, t in zip(vendors, ties) if t) + suffix
    if all(beats) and not any(overlaps):
        return "WIN", suffix.strip()
    if all(beats):
        return "DRAW", "better than both, but a range overlaps" + suffix
    return "DRAW", "better than only one" + suffix


def main(path):
    cells = parse(path)
    if not cells:
        sys.exit(f"{path}: no campaign result lines matched - wrong file, or the sweep printed nothing")
    wins = draws = losses = voids = 0
    vendor_delivery_failures = 0
    for key, per_fw in cells.items():
        num, shape, payload, qos, net = key
        print(f"\n== c{num} {shape} {payload} {qos} [{net}]")
        gate = boundary_verdict(shape, payload, net, per_fw)
        pol = policy_verdict(per_fw)
        deliv = delivery_verdict(shape, qos, per_fw)
        tickle_failed = any(fw == "tickle" for fw, _ in (deliv or []))
        failed_vendors = {fw for fw, _ in (deliv or []) if fw != "tickle"}
        for fw, why in (deliv or []):
            print(f"   DELIVERY FAILED: {why}")
        if tickle_failed:
            print("   -> TickLE did not deliver everything its client sent: every metric in this cell")
            print("      is a LOSE (the 2026-09-26 rule is asymmetric against TickLE on purpose).")
        elif failed_vendors:
            print(f"   -> {', '.join(sorted(failed_vendors))} excluded from this cell's verdicts: its per-sample")
            print("      figures are over a truncated run. TickLE is scored against the vendors that")
            print("      delivered everything (user decision 2026-09-26).")
            vendor_delivery_failures += len(failed_vendors)
        if pol:
            print(f"   POLICY MISMATCH: {'; '.join(pol)}")
            print("   -> cross-vendor comparison is VOID: the cell's premise is that all three made")
            print("      the same promise, and the output does not show that they did.")
        void_reason = ("boundary gate" if gate else
                       "policy mismatch" if pol else None)
        gate = gate or pol
        if gate and void_reason == "boundary gate":
            print(f"   BOUNDARY GATE FAILED: {'; '.join(gate)}")
            print("   -> this payload's cross-vendor comparison is VOID (section 9). Numbers below are")
            print("      printed anyway: the observed packet count is how the right size is computed.")
        for fw, data in per_fw.items():
            if data["void"]:
                print(f"   {fw:<11} VOID x{len(data['void'])}: {data['void'][0]}")
        scored = score(shape, per_fw)
        for fw, s in scored.items():
            for rep, why in s["void"]:
                print(f"   {fw:<11} rep{rep} VOID for scoring (goodput {why})")
        # Sample counts before values (G6): the reps each framework is scored on.
        print("   scored reps: " + "  ".join(f"{fw} {s['n']}" for fw, s in scored.items()))
        tvals = per_fw.get("tickle", {}).get("vals", {})
        tscored = scored.get("tickle", {}).get("vals", {})
        metrics = [m for m in sorted(tscored) if m.split(".", 1)[1] in VENDOR_DIRECTION]
        for m in [m for m in DIAGNOSTIC if m in tvals]:
            vend = {v: per_fw[v]["vals"][m] for v in VENDORS
                    if v in per_fw and m in per_fw[v]["vals"]}
            cols = "  ".join(f"{name[:6]} {fmt(vals)}" for name, vals in vend.items())
            print(f"   {m:<24} tickle {fmt(tvals[m]):<22} {cols:<44} diagnostic  (never scored; goodput is "
                  f"server.{GOODPUT})")
        for m in [m for m in sorted(tvals) if m.split(".", 1)[1] in INFORMATIONAL]:
            vend = {v: per_fw[v]["vals"][m] for v in VENDORS
                    if v in per_fw and m in per_fw[v]["vals"]}
            cols = "  ".join(f"{name[:6]} {fmt(vals)}" for name, vals in vend.items())
            why = ("whole run incl. warm-up and cool-down; scored over the window" if "whole_" in m
                   else "absolute CPU over a fixed duration; see cpu_s_per_Msample")
            print(f"   {m:<24} tickle {fmt(tvals[m]):<22} {cols:<44} not-comparable  ({why})")
        for m in metrics:
            t = tscored[m]
            vend = {v: scored[v]["vals"][m] for v in VENDORS
                    if v in scored and m in scored[v]["vals"] and v not in failed_vendors}
            need = 2 - len(failed_vendors)
            if len(vend) < max(need, 1):
                print(f"   {m:<24} tickle {fmt(t):<22} (no cross-vendor comparison: "
                      f"{max(need, 1) - len(vend)} vendor(s) missing)")
                continue
            v, why = verdict(m.split(".", 1)[1], t, vend)
            if failed_vendors and v in ("WIN", "DRAW", "TIE", "LOSE"):
                why = (why + "; " if why else "") + "vs complete vendors only"
            if tickle_failed:
                v, why = "LOSE", "TickLE delivery failed"
            if void_reason:
                v, why = "VOID", void_reason
            cols = "  ".join(f"{name[:6]} {fmt(vals)}" for name, vals in vend.items())
            print(f"   {m:<24} tickle {fmt(t):<22} {cols:<44} {v}{'  (' + why + ')' if why else ''}")
            wins += v == "WIN"; draws += v in ("DRAW", "TIE"); losses += v == "LOSE"; voids += v == "VOID"
    print(f"\n== totals over comparable metric-cells: WIN {wins}  DRAW/TIE {draws}  LOSE {losses}  VOID {voids}")
    if vendor_delivery_failures:
        print(f"   plus {vendor_delivery_failures} vendor delivery failure(s), each excluded from its cell's verdicts")
    print("Every LOSE and every DRAW is an optimisation target and needs a named hypothesis before")
    print("any code change (OPTIMIZATION_PLAN.md section 9).")


def selftest():
    """G1-G6 and the per-rep delivery check on fabricated rows, each with the answer it must give. Exit 0 when every
    case gives it. Each rule has a case where it must fire and the base case where it must not."""
    import tempfile
    client = ("role=client sent=1000 send_mbps=10 win_s=4.995 win_send_mbps=10 utime_s=0.5 stime_s=0.5 "
              "cpu_s_per_Msample=1000 wire_bytes_total=100000 wire_bytes_per_sample=100 "
              "wire_role_packets_per_sample=1.0")
    server = ("role=server recv=500 win_s=4.995 win_recv_mbps=5 utime_s=0.25 stime_s=0.25 cpu_s_per_Msample=1000 "
              "wire_bytes_total=50000 wire_bytes_per_sample=100")

    def row(fields, qos="Q1", fw="tickle", rep=1, shape="T"):
        return f"c8  {shape} p1 {qos} none   {fw} rep{rep} | ok | {fields}\n"

    def run(text, qos="Q1", shape="T"):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write(text)
        per_fw = next(iter(parse(f.name).values()))
        os.unlink(f.name)
        return score(shape, per_fw)["tickle"], delivery_verdict(shape, qos, per_fw)

    def void_of(text, shape="T"):
        s, _ = run(text, shape=shape)
        return s["void"][0][1].split(":")[0] if s["void"] else None

    no_win = client.replace(" win_s=4.995 win_send_mbps=10", "")
    cases = [
        ("base: goodput is the server's windowed rate", lambda: run(row(f"{client} {server}"))[0]["vals"][
            f"server.{GOODPUT}"] == [5.0]),
        ("base: client CPU per delivered sample (x sent/delivered)",
         lambda: run(row(f"{client} {server}"))[0]["vals"]["client.cpu_s_per_Msample"] == [2000.0]),
        ("base: client wire bytes per delivered sample",
         lambda: run(row(f"{client} {server}"))[0]["vals"]["client.wire_bytes_per_sample"] == [200.0]),
        ("base: the server's figures are not moved",
         lambda: run(row(f"{client} {server}"))[0]["vals"]["server.cpu_s_per_Msample"] == [1000.0]),
        ("base: the send rate is not a scored metric",
         lambda: "client.send_mbps" not in run(row(f"{client} {server}"))[0]["vals"]),
        ("G1: no receive count -> VOID", lambda: void_of(row(f"{client} {server.replace('recv=500 ', '')}")) == "G1"),
        ("G2: recv > sent -> VOID", lambda: void_of(row(f"{client} {server.replace('recv=500', 'recv=1500')}"))
         == "G2"),
        ("G3: windows differ by more than a bucket -> VOID",
         lambda: void_of(row(f"{client} {server.replace('win_s=4.995', 'win_s=5.100')}")) == "G3"),
        ("G3: windows within a bucket -> scored",
         lambda: void_of(row(f"{client} {server.replace('win_s=4.995', 'win_s=4.997')}")) is None),
        ("G4: no window -> send_mbps x recv/sent",
         lambda: run(row(f"{no_win} {server}"))[0]["vals"][f"server.{GOODPUT}"] == [5.0]),
        ("G5: a client figure that does not reproduce over sent= -> VOID",
         lambda: void_of(row(f"{client.replace('cpu_s_per_Msample=1000', 'cpu_s_per_Msample=1500')} {server}"))
         == "G5"),
        ("G5: nothing delivered -> VOID", lambda: void_of(row(f"{client} {server.replace('recv=500', 'recv=0')}"))
         == "G5"),
        ("latency: per round trip answered, no goodput",
         lambda: run(row("sent=100 recv=50 rtt_avg_ms=0.2 utime_s=0.001 stime_s=0.001 cpu_s_per_Msample=20 "
                         "wire_bytes_total=10000 wire_bytes_per_sample=100", shape="L"), shape="L")[0]["vals"]
         == {"client.rtt_avg_ms": [0.2], "client.cpu_s_per_Msample": [40.0], "client.wire_bytes_per_sample": [200.0]}),
        ("G6: a VOID rep counts in no metric, the good rep does",
         lambda: run(row(f"{client} {server}", rep=1) + row(f"{client} {server.replace('recv=500 ', '')}", rep=2))[0]
         ["n"] == 1),
        ("delivery: a short RELIABLE rep 1 is found even when rep 2 is complete",
         lambda: run(row(f"{client} {server}", qos="Q0", rep=1)
                     + row(f"{client} {server.replace('recv=500', 'recv=1000')}", qos="Q0", rep=2), qos="Q0")[1]
         == [("tickle", "tickle rep1 delivered 500 of 1000 (50.0% missing)")]),
        ("delivery: a complete RELIABLE cell is not flagged",
         lambda: run(row(f"{client} {server.replace('recv=500', 'recv=1000')}", qos="Q0"), qos="Q0")[1] is None),
        ("delivery: BEST_EFFORT is scored on goodput, never flagged",
         lambda: run(row(f"{client} {server}"))[1] is None),
    ]
    fails = 0
    for name, check in cases:
        try:
            ok = check()
        except Exception as err:   # noqa: BLE001 - a case that raises has not given its answer
            ok, name = False, f"{name} (raised {err!r})"
        fails += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {name}")
    try:
        run(row(f"{client} diag_send_mbps=10 {server}"))
        print("FAIL a goodput_rescore.py rewrite is refused (it was scored)")
        fails += 1
    except SystemExit:
        print("ok   a goodput_rescore.py rewrite is refused")
    print(f"SELFTEST {'OK' if not fails else 'FAILED'} ({len(cases) + 1 - fails}/{len(cases) + 1})")
    return 1 if fails else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(selftest())
    main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/tickle_campaign_latest.txt")
