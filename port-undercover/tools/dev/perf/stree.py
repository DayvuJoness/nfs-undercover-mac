import re,sys,collections
# Parse one thread's call tree from macOS `sample` output: inclusive and self counts per symbol.
lines=open(sys.argv[1]).read().split('\n')
start=int(sys.argv[2]); end=int(sys.argv[3]); top=int(sys.argv[4]) if len(sys.argv)>4 else 40
pat=re.compile(r'^([ +!:|]*)(\d+) (.+?)  \(in ([^)]+)\)')
nodes=[]
for l in lines[start-1:end]:
    m=pat.match(l)
    if not m: continue
    depth=len(m.group(1)); cnt=int(m.group(2)); sym=m.group(3).strip()
    nodes.append((depth,cnt,sym))
incl=collections.Counter(); selfc=collections.Counter(); stack=[]
for i,(d,cnt,sym) in enumerate(nodes):
    while stack and stack[-1][0]>=d: stack.pop()
    if sym not in [s for _,s in stack]: incl[sym]+=cnt
    stack.append((d,sym))
    ch=0
    for d2,c2,s2 in nodes[i+1:]:
        if d2<=d: break
        # direct children: the first deeper level
    # self = cnt - sum(direct children)
for i,(d,cnt,sym) in enumerate(nodes):
    j=i+1; kids=0; cd=None
    while j<len(nodes) and nodes[j][0]>d:
        if cd is None: cd=nodes[j][0]
        if nodes[j][0]==cd: kids+=nodes[j][1]
        j+=1
    selfc[sym]+=cnt-kids
print("inclusive:"); [print("  %5d %s"%(v,k)) for k,v in incl.most_common(top)]
print("self:"); [print("  %5d %s"%(v,k)) for k,v in selfc.most_common(25)]
