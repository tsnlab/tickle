#!/usr/bin/env python3
"""Time-varying bandwidth schedules for the rate-step driver (wtc_run.sh RATE_STEP="<second>:<Mbit/s> ..."). Base link M1 (5 ms one way, 1 % loss, queue 100).
Usage: wtc_schedules.py <pattern> [--seed N]  -> prints "<duration_s> <RATE_STEP string>"   patterns: P3 P4 P5a P5b P6 P7
P3 gradual fall 30 -> 4 linearly over 150 s (a step every 5 s), then hold 30 s; P4 gradual rise 4 -> 30 over 150 s, hold 30 s; P5a / P5b square wave 30 <-> 10 with
a 20 s / 60 s period, 180 s; P6 30 Mbit/s, 4 Mbit/s for 5 s at t = 60 s, 150 s; P7 random walk: an integer rate drawn uniformly from 5..30 every 5 s (seed fixed), 180 s."""
import random,sys
def sched(p,seed=20261126):
    if p=="P3": return 180,[(t,round(30-26*t/150)) for t in range(0,155,5)]
    if p=="P4": return 180,[(t,round(4+26*t/150)) for t in range(0,155,5)]
    if p in("P5a","P5b"):
        per=20 if p=="P5a" else 60; h=per//2; out=[]; t=0; hi=True
        while t<180: out.append((t,30 if hi else 10)); t+=h; hi=not hi
        return 180,out
    if p=="P6": return 150,[(0,30),(60,4),(65,30)]
    if p=="P7":
        r=random.Random(seed); return 180,[(t,r.randint(5,30)) for t in range(0,180,5)]
    raise SystemExit("unknown pattern "+p)
if __name__=="__main__":
    seed=int(sys.argv[sys.argv.index("--seed")+1]) if "--seed" in sys.argv else 20261126
    d,s=sched(sys.argv[1],seed)
    print(d," ".join(f"{t}:{r}" for t,r in s))
