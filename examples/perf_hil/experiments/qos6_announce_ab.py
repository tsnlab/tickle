#!/usr/bin/env python3
"""qos6_announce_ab.py - the reading of QoS plan step 6's A/B (qos6_announce_ab.sh), written before any run.

Step 6 (D1, D8, D9) is wire v14: every discovery announce record grows by 12 bytes (lifespan, history depth); nothing
per sample changes, and the rmw receive path gains one byte read and a branch per delivered sample. The plan's
falsifier (QOS_PLAN 2026-10-18, D9): "discovery join time or announce bytes measurably worse" breaks the wire rule.
A is the parent (ab/qos-5-compat), B the change.

1. bytes A.txt B.txt - announce_bytes.c's output for each tree (one fixed endpoint set; announce and summary).
   CONTROL: each file holds exactly one announce and one summary datagram, and the two announces list the same entity
   count - else VOID (the instrument did not produce what is compared).
   PASS iff  B announce = A announce + 12 x entity_count + padding to 4  (the designed change, nothing else), and
             B summary = A summary in length (the per-second discovery traffic is untouched).
   Anything else FAIL: the announce changed by more, or less, than the record.
   Printed: bytes per announce for both, the delta, and the per-entity delta.

2. join A_1.txt B_1.txt B_2.txt A_2.txt - discovery_join.sh join N E JOINS output per phase (order A B B A).
   CONTROL: every "join k" row of every phase reads "ok" (expected = (N-1) x E and a finite reached_ms). A phase with a
   VOID row is VOID; the verdict needs both phases of both arms (else VOID - never read as PASS).
   Sample size: JOINS rows per phase; at least 10 per arm or VOID.
   PASS iff  median reached_ms(B) <= median reached_ms(A) + max(0.5 ms, 10% of median(A)), over each arm's pooled
             rows. Not DISCOVERY_PLAN M3's 5 ms: a PC dry run (2026-10-19, N=3 E=32, 6 joins an arm) read ~1.8 ms for
             both, so 5 ms would pass a tripling. The change adds 12 B x E to the joiner's announce and to every
             peer's (one more fragment at E=32), microseconds on a veth; 10% is "measurably worse" with room for noise.
             Also printed: each phase's median; an A/A drift between A_1 and A_2 larger than both the B-A delta and the
             bound makes the verdict DRIFT, which is not read.
   FAIL otherwise.

3. The rmw receive path is ab_rmw_regress.sh's own verdicts (its rtt reading, ab_rmw_regress.py, and its tput reading,
   ab_rmw_tput.py, both pre-registered there). This chain only collects them: anything but their pass verdict fails
   step 6 for that row. The rtt reading's pass verdict is "NOT REAL at this power"; "REAL REGRESSION" fails, VOID voids.
   (The first rig run, 2026-10-19, printed FAIL on "NOT REAL": the word was compared to PASS. Corrected after that run;
   the rule itself is unchanged.)

final OUTB - reads $OUTB.bytes_verdict.txt, $OUTB.join_verdict.txt and the ab_rmw_regress verdict files named in
   $OUTB.rmw_outb, and prints one VERDICT line: PASS only if all three passed; VOID if any is VOID or missing.

Each subcommand prints its verdict as the first line "VERDICT <PASS|FAIL|VOID|DRIFT> ..." and exits 0.
"""
import re
import statistics
import sys

RECORD_GROWTH = 12  # bytes tt_VERSION 14 added to struct tt_UpdateEntity
JOIN_FLOOR_MS = 0.5
JOIN_FRACTION = 0.10
MIN_JOINS = 10


def datagrams(path):
    out, section = {}, None
    for line in open(path):
        line = line.strip()
        if line.startswith("-- "):
            section = line[3:]
        m = re.match(r"datagram (\d+):(.*)$", line)
        if m and section:
            out.setdefault(section, []).append((int(m.group(1)), m.group(2).split()))
    return out


def entity_count(hexbytes):
    # single form: marker, version, source, type, then tt_DataHeader (16 B), then tt_AnnounceHeader.entity_count
    return int(hexbytes[4 + 16], 16)


