import sys, collections
import xml.etree.ElementTree as ET
root=ET.parse(sys.argv[1]).getroot(); pat=sys.argv[2]
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
c=collections.Counter(); n=0; tot=0
for row in root.iter('row'):
    t=row.find('thread'); tn=th(t) if t is not None else '?'
    fs=bt(row.find('backtrace'))
    if pat not in tn: continue
    tot+=1
    # outermost imports_dispatch
    idx=[i for i,f in enumerate(fs) if f.startswith('imports_dispatch')]
    if not idx: continue
    i=idx[-1]; n+=1
    c[fs[i-1][:70] if i>0 else 'self']+=1
print(tot,n)
for k,v in c.most_common(30): print('%5.1f%% %s'%(100*v/tot,k))
