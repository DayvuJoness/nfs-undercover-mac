import sys, collections
import xml.etree.ElementTree as ET
root=ET.parse(sys.argv[1]).getroot(); pat=sys.argv[2]; N=int(sys.argv[3]) if len(sys.argv)>3 else 40
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
inc=collections.Counter(); n=0
for row in root.iter('row'):
    t=row.find('thread'); tn=th(t) if t is not None else '?'
    fs=bt(row.find('backtrace'))
    if pat not in tn: continue
    n+=1
    for f in set(fs): inc[f]+=1
print(n)
for f,k in inc.most_common(N): print('%5.1f%% %s'%(100*k/n,f[:100]))