def bytes_verdict(a_path, b_path):
    a, b = datagrams(a_path), datagrams(b_path)
    for name, d in (("A", a), ("B", b)):
        if len(d.get("announce", [])) != 1 or len(d.get("summary", [])) != 1:
            return f"VERDICT VOID {name} does not hold one announce and one summary"
    (a_len, a_hex), (b_len, b_hex) = a["announce"][0], b["announce"][0]
    entities = entity_count(a_hex)
    if entity_count(b_hex) != entities:
        return f"VERDICT VOID entity counts differ: A {entities}, B {entity_count(b_hex)}"
    grown = a_len + RECORD_GROWTH * entities
    expected = (grown + 3) // 4 * 4 if a_len % 4 == 0 else grown
    summary_same = a["summary"][0][0] == b["summary"][0][0]
    ok = b_len in (grown, expected) and summary_same
    return (f"VERDICT {'PASS' if ok else 'FAIL'} announce A {a_len} B {b_len} B (+{b_len - a_len}, "
            f"{(b_len - a_len) / entities:.1f} per entity of {entities}; designed +{RECORD_GROWTH} each); "
            f"summary A {a['summary'][0][0]} B {b['summary'][0][0]} B")


def join_rows(path):
    rows, void = [], False
    for line in open(path):
        if not line.startswith("join "):
            continue
        if "| ok |" not in line:
            void = True
            continue
        m = re.search(r"reached_ms=([-0-9.]+)", line)
        if not m or float(m.group(1)) < 0:
            void = True
            continue
        rows.append(float(m.group(1)))
    return rows, void


def join_verdict(a1, b1, b2, a2):
    phases = {}
    for name, path in (("A_1", a1), ("B_1", b1), ("B_2", b2), ("A_2", a2)):
        rows, void = join_rows(path)
        if void or not rows:
            return f"VERDICT VOID phase {name} has a VOID row or no rows"
        phases[name] = rows
    a = phases["A_1"] + phases["A_2"]
    b = phases["B_1"] + phases["B_2"]
    if min(len(a), len(b)) < MIN_JOINS:
        return f"VERDICT VOID {len(a)} A and {len(b)} B joins, fewer than {MIN_JOINS} per arm"
    ma, mb = statistics.median(a), statistics.median(b)
    drift = abs(statistics.median(phases["A_1"]) - statistics.median(phases["A_2"]))
    per_phase = " ".join(f"{k} {statistics.median(v):.2f}" for k, v in phases.items())
    bound = max(JOIN_FLOOR_MS, JOIN_FRACTION * ma)
    verdict = "PASS" if mb <= ma + bound else "FAIL"
    if drift > abs(mb - ma) and drift > bound:
        verdict = "DRIFT"
    return (f"VERDICT {verdict} median reached_ms A {ma:.2f} (n={len(a)}) B {mb:.2f} (n={len(b)}), "
            f"B-A {mb - ma:+.2f} ms, bound +{bound:.2f} ms; A/A drift {drift:.2f} ms; phases: {per_phase}")


def first_verdict(path):
    try:
        for line in open(path):
            if line.startswith("VERDICT"):
                return line.split()[1], line.strip()
    except OSError:
        pass
    return "MISSING", f"{path}: no verdict"


def rtt_verdict(path):
    """ab_rmw_regress.py's pass verdict is "NOT REAL at this power" (no regression found); "REAL REGRESSION" fails."""
    word, line = first_verdict(path)
    if line.startswith("VERDICT: NOT REAL"):
        return "PASS", line
    return word, line


def final(outb):
    parts = [("bytes", first_verdict(outb + ".bytes_verdict.txt")), ("join", first_verdict(outb + ".join_verdict.txt"))]
    try:
        rmw = open(outb + ".rmw_outb").read().strip()
    except OSError:
        rmw = ""
    parts.append(("rmw rtt", rtt_verdict(rmw + ".verdict.txt") if rmw else ("MISSING", "no rmw run")))
    parts.append(("rmw tput", first_verdict(rmw + ".tput_verdict.txt") if rmw else ("MISSING", "no rmw run")))
    words = [w for _, (w, _) in parts]
    if any(w in ("VOID", "MISSING", "DRIFT") for w in words):
        verdict = "VOID"
    elif all(w == "PASS" for w in words):
        verdict = "PASS"
    else:
        verdict = "FAIL"
    lines = [f"VERDICT {verdict} step 6: " + ", ".join(f"{n} {w}" for n, (w, _) in parts)]
    lines += [f"  {n}: {line}" for n, (_, line) in parts]
    return "\n".join(lines)


def main(argv):
    if len(argv) >= 2 and argv[1] == "--prereg":
        print(__doc__)
        return 0
    if len(argv) == 4 and argv[1] == "bytes":
        print(bytes_verdict(argv[2], argv[3]))
    elif len(argv) == 6 and argv[1] == "join":
        print(join_verdict(*argv[2:6]))
    elif len(argv) == 3 and argv[1] == "final":
        print(final(argv[2]))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
