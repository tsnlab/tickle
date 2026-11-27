#!/usr/bin/env python3
"""Scores train_probe results: <dir>/<mode>_r<kbit>_n<N>.txt -> per mode, rate and train size: user span / ideal (p5, p50, p95), kernel span / ideal (p50, the modes that have a kernel stamp),
distinct stamps per train (the probe's `datagrams per distinct stamp` line prints the number of stamp groups: N = every datagram stamped on its own, 1 = the whole train under one stamp),
user stamp lag behind the kernel's (p50, last datagram), whole trains. OK = p50 within 10 % of the ideal span and p5-p95 within 20 %. Usage: tpscore.py <dir> [N ...]"""
import glob,os,re,sys
D=sys.argv[1]; MODES=["hal_offload0","hal_offload0_drain","hal_offload1","hal_offload1_drain","each","batch","each_gro"]
RATES=[4000,30000,100000,200000]
def pcts(line):
    m=re.findall(r"p(\d\d)\s+(-?\d+)",line); return {int(a):int(b) for a,b in m}
def parse(f):
    t=open(f,errors="replace").read(); r={}
    m=re.search(r"ideal_span_us=(\d+)",t); ideal=int(m.group(1)) if m else None
    m=re.search(r"trains_seen=(\d+) whole=(\d+)",t); r["whole"]=int(m.group(2)) if m else 0
    for l in t.splitlines():
        if "user span / ideal" in l: r["u"]={k:v/1000 for k,v in pcts(l).items()}
        elif "kernel span - ideal" in l and ideal:
            p=pcts(l); r["k"]={k:(ideal*1000+v)/(ideal*1000) for k,v in p.items()}
        elif "datagrams per distinct stamp" in l: r["g"]=pcts(l)
        elif "user stamp - kernel stamp" in l: r["lag"]=pcts(l)
    return r
def ok(u): return u and 0.9<=u[50]<=1.1 and 0.8<=u[5]<=1.2 and 0.8<=u[95]<=1.2
sizes=[int(x) for x in sys.argv[2:]] or [10,20,40]
for n in sizes:
    print(f"\n### train of N = {n} datagrams: user span / ideal  p50 (p5-p95) | kernel span / ideal p50 | distinct stamps per train p50 (N = each on its own) | user-kernel lag p50 us | OK = p50 within 10 %, p5-p95 within 20 %")
    print("| mode | "+" | ".join(f"{r//1000} Mbit/s" for r in RATES)+" |"); print("|---|"+"---|"*len(RATES))
    for mode in MODES:
        cells=[]
        for rate in RATES:
            f=f"{D}/{mode}_r{rate}_n{n}.txt"
            if not os.path.exists(f): cells.append("-"); continue
            r=parse(f); u=r.get("u")
            if not u: cells.append(f"no data (whole {r['whole']})"); continue
            s=f"{u[50]:.2f} ({u[5]:.2f}-{u[95]:.2f})"
            if "k" in r: s+=f"; k {r['k'][50]:.2f}"
            if "g" in r: s+=f"; st {r['g'][50]}"
            if "lag" in r: s+=f"; lag {r['lag'][50]/1000:.0f}"
            s+=" **OK**" if ok(u) else " x"
            cells.append(s)
        print(f"| {mode} | "+" | ".join(cells)+" |")
