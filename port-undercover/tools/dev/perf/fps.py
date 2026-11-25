import csv, sys
rows = [r for r in csv.DictReader(open(sys.argv[1])) if r['repeat'] == '0']
t = [float(r['sealed_s']) for r in rows]
w = 5.0; start = t[0]; out = []
while start < t[-1]:
    out.append('%d' % round(sum(1 for x in t if start <= x < start + w) / w)); start += w
print(' '.join(out))
