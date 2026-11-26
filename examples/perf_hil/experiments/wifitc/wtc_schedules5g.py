#!/usr/bin/env python3
"""Synthetic 5G-like bandwidth/delay/loss schedules (NOT real 5G traces; parameters follow the ranges given in the task and typical literature figures, stated in the report).
Usage: wtc_schedules5g.py <N1|N2|N3|N4|all> [--out DIR] [--seed N]
Each schedule is a JSON file {name, seed, duration_s, base{limit_pkts, delay_ms, loss}, steps[{t_ms, rate_kbit, delay_ms, loss}]} played by wtc_netem_player.py on both Pis'
taps (same rate and delay in both directions, a 100 % loss step = an outage). 180 s each.
N1 mid-band: random walk 20-150 Mbit/s (log-normal step, sigma 0.25), a step every 200-500 ms, one-way delay 10 ms, 0.5 % loss, queue 500 packets.
N2 mmWave blockage: 200 Mbit/s; every 2-6 s an event: 75 % a blockage of 0.2-2 s at 5-10 Mbit/s, 25 % a near-outage of 50-100 ms (100 % loss); after an event the rate recovers in two steps
   (100 Mbit/s for 100 ms, then 200); one-way delay 8 ms, 0.1 % loss, queue 500.
N3 handover: 100 Mbit/s steady; every 10-15 s an outage of 50-100 ms (100 % loss) after which the one-way delay becomes a new value drawn from 10..25 ms; 0.1 % loss, queue 500.
N4 cell edge / congested: 10-40 Mbit/s slow drift (a step every 1 s, normal sigma 3 Mbit/s, clipped), bursty loss (Gilbert-Elliott 1 % / 15 % / 60 % / 0.5 %, about 4 % mean),
   one-way delay 15 ms, bufferbloat: queue 200 packets."""
import json,math,os,random,sys
DUR=180_000
def clip(x,a,b): return max(a,min(b,x))
def gen(name,seed):
    r=random.Random(seed); steps=[]
    if name=="N1":
        base=dict(limit_pkts=500,delay_ms=10,loss="0.5%"); t=0; rate=60.0
        while t<DUR:
            steps.append(dict(t_ms=t,rate_kbit=int(rate*1000),delay_ms=10,loss="0.5%"))
            t+=r.randint(200,500); rate=clip(rate*math.exp(r.gauss(0,0.25)),20,150)
    elif name=="N2":
        base=dict(limit_pkts=500,delay_ms=8,loss="0.1%"); L="0.1%"
        steps.append(dict(t_ms=0,rate_kbit=200000,delay_ms=8,loss=L)); t=int(r.uniform(2000,6000))
        while t<DUR-3000:
            if r.random()<0.75:
                d=int(r.uniform(200,2000)); steps.append(dict(t_ms=t,rate_kbit=int(r.uniform(5,10)*1000),delay_ms=8,loss=L))
            else:
                d=int(r.uniform(50,100)); steps.append(dict(t_ms=t,rate_kbit=200000,delay_ms=8,loss="100%"))
            t+=d; steps.append(dict(t_ms=t,rate_kbit=100000,delay_ms=8,loss=L)); t+=100; steps.append(dict(t_ms=t,rate_kbit=200000,delay_ms=8,loss=L))
            t+=int(r.uniform(2000,6000))
    elif name=="N3":
        base=dict(limit_pkts=500,delay_ms=10,loss="0.1%"); L="0.1%"; delay=10
        steps.append(dict(t_ms=0,rate_kbit=100000,delay_ms=delay,loss=L)); t=int(r.uniform(10000,15000))
        while t<DUR-1000:
            d=int(r.uniform(50,100)); steps.append(dict(t_ms=t,rate_kbit=100000,delay_ms=delay,loss="100%")); t+=d
            delay=r.randint(10,25); steps.append(dict(t_ms=t,rate_kbit=100000,delay_ms=delay,loss=L)); t+=int(r.uniform(10000,15000))
    elif name=="N4":
        G="gemodel 1% 15% 60% 0.5%"; base=dict(limit_pkts=200,delay_ms=15,loss=G); t=0; rate=25.0
        while t<DUR:
            steps.append(dict(t_ms=t,rate_kbit=int(rate*1000),delay_ms=15,loss=G)); t+=1000; rate=clip(rate+r.gauss(0,3),10,40)
    else: raise SystemExit("unknown "+name)
    # time-weighted nominal mean rate (an outage keeps its nominal rate: it is a loss, not a rate)
    tot=0.0; ends=[s["t_ms"] for s in steps[1:]]+[DUR]
    for s,e in zip(steps,ends): tot+=s["rate_kbit"]*(e-s["t_ms"])
    return dict(name=name,seed=seed,duration_s=DUR//1000,base=base,steps=steps,mean_rate_kbit=round(tot/DUR),min_rate_kbit=min(s["rate_kbit"] for s in steps),max_rate_kbit=max(s["rate_kbit"] for s in steps))
if __name__=="__main__":
    seed=int(sys.argv[sys.argv.index("--seed")+1]) if "--seed" in sys.argv else 20261126
    out=sys.argv[sys.argv.index("--out")+1] if "--out" in sys.argv else None
    names=["N1","N2","N3","N4"] if sys.argv[1]=="all" else [sys.argv[1]]
    for i,n in enumerate(names):
        s=gen(n,seed+1+["N1","N2","N3","N4"].index(n))
        if out:
            os.makedirs(out,exist_ok=True); json.dump(s,open(f"{out}/{n}.json","w"),indent=0)
        print(n,"seed",s["seed"],"steps",len(s["steps"]),"mean",s["mean_rate_kbit"],"kbit min",s["min_rate_kbit"],"max",s["max_rate_kbit"],"outages",sum(1 for x in s["steps"] if x["loss"]=="100%"))
