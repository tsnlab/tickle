#!/usr/bin/env python3
"""Rate-step stage scorer, exact version: every sample is attributed to a phase by its SEND time (the sender's CLOCK_MONOTONIC) against the step times
(the sender-side tap's monotonic time at the `tc qdisc change`, WTC_CLI_STEP_MONO_NS in <run>.steps); no index or order assumptions.
Per-sample sources: TickLE `large_trace` LAT lines in <run>.far (index, sent_ns, arrived_ns; undelivered samples get their send time from the
publisher's fixed period), DDS nodes -L files <run>.trace.pub (seq, send_ns of every published sample) and <run>.trace.sub (seq, send_ns, recv_ns).
Latency = arrival - send - clock offset (server clock - client clock, mean of <run>.clockoff and .clockoff2).
Usage: qrs3.py <stage dir> [--md]   (the old qrs/wtc_ratestep_read.py attributed DDS samples by sorted order and is kept for reference only)"""
import glob,os,re,statistics as st,sys
B=sys.argv[1]
PERIOD_NS=4e9
ARMS=["tickle-ad","tickle-p95","cyclonedds","fastdds"]+[a for a in ("tickle-ad-c30","tickle-k3770") if True]
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
def steps(b):
    try: return [int(m) for m in re.findall(r"WTC_CLI_STEP_MONO_NS (\d+)",open(b+".steps").read())]
    except OSError: return []
def pct(v,p):
    v=sorted(v); return v[min(len(v)-1,int(p/100*len(v)))] if v else None
def load(b,arm):
    """-> list of (send_ns, latency_ms or None) for every published sample"""
    off=offset_ms(b)
    if arm.startswith("tickle"):
        got={}
        for l in open(b+".far",errors="replace"):
            if l.startswith("LAT "):
                f=dict(t.split("=") for t in l.split()[1:]); got[int(f["index"])]=(int(f["sent_ns"]),(int(f["arrived_ns"])-int(f["sent_ns"]))/1e6-off)
        near=kv(b+".near") or {}; pub=int(float(near.get("published",0)))
        if not got: return []
        i0=min(got); s0=got[i0][0]
        out=[]
        for i in range(pub):
            out.append((got[i][0],got[i][1]) if i in got else (s0+(i-i0)*PERIOD_NS,None))
        return out
    try:
        pubs={}
        for l in open(b+".trace.pub"):
            q,s=l.split(); pubs[int(q)]=int(s)
        subs={}
        for l in open(b+".trace.sub"):
            q,s,r=l.split(); subs[int(q)]=(int(s),int(r))
    except OSError: return []
    return [(pubs[q],((subs[q][1]-subs[q][0])/1e6-off) if q in subs else None) for q in sorted(pubs)]
def analyse(size):
    res={}
    for arm in ARMS:
        runs=sorted(glob.glob(f"{B}/{size}/rel_large/M1/r*_{arm}_*.done"))
        per=[]
        for done in runs:
            b=done[:-5]; st_=steps(b); data=load(b,arm)
            if len(st_)<2 or not data: continue
            t1,t2=st_[0],st_[1]
            ph=lambda s:0 if s<t1 else (1 if s<t2 else 2)
            P=[[],[],[]]
            for s,l in data: P[ph(s)].append((s,l))
            rec={}
            # recovery: seconds from the step to the send time of the first sample sent after it with latency <= 1.5x the steady value of the new phase
            # (down: last 4 delivered of the slow phase; up: last 4 delivered of the first phase), and all later samples of the phase delivered
            def recov(after,ref_set,tstep):
                ok=[l for s,l in ref_set if l is not None][-4:]
                if not ok: return None
                ref=st.median(ok)
                for k,(s,l) in enumerate(after):
                    if l is not None and l<=1.5*ref and all(x[1] is not None and x[1]<=1.5*ref for x in after[k:k+3]): return (s-tstep)/1e9
                return None
            rec["down"]=recov(P[1],P[1],t1)
            rec["up"]=recov(P[2],P[0],t2)
            near=kv(b+".near") or {}
            per.append(dict(P=P,rec=rec,tx=near.get("tx_datagrams"),retx=near.get("retransmitted"),run=os.path.basename(b)))
        res[arm]=per
    return res
def fmt(v): return "-" if v is None else f"{v:.0f}"
def run_vals(per,ph):
    out=[]
    for r in per:
        d=[l for s,l in r["P"][ph] if l is not None]; n=len(r["P"][ph])
        out.append((len(d),n,st.median(d) if d else None))
    return out
for size,sd in (("rl1m","1 MiB"),("rl128k","128 KiB")):
    res=analyse(size)
    print(f"\n=== {sd}: M1 stepped 30 -> 4 -> 30 Mbit/s, phases by send time vs the step times ===")
    print("arm | runs | delivered by phase (sum over runs) | p50 / p90 / max ms by phase (pooled) | recovery s after step down / up (median over runs; 'never' = not within the run) | tx / retx median")
    for arm,per in res.items():
        if not per: print(arm,"- no runs"); continue
        dl=[sum(len([1 for s,l in r['P'][p] if l is not None]) for r in per) for p in range(3)]
        tt=[sum(len(r['P'][p]) for r in per) for p in range(3)]
        pools=[[l for r in per for s,l in r['P'][p] if l is not None] for p in range(3)]
        pl=lambda p:(f"{st.median(pools[p]):.0f} / {pct(pools[p],90):.0f} / {max(pools[p]):.0f}" if pools[p] else "- / - / -")
        def rc(key):
            v=[r['rec'][key] for r in per]; nev=sum(1 for x in v if x is None); g=[x for x in v if x is not None]
            return (f"{st.median(g):.0f}" if g else "never")+(f" ({nev}/{len(v)} never)" if nev else "")
        txs=[float(r['tx']) for r in per if r['tx']]; rtx=[float(r['retx']) for r in per if r['retx']]
        print(f"{arm} | {len(per)} | {dl[0]}/{tt[0]} ; {dl[1]}/{tt[1]} ; {dl[2]}/{tt[2]} | {pl(0)} ; {pl(1)} ; {pl(2)} | down {rc('down')} up {rc('up')} | "+(f"{st.median(txs):.0f} / {st.median(rtx):.0f}" if txs else "-"))
    # WIN / TIE / LOSE of ADAPTIVE against each DDS and the fixed arms, per phase, on per-run phase medians (strict, all runs) and delivery
    print("per-phase verdicts of tickle-ad (every ADAPTIVE run's phase p50 against every other-arm run's; lower p50 wins; a run with <3 delivered samples in the phase counts as infinity):")
    A=res.get("tickle-ad") or []
    for other in ("cyclonedds","fastdds","tickle-p95","tickle-k3770","tickle-ad-c30"):
        O=res.get(other) or []
        if not A or not O: continue
        out=[]
        for ph in range(3):
            av=[(m if (n>=3 and m is not None) else float("inf")) for d,n_,m in run_vals(A,ph) for n in [d]]
            ov=[(m if (n>=3 and m is not None) else float("inf")) for d,n_,m in run_vals(O,ph) for n in [d]]
            if all(a<o for a in av for o in ov): v="WIN"
            elif all(o<a for a in av for o in ov): v="LOSE"
            else: v="TIE"
            out.append(f"{v} ({st.median([x for x in av if x<float('inf')]) if any(x<float('inf') for x in av) else float('inf'):.0f} v {st.median([x for x in ov if x<float('inf')]) if any(x<float('inf') for x in ov) else float('inf'):.0f} ms)")
        print(f"  vs {other}: before {out[0]}; at 4 Mbit/s {out[1]}; after {out[2]}")
