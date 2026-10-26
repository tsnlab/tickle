#!/usr/bin/env python3
"""eth_storm_fix_classes.py - DESCRIPTIVE, written after eth_storm_fix_ab.sh's run had started (not registered).

The first peek at the run's natural ETHERNET storms (resends >= 100,000) showed two kinds: main's released mostly by
the repair timeout (the EWS loop), and the candidates' released mostly by RACK's time rule and as first resends,
with few timeout releases. This splits every STORM rep by the writer's RSTATS `lro` counters: TIMEOUT when
released_timeout >= 10% of its resends, OTHER otherwise, and prints per tree and phase the counts, plus each tree's
total and median timeout releases. It decides nothing; the registered reading is eth_storm_fix_read.py.
Usage: eth_storm_fix_classes.py OUT TAG
"""
import glob
import os
import re
import statistics
import sys

KV = re.compile(r"([a-z_0-9]+)=([^ ]+)")


def main():
    out, tag = sys.argv[1], sys.argv[2]
    rows = []
    for d in sorted(glob.glob(os.path.join(out, f"{tag}_*", "runs", "*rel_large*"))):
        run = os.path.basename(os.path.dirname(os.path.dirname(d)))[len(tag) + 1:]
        phase, label = run.split("_", 1)
        kind = phase if phase in ("x", "p", "r4") else phase[0]
        try:
            ping = open(os.path.join(d, "ping.log"), errors="replace").read().splitlines()
            meta = dict(KV.findall(open(os.path.join(d, "meta.txt"), errors="replace").read()))
        except OSError:
            continue
        pub = [dict(KV.findall(x)) for x in ping if x.startswith("RESULT: role=pub")]
        lro = [dict(KV.findall(x)) for x in ping if x.startswith("RSTATS: role=pub lro ")]
        if not pub or not lro:
            continue
        r = int(pub[0].get("retransmitted", 0))
        rows.append({"kind": kind, "tree": label.rstrip("0123456789"), "rep": f"{run}-{d.rsplit('-', 1)[1]}",
                     "resends": r, "timeout": int(lro[0].get("released_timeout", 0)),
                     "rack": int(lro[0].get("rack", 0)), "shown": int(lro[0].get("released_shown", 0)),
                     "load": float(meta.get("load", 0) or 0)})
    for kind in ("e", "x", "p", "w"):
        sel = [r for r in rows if r["kind"] == kind]
        if not sel:
            continue
        print(f"phase {kind}:")
        for t in ("M", "A", "F", "G"):
            mine = [r for r in sel if r["tree"] == t]
            if not mine:
                continue
            st = [r for r in mine if r["resends"] >= 100_000]
            to = [r for r in st if r["timeout"] >= 0.1 * r["resends"]]
            print(f"  {t}: reps {len(mine)}, STORM {len(st)} (TIMEOUT {len(to)}, OTHER {len(st) - len(to)}); "
                  f"timeout releases total {sum(r['timeout'] for r in mine)}, median "
                  f"{statistics.median(r['timeout'] for r in mine)}")
            for r in sorted(st, key=lambda x: -x["resends"]):
                print(f"    {r['rep']}: resends {r['resends']} timeout {r['timeout']} rack {r['rack']} shown "
                      f"{r['shown']} load {r['load']:.1f}")


if __name__ == "__main__":
    sys.exit(main())
