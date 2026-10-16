#!/usr/bin/env python3
"""hybrid_rx_drop_read.py - the verdicts of hybrid_rx_drop_pc.sh, as its header pre-registers them (2026-10-16).

Usage: hybrid_rx_drop_read.py RESULTS.txt
"""

import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hybrid_first_n_read import NONE_SEEN, RATE_HZ, L0_MIN_SHARE, parse, realized  # noqa: E402

N_MAX_BUCKET = -1  # the n histogram's last bucket: n = 45


def treatment(run):
    sub = run["sub"]
    return {
        "receiver_drops": int(sub.get("hybrid_receiver_drops", "0")),
        "receiver_lost": int(sub.get("hybrid_receiver_lost", "0")),
        "stale_forgets": int(sub.get("hybrid_stale_forgets", "0")),
    }


def void_reason(run):
    if run["pub"] is None or run["sub"] is None:
        return "no RESULT"
    r = realized(run)
    t = treatment(run)
    flooded = run["cell"].endswith("f")
    if flooded and t["receiver_drops"] == 0:
        return "the flood overran nothing (receiver_drops 0)"
    if run["cell"] == "l5f" and t["receiver_lost"] == 0:
        return "no gap excused (receiver_lost 0)"
    if run["cell"] == "l5" and r["events"] == 0:
        return "no loss event at the reader (loss not applied)"
    if run["cell"] == "l0f" and r["published"] < L0_MIN_SHARE * run["duration"] * RATE_HZ:
        return f"published {r['published']}"
    return None


def main():
    runs = parse(sys.argv[1])
    duration = None  # parse() reads hybrid_first_n_pc's header; this harness's has its own name
    with open(sys.argv[1], encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("=== hybrid_rx_drop_pc"):
                duration = float(line.split("duration=")[1].split("s")[0])
    for run in runs:
        run["duration"] = duration
    valid = {}
    for run in runs:
        reason = void_reason(run)
        if reason is not None:
            print(f"VOID cell={run['cell']} arm={run['arm']} rep={run['rep']}: {reason}")
            continue
        r = realized(run)
        r.update(treatment(run))
        hist = [int(x) for x in run["pub"]["hybrid_n_hist"].split("/")]
        r["at_n_max"] = hist[N_MAX_BUCKET]
        r["heard_none_seen"] = any(code == NONE_SEEN for _, code in run["peers"])
        r["rep"] = run["rep"]
        valid.setdefault((run["cell"], run["arm"]), []).append(r)

    for (cell, arm), rs in sorted(valid.items()):
        with_runs = [r for r in rs if r["at_runs"] > 0]
        bad = [r for r in rs if r["heard_none_seen"] or r["at_n_max"] > 0]
        print(
            f"CELL {cell} {arm}: runs={len(rs)} runs_with_n>1={len(with_runs)} "
            f"samples_at_n>1={[r['at_runs'] for r in with_runs]} runs_none_seen_or_n_max={len(bad)} "
            f"samples_at_n_max={[r['at_n_max'] for r in bad]} stale_forgets={sum(r['stale_forgets'] for r in rs)} "
            f"receiver_drops median={statistics.median(r['receiver_drops'] for r in rs)} "
            f"receiver_lost median={statistics.median(r['receiver_lost'] for r in rs)} "
            f"below_nmax median={statistics.median(r['below_nmax'] for r in rs)} "
            f"incomplete={sum(1 for r in rs if r['delivered'] < r['published'] or r['abandoned'] > 0)}"
        )

    def get(cell, arm):
        return valid.get((cell, arm), [])

    a5f = get("l5f", "A")
    a_bad = [r for r in a5f if r["heard_none_seen"] or r["at_n_max"] > 0]
    repro = len(a_bad) > 0
    print(f"REPRO {'YES' if repro else 'NO'}: A at l5f heard 0xff00 or sent at n = 45 in {len(a_bad)} of {len(a5f)} runs")
    b5f = get("l5f", "B")
    if not repro:
        print("FIXED NOT READ: the route was not reproduced")
    elif not b5f:
        print("FIXED NOT READ: no valid B run at l5f")
    else:
        b_bad = [r for r in b5f if r["heard_none_seen"] or r["at_n_max"] > 0 or r["stale_forgets"] > 0]
        print(f"FIXED {'YES' if not b_bad else 'NO'}: B at l5f went wrong in {len(b_bad)} of {len(b5f)} runs")
    control = [r for arm in ("A", "B") for r in get("l5", arm) if r["at_runs"] > 0 or r["heard_none_seen"]]
    n_control = len(get("l5", "A")) + len(get("l5", "B"))
    print(f"CONTROL {'HELD' if not control else 'BROKEN'}: l5 runs with n > 1 or 0xff00: {len(control)} of {n_control}")
    a0, b0 = get("l0f", "A"), get("l0f", "B")
    if a0 and b0:
        ma = statistics.median(r["below_nmax"] for r in a0)
        mb = statistics.median(r["below_nmax"] for r in b0)
        print(f"KEPT {'YES' if mb <= ma + 1 else 'NO'}: l0f below_nmax median A {ma} B {mb}")
    else:
        print("KEPT NOT READ: an l0f arm has no valid run")


if __name__ == "__main__":
    main()
