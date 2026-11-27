import os,sys,glob,statistics
os.chdir(os.path.dirname(os.path.abspath(__file__)))
DYN=("P5a","P5b","P7","P6","P3","P4")
STAT=("M1","M2","S1","S2")
def parse(f):
    d={}
    for l in open(f):
        t=l.replace('|',' ').split()
        if len(t)<14 or t[0] not in DYN+STAT+("STEP",): continue
        try:
            d[(t[0],t[1])]=dict(in30=float(t[3].rstrip('%')),ov13=float(t[5].rstrip('%')),und=float(t[6].rstrip('%')),cuts=int(t[7]),p50=float(t[9]),p99=float(t[10]),retx=float(t[12].rstrip('%')))
        except Exception: pass
    return d
cfg=sys.argv[1]; vars_=sys.argv[2:]
seeds=sorted({int(f.split('_seed')[1].split('.')[0]) for f in glob.glob('os_%s_*_seed*.txt'%cfg)})
print("config",cfg,"seeds",seeds)
def rowavg(var,size,keys,pats):
    vals=[]
    for sd in seeds:
        d=parse('os_%s_%s_seed%d.txt'%(cfg,var,sd))
        r=[d[(p,size)][k] for p in pats for k in keys if (p,size) in d]
        vals.append(r)
    return vals
for size in ("128KiB","1MiB"):
    print(" ",size,"  mean over 6 dynamic patterns and seeds [min..max over seeds of the pattern-mean]")
    print("   %-8s %12s %12s %12s %7s | static p50/p99 mean over seeds: %s | retx%%"%("var","in30","over1.3","under.7","cuts"," ".join(STAT)))
    for v in vars_:
        cols={}
        for k in ("in30","ov13","und","cuts"):
            per_seed=[]
            for sd in seeds:
                d=parse('os_%s_%s_seed%d.txt'%(cfg,v,sd))
                per_seed.append(sum(d[(p,size)][k] for p in DYN)/len(DYN))
            cols[k]=(statistics.mean(per_seed),min(per_seed),max(per_seed))
        st=[]
        for p in STAT:
            p50=[];p99=[]
            for sd in seeds:
                d=parse('os_%s_%s_seed%d.txt'%(cfg,v,sd)); p50.append(d[(p,size)]["p50"]); p99.append(d[(p,size)]["p99"])
            st.append("%s %4.0f/%5.0f"%(p,statistics.mean(p50),statistics.mean(p99)))
        rx=[]
        for sd in seeds:
            d=parse('os_%s_%s_seed%d.txt'%(cfg,v,sd)); rx.append(sum(d[(p,size)]["retx"] for p in DYN)/len(DYN))
        print("   %-8s %4.0f [%2.0f..%2.0f] %4.0f [%2.0f..%2.0f] %4.0f [%2.0f..%2.0f] %5.1f | %s | %.1f"%(v,*cols["in30"],*cols["ov13"],*cols["und"],cols["cuts"][0]," ".join(st),statistics.mean(rx)))
