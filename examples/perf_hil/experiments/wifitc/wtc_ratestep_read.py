import glob,os,re,statistics as st,sys
B=os.path.expanduser("~/rig_results_safe/wtc_ratestep_20261122")
STEP1,STEP2,PERIOD=45.0,90.0,4.0
ARMS=["tickle-ad","tickle-ad-c30","tickle-p95","tickle-k3770","cyclonedds","fastdds"]
def un(x): return x-18446744073709551.616 if x>9223372036854775.0 else x
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
def phase(i): t=i*PERIOD; return 0 if t<STEP1 else (1 if t<STEP2 else 2)
def pct(v,p):
    v=sorted(v); 
    if not v: return None
    return v[min(len(v)-1,int(p/100*len(v)))]
def samples(b,arm):
    """-> (published, {index: latency_ms}) ; None index mapping when unknown"""
    near=kv(b+".near") or {}; far=kv(b+".far") or {}
    pub=int(float(near.get("published",0))); off=offset_ms(b); d={}
    if arm.startswith("tickle"):
        for l in open(b+".far",errors="replace"):
            if l.startswith("LAT "):
                f=dict(t.split("=") for t in l.split()[1:])
                d[int(f["index"])]=(int(f["arrived_ns"])-int(f["sent_ns"]))/1e6-off
        return pub,d,True
    lat=[]
    try:
        lat=[un(float(x))/1000-off for x in open(b+".lat.sub").read().split()]
    except Exception: pass
    full=len(lat)==pub and pub>0
    if full: d={i:lat[i] for i in range(pub)}
    return pub,d,full,lat if not full else None
rows={}
for size,sd in (("rl1m","1 MiB"),("rl128k","128 KiB")):
    print(f"\n=== {sd}, M1 stepped 30 -> 4 (45 s) -> 30 Mbit/s (90 s), 0.25 Hz, 135 s, 3 runs per arm ===")
    print("arm | delivered by phase (before / at 4 Mbit / after) | p50 / p99 ms by phase | recovery after step down / up (samples; 1 sample = 4 s) | tx / retx (median) | swing, final rate")
    for arm in ARMS:
        runs=sorted(glob.glob(f"{B}/{size}/rel_large/M1/r*_{arm}_A.done"))
        if not runs: print(arm,"no runs"); continue
        dl=[0,0,0]; tot=[0,0,0]; lats=[[],[],[]]; rec_dn=[]; rec_up=[]; txs=[]; rtx=[]; sw=[]; fin=[]; partial=False; allruns=[]
        for done in runs:
            b=done[:-5]; res=samples(b,arm); pub,d=res[0],res[1]
            if len(res)==4 and res[3] is not None: partial=True; allruns.append((pub,res[3]))
            near=kv(b+".near") or {}
            for i in range(pub): tot[phase(i)]+=1
            for i,l in d.items():
                dl[phase(i)]+=1; lats[phase(i)].append(l)
            if d and pub:
                # recovery: first sample after a step whose latency <= 1.5x phase steady median and all later delivered within the phase
                p1=[d[i] for i in sorted(d) if phase(i)==0]; p2=[d[i] for i in sorted(d) if phase(i)==1]; p3=[d[i] for i in sorted(d) if phase(i)==2]
                if p2:
                    ref2=st.median(p2[-4:]); first2=[i for i in range(pub) if phase(i)==1]
                    r=next((k for k,i in enumerate(first2) if i in d and d[i]<=1.5*ref2),len(first2)); rec_dn.append(r)
                if p1 and p3:
                    ref1=st.median(p1[-4:]); first3=[i for i in range(pub) if phase(i)==2]
                    r=next((k for k,i in enumerate(first3) if i in d and d[i]<=1.5*ref1),len(first3)); rec_up.append(r)
            if near.get("tx_datagrams"): txs.append(float(near["tx_datagrams"])); rtx.append(float(near.get("retransmitted",0)))
            if arm.startswith("tickle-ad"):
                tr=[dict(t.split("=",1) for t in l.split()[1:] if "=" in t) for l in open(b+".near",errors="replace") if l.startswith("RATETRACE")]
                if tr:
                    rate=[float(t["rate_kbit"]) for t in tr]; first=next((i for i,t in enumerate(tr) if t["st"]!="0"),0); post=rate[first:] or rate
                    sw.append(max(post)/max(min(post),1)); fin.append(float(near.get("rate_bps",0))/1e6)
        ph=lambda p:(f"{dl[p]}/{tot[p]}")
        pl=lambda p:(f"{st.median(lats[p]):.0f} / {pct(lats[p],90):.0f} / {max(lats[p]):.0f}" if lats[p] else "- / - / -")
        note=" (order-only for partial runs)" if partial else ""
        print(f"{arm} | {ph(0)} / {ph(1)} / {ph(2)} | {pl(0)} ; {pl(1)} ; {pl(2)} | down {st.median(rec_dn) if rec_dn else '-'} up {st.median(rec_up) if rec_up else '-'} | tx {('%.0f'%st.median(txs)) if txs else '-'} retx {('%.0f'%st.median(rtx)) if rtx else '-'} | "+(f"swing med {st.median(sw):.0f}x max {max(sw):.0f}x, final {st.median(fin):.1f}M" if sw else "-")+note)
