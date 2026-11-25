#!/usr/bin/env python3
"""Scorer for the time-varying bandwidth runs (wtc_chainVB.sh). Layout <dir>/<pattern>/<rl1m|rl128k>/rel_large/M1/r<n>_<arm>_A.*
Per pattern, size and arm: delivered, p50/p90/max (pooled over the runs; every sample placed by its send time on the sender Pi's clock against the schedule markers
WTC_CLI_STEP_MONO_NS / `step at=` in <run>.steps); WIN/TIE/LOSE of tickle-ad against each rival on per-run p50 (strict: every ad run below every rival run; a run with fewer
than 3 delivered samples counts as infinity; the A/A VOID rule does not apply, one copy per arm, 3 runs); how well tickle-ad's pacing rate (RATETRACE rate_kbit, time-weighted
by el_ms) follows the capacity (nominal netem rate x 0.991, the carried fraction of the calibration). Usage: wtc_varying_read.py <dir>"""
import glob,os,re,statistics as st,sys
D=sys.argv[1]; PERIOD=4e9; CARRIED=0.991
ARMS=["tickle-ad","tickle-p95","cyclonedds","fastdds"]
def kv(p):
    try:
        for l in open(p,errors="replace"):
            if l.startswith("RESULT"): return dict(t.split("=",1) for t in l.replace("RESULT:","RESULT").split() if "=" in t)
    except OSError: pass
def offset_ms(b):
    v=[]
    for s in (".clockoff",".clockoff2"):
        try: v.append(float(re.search(r"offset_us=(-?[0-9.]+)",open(b+s).read()).group(1)))
        except Exception: pass
    return sum(v)/len(v)/1000 if v else 0.0
def markers(b):
    try: txt=open(b+".steps").read()
    except OSError: return []
    out=[]; mono=None
    for l in txt.splitlines():
        m=re.match(r"WTC_CLI_STEP_MONO_NS (\d+)",l)
        if m: mono=int(m.group(1)); continue
        m=re.search(r"step at=(\d+)s rate=(\d+)mbit",l)
        if m and mono is not None: out.append((mono,int(m.group(2)),int(m.group(1)))); mono=None
    return out
def cap_at(mk,t_ns,init=30):
    c=init
    for mono,r,_ in mk:
        if mono<=t_ns: c=r
        else: break
    return c
def load(b,arm):
    off=offset_ms(b)
    if arm.startswith("tickle"):
        got={}
        try:
            for l in open(b+".far",errors="replace"):
                if l.startswith("LAT "):
                    f=dict(t.split("=") for t in l.split()[1:]); got[int(f["index"])]=(int(f["sent_ns"]),(int(f["arrived_ns"])-int(f["sent_ns"]))/1e6-off)
        except OSError: return []
        near=kv(b+".near") or {}; pub=int(float(near.get("published",0)))
        if not got: return []
        i0=min(got); s0=got[i0][0]
        return [(got[i][0],got[i][1]) if i in got else (s0+(i-i0)*PERIOD,None) for i in range(pub)]
    try:
        pubs={}
        for l in open(b+".trace.pub"):
            q,s=l.split(); pubs[int(q)]=int(s)
        subs={}
        for l in open(b+".trace.sub"):
            q,s,r=l.split(); subs[int(q)]=(int(s),int(r))
    except OSError: return []
    return [(pubs[q],((subs[q][1]-subs[q][0])/1e6-off) if q in subs else None) for q in sorted(pubs)]
def pct(v,p):
    v=sorted(v); return v[min(len(v)-1,int(p/100*len(v)))] if v else None
pats=[p for p in ("P7","P5a","P5b","P6","P3","P4") if os.path.isdir(f"{D}/{p}")] or sorted(d for d in os.listdir(D) if os.path.isdir(f"{D}/{d}/rl1m"))
for pat in pats:
  for size,sd in (("rl1m","1 MiB"),("rl128k","128 KiB")):
    root=f"{D}/{pat}/{size}/rel_large/M1"
    if not os.path.isdir(root): continue
    print(f"\n=== {pat}, {sd} ===")
    print("arm | runs | delivered | p50 / p90 / max ms (pooled) | per-run p50 medians | tx / retx median")
    perrun={}
    for arm in ARMS:
        runs=sorted(glob.glob(f"{root}/r*_{arm}_A.done"))
        L=[];pub=0;dl=0;meds=[];tx=[];rt=[]
        for done in runs:
            b=done[:-5]; data=load(b,arm)
            if not data: continue
            d=[l for s,l in data if l is not None]; L+=d; pub+=len(data); dl+=len(d)
            meds.append(st.median(d) if len(d)>=3 else float("inf"))
            n=kv(b+".near") or {}
            if n.get("tx_datagrams"): tx.append(float(n["tx_datagrams"])); rt.append(float(n.get("retransmitted",0)))
        perrun[arm]=meds
        if L: print(f"{arm} | {len(runs)} | {dl}/{pub} | {st.median(L):.0f} / {pct(L,90):.0f} / {max(L):.0f} | {', '.join('inf' if m==float('inf') else '%.0f'%m for m in meds)} | "+(f"{st.median(tx):.0f} / {st.median(rt):.0f}" if tx else "-"))
        else: print(arm,"| no data")
    A=perrun.get("tickle-ad") or []
    out=[]
    for other in ("cyclonedds","fastdds","tickle-p95"):
        O=perrun.get(other) or []
        if not A or not O: continue
        if all(a<o for a in A for o in O): v="WIN"
        elif all(o<a for a in A for o in O): v="LOSE"
        else: v="TIE"
        out.append(f"{other}: {v} ({st.median(A):.0f} v {st.median(O):.0f} ms)")
    print("tickle-ad vs",";  ".join(out))
    # capacity following
    ratios=[];wts=[]
    for done in sorted(glob.glob(f"{root}/r*_tickle-ad_A.done")):
        b=done[:-5]; mk=markers(b)
        if not mk: continue
        for l in open(b+".near",errors="replace"):
            if not l.startswith("RATETRACE"): continue
            f=dict(t.split("=",1) for t in l.split()[1:] if "=" in t)
            try: t_ns=(float(f["t_ms"])-float(f["el_ms"])/2)*1e6; w=float(f["el_ms"]); r=float(f["rate_kbit"])/1000.0
            except Exception: continue
            first=mk[0][0]
            if t_ns<first-5e9 or t_ns>mk[-1][0]+60e9: continue
            cap=cap_at(mk,t_ns)*CARRIED
            ratios.append(r/cap); wts.append(w)
    if ratios:
        tot=sum(wts)
        srt=sorted(zip(ratios,wts)); acc=0; med=None
        for r,w in srt:
            acc+=w
            if acc>=tot/2: med=r; break
        within=sum(w for r,w in zip(ratios,wts) if 0.7<=r<=1.3)/tot; over=sum(w for r,w in zip(ratios,wts) if r>1.0)/tot; over13=sum(w for r,w in zip(ratios,wts) if r>1.3)/tot; under=sum(w for r,w in zip(ratios,wts) if r<0.7)/tot
        print(f"tickle-ad rate / capacity (time-weighted over {len(ratios)} rounds): median {med:.2f}; within +-30 %: {100*within:.0f} %; over capacity {100*over:.0f} %, over 1.3x {100*over13:.0f} %, under 0.7x {100*under:.0f} %")
