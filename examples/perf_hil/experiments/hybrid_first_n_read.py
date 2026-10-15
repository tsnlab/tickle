#!/usr/bin/env python3
"""hybrid_first_n_read.py - the verdicts of hybrid_first_n_pc.sh, as its header pre-registers them (2026-10-15).

Usage: hybrid_first_n_read.py RESULTS.txt
"""

import statistics
import sys

NONE_SEEN = 0xFF00
RATE_HZ = 30
L0_MIN_SHARE = 0.9
KEPT_MAX_BELOW = 2
KEPT_SLACK = 1


def fields(text):
    out = {}
    for token in text.split():
        if "=" in token:
            key, value = token.split("=", 1)
            out.setdefault(key, value)
    return out


def parse(path):
    runs = []
    duration = None
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("=== hybrid_first_n_pc"):
                duration = float(fields(line)["duration"].rstrip("s"))
            if not line.startswith("RUN "):
                continue
            head, pub, sub = (part.strip() for part in line.split("|"))
            run = fields(head)
            run["pub"] = fields(pub) if pub != "NO_RESULT" else None
            run["sub"] = fields(sub) if sub != "NO_RESULT" else None
            run["duration"] = duration
            peers = []
            if run.get("peer_p", "none") != "none":
                for entry in run["peer_p"].strip(";").split(";"):
                    entry_fields = dict(item.split("=", 1) for item in entry.split(","))
                    peers.append((int(entry_fields["before_sample"]), int(entry_fields["code"], 16)))
            run["peers"] = peers
            runs.append(run)
    return runs


def realized(run):
    """The treatment read from the run itself: samples at n > 1, samples below n_max, the first p heard."""
    hist = [int(x) for x in run["pub"]["hybrid_n_hist"].split("/")]
    first = next(((sample, code) for sample, code in run["peers"] if code != 0), None)
    events = int(run["sub"]["hybrid_rx_loss"].split("/")[0])
    return {
        "at_runs": sum(hist[1:]),
        "below_nmax": sum(hist[1:-1]),
        "mtu": hist[0],
        "first_p": first,
        "events": events,
        "published": int(run["pub"]["published"]),
        "delivered": int(run["sub"]["delivered"]),
        "abandoned": int(run["sub"]["large_abandoned"]),
        # From the match point (third campaign): absent in a bench before first_index existed.
        "first_index": int(run["sub"].get("first_index", "0")),
        "missing_after_first": int(run["sub"].get("missing_after_first", "-1")),
        "last_index": int(run["sub"].get("last_index", "-1")),
    }


def void_reason(run):
    if run["pub"] is None or run["sub"] is None:
        return "no RESULT"
    r = realized(run)
    if run["cell"] == "l5" and r["events"] == 0:
        return "no loss event at the reader (loss not applied)"
    if run["cell"] == "l0" and r["published"] < L0_MIN_SHARE * run["duration"] * RATE_HZ:
        return f"published {r['published']}"
    return None


