import csv,sys
# game frames per 5 s window from frame_id progression (counts frames the presenter dropped)
for p in sys.argv[1:]:
    rows=[r for r in csv.DictReader(open(p))]
    t0=float(rows[0]['sealed_s']); out=[]; w=5.0
    ids=[(float(r['sealed_s'])-t0,int(r['frame_id'])) for r in rows]
    import bisect
    ts=[x[0] for x in ids]
    t=0
    while t<ts[-1]:
        a=bisect.bisect_left(ts,t); b=bisect.bisect_left(ts,t+w)-1
        if b>a: out.append('%d'%round((ids[b][1]-ids[a][1])/w))
        t+=w
    print(p.split('/')[-1], ' '.join(out))
