import csv, collections, sys
for p in sys.argv[1:]:
    rows=[r for r in csv.DictReader(open(p))]
    t0=float(rows[0]['sealed_s'])
    sel=[r for r in rows if r['repeat']=='0' and 130<=float(r['sealed_s'])-t0<=190 and float(r['presented_s'])>0]
    pres=sorted(float(r['presented_s']) for r in sel)
    d=[(b-a)*1000 for a,b in zip(pres,pres[1:])]
    dbl=sum(1 for x in d if x>12.5)
    lat=sorted((float(r['presented_s'])-float(r['sealed_s']))*1000 for r in sel)
    print('%s: %.1f shown/s, held twice %.1f%%, seal->screen p50 %.1f p90 %.1f ms'%(p.split('/')[-1], len(d)/(pres[-1]-pres[0]), 100*dbl/len(d), lat[len(lat)//2], lat[int(len(lat)*.9)]))
