#!/usr/bin/env python3
"""Runs every (workload, sample size, candidate, feedback model, seed) of DESIGN section 27 and aggregates the tables.
usage: run_all.py <algosim binary> <sched dir> <out dir> [jobs] [seeds]"""
import itertools, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

binary, sched, out = sys.argv[1:4]
jobs = int(sys.argv[4]) if len(sys.argv) > 4 else 6
nseeds = int(sys.argv[5]) if len(sys.argv) > 5 else 6
WL = ["P3", "P4", "P5a", "P5b", "P6", "P7", "RS", "RSm", "RSd", "M1", "M2", "S1", "S2", "N1", "N2", "N3", "N4", "H100", "H200"]
SIZES = [89, 723]  # 128 KiB, 1 MiB in datagram units
BASE = ["adapt", "bbr3", "nada", "gcc", "train", "train1"]
CANDS = [("fixed", "a"), ("oracle", "a")]
for c in BASE:
    for suffix in ("", "_hi"):
        for fb in "ab":
            CANDS.append((c + suffix, fb))
for c in ("train1t", "traint"):  # a train of about 10 ms of the link: needs the enhanced report (model b) to mean anything
    for suffix in ("", "_hi"):
        CANDS.append((c + suffix, "b"))
# the stress sweep (SLOT_US / JITTER_US in the environment) runs a subset: CANDS="adapt:a,bbr3:b,..." WLS="P7,N1,..."
if os.environ.get("CANDS"):
    CANDS = [tuple(x.split(":")) for x in os.environ["CANDS"].split(",")]
if os.environ.get("WLS"):
    WL = os.environ["WLS"].split(",")
SEEDS = [12345 + sd * 7919 for sd in range(1, nseeds + 1)]
os.makedirs(out, exist_ok=True)

def one(t):
    wl, units, cand, fb, seed = t
    path = f"{out}/{wl}_{units}_{cand}_{fb}_{seed}.txt"
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return
    r = subprocess.run(["nice", "-n", "10", binary, f"{sched}/{wl}.txt", str(units), cand, fb, str(seed)],
                       capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        print("FAILED", t, r.stderr[:200], file=sys.stderr)
        return
    with open(path, "w") as f:
        f.write(r.stdout)

tasks = [(wl, u, c, f, sd) for wl in WL for u in SIZES for (c, f) in CANDS for sd in SEEDS]
print(len(tasks), "runs", flush=True)
with ThreadPoolExecutor(jobs) as ex:
    list(ex.map(one, tasks))
print("done", flush=True)
