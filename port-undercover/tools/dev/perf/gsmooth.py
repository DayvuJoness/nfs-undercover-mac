import csv,sys
# Game frame intervals from frame_id and sealed_s: each row's frame_id; intervals between consecutive new ids.
p=sys.argv[1]; a=float(sys.argv[2]); b=float(sys.argv[3])
rows=[r for r in csv.DictReader(open(p))]
t0=float(rows[0]['sealed_s'])
pts=[(int(r['frame_id']),float(r['sealed_s'])) for r in rows if r['repeat']=='0' and a<=float(r['sealed_s'])-t0<=b]
# a sealed frame row covers frame_id; dropped ids between rows were finished but not presented
iv=[]
for (i0,s0),(i1,s1) in zip(pts,pts[1:]):
    if i1>i0: iv += [(s1-s0)*1000/(i1-i0)]*(i1-i0)
iv.sort(); n=len(iv)
q=lambda f: iv[min(n-1,int(f*n))]
print('%s: %d game frames, mean %.2f ms; p50 %.2f p90 %.2f p99 %.2f p99.9 %.2f max %.1f ms'%(p.split('/')[-1],n,sum(iv)/n,q(.5),q(.9),q(.99),q(.999),iv[-1]))
