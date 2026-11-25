import sys, collections
import xml.etree.ElementTree as ET
root=ET.parse(sys.argv[1]).getroot(); thread_pat=sys.argv[2]; fn=sys.argv[3]; depth=int(sys.argv[4]) if len(sys.argv)>4 else 4
frames={}; bts={}; threads={}
def fname(el):
    if el.get('ref'): return frames.get(el.get('ref'),'?')
    n=el.get('name') or el.get('addr') or '?'; frames[el.get('id')]=n; return n
def bt(el):
    if el is None: return []
    if el.get('ref'): return bts.get(el.get('ref'),[])
    fs=[fname(f) for f in el.findall('frame')]; bts[el.get('id')]=fs; return fs
def th(el):
    if el.get('ref'): return threads.get(el.get('ref'),'?')
    n=el.get('fmt'); threads[el.get('id')]=n; return n
c=collections.Counter(); n=0
for row in root.iter('row'):
    t=row.find('thread'); tn=th(t) if t is not None else '?'
    fs=bt(row.find('backtrace'))
    if thread_pat not in tn: continue
    for i,f in enumerate(fs):
        if fn in f:
            n+=1; c[' <- '.join(x[:60] for x in fs[i+1:i+1+depth])]+=1; break
print(n)
for k,v in c.most_common(15): print(v,k)