def main():
    runs = parse(sys.argv[1])
    valid = {}
    for run in runs:
        reason = void_reason(run)
        key = (run["cell"], run["arm"])
        if reason is not None:
            print(f"VOID cell={run['cell']} arm={run['arm']} rep={run['rep']}: {reason}")
            continue
        r = realized(run)
        r["rep"] = run["rep"]
        valid.setdefault(key, []).append(r)

    for (cell, arm), rs in sorted(valid.items()):
        with_runs = [r for r in rs if r["at_runs"] > 0]
        incomplete = [r for r in rs if r["delivered"] < r["published"] or r["abandoned"] > 0]
        first_codes = sorted({f"0x{r['first_p'][1]:04x}@{r['first_p'][0]}" for r in with_runs if r["first_p"]})
        print(
            f"CELL {cell} {arm}: runs={len(rs)} runs_with_n>1={len(with_runs)} "
            f"samples_at_n>1={[r['at_runs'] for r in with_runs]} incomplete={len(incomplete)} "
            f"(delivered/published/abandoned {[(r['delivered'], r['published'], r['abandoned']) for r in incomplete]}) "
            f"below_nmax median={statistics.median(r['below_nmax'] for r in rs)} max={max(r['below_nmax'] for r in rs)} "
            f"mtu_samples median={statistics.median(r['mtu'] for r in rs)} first_p_of_n>1_runs={first_codes}"
        )

    def get(cell, arm):
        return valid.get((cell, arm), [])

    a5 = get("l5", "A")
    a5_runs = [r for r in a5 if r["at_runs"] > 0]
    repro = len(a5_runs) > 0 and all(r["first_p"] and r["first_p"][1] == NONE_SEEN for r in a5_runs)
    print(
        f"REPRO {'YES' if repro else 'NO'}: A at l5 sent samples at n > 1 in {len(a5_runs)} of {len(a5)} runs; first p "
        f"heard in them: {[hex(r['first_p'][1]) if r['first_p'] else None for r in a5_runs]}"
    )
    ag = get("l5", "Ag")
    if ag:
        bad = [r for r in ag if r["at_runs"] > 0]
        print(f"CONTROL {'HELD' if not bad else 'BROKEN'}: Ag at l5 sent samples at n > 1 in {len(bad)} of {len(ag)} runs")
    b5 = get("l5", "B")
    if b5:
        if not repro:
            print("FIXED NOT READ: the mechanism did not reproduce in A, so B's l5 cannot fail")
        else:
            bad = [r for r in b5 if r["at_runs"] > 0]
            lost = [r for r in b5 if r["delivered"] < r["published"] or r["abandoned"] > 0]
            ok = not bad and not lost
            print(
                f"FIXED {'PASS' if ok else 'FAIL'}: B at l5 - runs with n > 1: {len(bad)} of {len(b5)}, "
                f"incomplete deliveries: {len(lost)}"
            )
    # MATCHED (third campaign, no-send-unmatched under both arms): a VOLATILE reader starts where the writer learnt it,
    # so what counts is delivery from its first sample on - nothing missing between its first and last sample, the last
    # the last published, nothing abandoned - and no sample at n > 1. Read for B, and for A as the control's rate.
    for arm in ("A", "B"):
        rs = get("l5", arm)
        if not rs or rs[0]["missing_after_first"] < 0:
            continue
        bad = [r for r in rs if r["at_runs"] > 0]
        gaps = [r for r in rs if r["missing_after_first"] != 0 or r["abandoned"] > 0
                or r["last_index"] != r["published"] - 1]
        late = [r["first_index"] for r in rs if r["first_index"] > 0]
        verdict = "PASS" if not bad and not gaps else "FAIL"
        print(
            f"MATCHED {arm} {verdict}: l5 runs with n > 1: {len(bad)} of {len(rs)} "
            f"(samples {[r['at_runs'] for r in bad]}), incomplete from the match point: {len(gaps)} "
            f"{[(r['rep'], r['first_index'], r['missing_after_first'], r['last_index'], r['published']) for r in gaps]}, "
            f"runs whose first delivered sample was not 0: {len(late)} (first_index {sorted(late)})"
        )
    a0, b0 = get("l0", "A"), get("l0", "B")
    if a0 and b0:
        a_med = statistics.median(r["below_nmax"] for r in a0)
        b_med = statistics.median(r["below_nmax"] for r in b0)
        b_max = max(r["below_nmax"] + r["mtu"] for r in b0)
        a_max = max(r["below_nmax"] + r["mtu"] for r in a0)
        ok = all(r["below_nmax"] <= KEPT_MAX_BELOW for r in b0) and b_med <= a_med + KEPT_SLACK
        print(
            f"KEPT {'PASS' if ok else 'FAIL'}: l0 samples below n_max (n 2..44) median A {a_med} B {b_med}, max B "
            f"{max(r['below_nmax'] for r in b0)}; samples not at n_max (MTU included) max A {a_max} B {b_max}"
        )
        for arm, rs in (("A", a0), ("B", b0)):
            cpu = [float(run["pub"]["cpu_ms_per_mb"]) for run in runs if run["cell"] == "l0" and run["arm"] == arm
                   and run["pub"]]
            print(f"INFO l0 {arm}: publisher cpu_ms_per_mb median {statistics.median(cpu):.3f} (PC, not a rig figure)")


if __name__ == "__main__":
    main()
