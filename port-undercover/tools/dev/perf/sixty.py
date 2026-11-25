import csv, collections, sys
# Seconds (85-195 s) in which most frames took 4 refreshes from submission to the screen: the panel ran at 60 Hz.
for p in sys.argv[1:]:
    rows=[r for r in csv.DictReader(open(p))]
    t0=float(rows[0]['sealed_s'])
    new=[r for r in rows if r['repeat']=='0' and float(r['presented_s'])>0]
    by=collections.defaultdict(list)
    for r in new:
        s=int(float(r['sealed_s'])-t0)
        if 85<=s<195: by[s].append(round((float(r['presented_s'])-float(r['submitted_s']))*120))
    slow=sum(1 for s,v in by.items() if sum(1 for x in v if x>=4) > len(v)/2)
    print('%s: %d of %d seconds with the panel at 60 Hz'%(p.split('/')[-1], slow, len(by)))
