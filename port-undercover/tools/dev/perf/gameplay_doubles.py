import csv, sys
# On-screen doubles in gameplay only (frames with real GPU work), 85-195 s.
for p in sys.argv[1:]:
    rows=[r for r in csv.DictReader(open(p))]
    t0=float(rows[0]['sealed_s'])
    sel=sorted([r for r in rows if r['repeat']=='0' and 85<=float(r['sealed_s'])-t0<=195 and float(r['presented_s'])>0], key=lambda r: float(r['presented_s']))
    n=late=disp=0
    for a,b in zip(sel,sel[1:]):
        if float(a['gpu_ms'])<2 or float(b['gpu_ms'])<2: continue
        n+=1
        if (float(b['presented_s'])-float(a['presented_s']))*1000>12.5:
            if (float(b['submitted_s'])-float(a['submitted_s']))*1000>12.5: late+=1
            else: disp+=1
    dur=(float(sel[-1]['presented_s'])-float(sel[0]['presented_s']))
    print('%s: gameplay intervals %d, display-side doubles %.2f%%, late game frames %.2f%%'%(p.split('/')[-1],n,100*disp/max(n,1),100*late/max(n,1)))
