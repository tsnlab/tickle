import re,glob,sys,os
for d in sys.argv[1:]:
  for res in sorted(glob.glob(d+'/*.txb')):
    run=os.path.basename(res)[:-4]; base=res[:-4]
    sent=bits=byt=0; ok=0
    for ext in ('ping','echo'):
        t=open(f'{base}.{ext}').read()
        for m in re.finditer(r'acknack_tx .*?sent=(\d+) bits_sent=(\d+) bytes_sent=(\d+)',t):
            sent+=int(m[1]);bits+=int(m[2]);byt+=int(m[3]);ok+=1
    tx=sum(int(x) for x in re.findall(r'=(\d+)',open(res).read()))
    r=open(base+'.result').read()
    sc=re.search(r'scored=(\d+)',r); p90=re.search(r'p90_us=([\d.]+)',r); rt=re.search(r'retransmitted=(\d+)',r)
    mw=(byt-20*sent)/8/sent if sent else 0
    print(f"{run:18s} roles={ok} sent={sent:6d} bytes={byt:7d} bits/req={bits/sent if sent else 0:5.2f} mean_words={mw:4.2f} txbytes={tx:9d} share={100*byt/tx:5.2f}% scored={sc and sc[1]} p90ms={float(p90[1])/1000 if p90 else -1:.0f} retx={rt and rt[1]}")
