import sys, collections
import xml.etree.ElementTree as ET
tree=ET.parse(sys.argv[1]); root=tree.getroot()
frames={}; bts={}; threads={}
def frame_name(el):
    if el.get('ref'): return frames.get(el.get('ref'),'?')
    n=el.get('name') or el.get('addr') or '?'
    frames[el.get('id')]=n; return n
def bt(el):
    if el is None: return []
    if el.get('ref'): return bts.get(el.get('ref'),[])
    fs=[frame_name(f) for f in el.findall('frame')]
    bts[el.get('id')]=fs; return fs
def th(el):
    if el.get('ref'): return threads.get(el.get('ref'),'?')
    n=el.get('fmt'); threads[el.get('id')]=n; return n
selfc=collections.defaultdict(collections.Counter); incl=collections.defaultdict(collections.Counter); tot=collections.Counter()
for row in root.iter('row'):
    t=row.find('thread'); b=row.find('backtrace'); st=row.find('thread-state')
    tn=th(t) if t is not None else '?'
    fs=bt(b)
    tot[tn]+=1
    if fs: selfc[tn][fs[0]]+=1
    for f in set(fs): incl[tn][f]+=1
for tn,n in tot.most_common(8):
    print('== %s: %d samples'%(tn[:60],n))
    for f,k in selfc[tn].most_common(int(sys.argv[2]) if len(sys.argv)>2 else 12): print('   self %5.1f%% %s'%(100*k/n,f[:90]))
