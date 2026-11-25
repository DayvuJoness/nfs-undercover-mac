import csv,sys
# The scene load's busy stretch: the longest gap between two main-thread sleeps after the menu (t >= 55 s).
for p in sys.argv[1:]:
    rows=list(csv.DictReader(open(p))); t0=int(rows[0]['start_monotonic_ns'])
    ev=[((int(r['start_monotonic_ns'])-t0)/1e9, float(r['actual_ms'])/1e3, r['requested_ms']) for r in rows]
    ev=[e for e in ev if 55<=e[0]<75]
    gaps=[(b[0]-(a[0]+a[1]), a[0]+a[1]) for a,b in zip(ev,ev[1:])]
    g=max(gaps)
    # from the end of the menu-close animation's Sleep(0) loop to the first steady driving frame
    s1=sum(1 for e in ev if e[2]=='1' and g[1]<=e[0]<g[1]+1.5)
    w1=sum(e[1] for e in ev if e[2]=='1' and g[1]<=e[0]<g[1]+1.5)
    print("%-12s longest busy stretch %.0f ms at %.2f s; Sleep(1) after it: %d calls %.0f ms" % (p.split('/')[-1], g[0]*1e3, g[1], s1, w1*1e3))
