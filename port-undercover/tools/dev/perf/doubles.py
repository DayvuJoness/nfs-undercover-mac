import csv, collections, sys
for p in sys.argv[1:]:
    rows=[r for r in csv.DictReader(open(p))]
    t0=float(rows[0]['sealed_s'])
    sel=sorted([r for r in rows if r['repeat']=='0' and 130<=float(r['sealed_s'])-t0<=190 and float(r['presented_s'])>0], key=lambda r: float(r['presented_s']))
    P=1000/120; disp=game=0; n=0
    for a,b in zip(sel,sel[1:]):
        n+=1
        if (float(b['presented_s'])-float(a['presented_s']))*1000>12.5:
            if (float(b['submitted_s'])-float(a['submitted_s']))*1000>12.5: game+=1
            else: disp+=1
    print('%s: %d intervals, display-side doubles %.2f%%, late frame (game/presenter) %.2f%%'%(p.split('/')[-1],n,100*disp/n,100*game/n))
