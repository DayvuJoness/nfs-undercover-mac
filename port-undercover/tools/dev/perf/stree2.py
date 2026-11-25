import re,sys,collections
# Busy-inclusive counts: each sample's leaf, if it is not a wait, is credited to every function on its stack.
lines=open(sys.argv[1]).read().split('\n'); start=int(sys.argv[2]); end=int(sys.argv[3])
pat=re.compile(r'^([ +!:|]*)(\d+) (.+?)  \(in ([^)]+)\)')
nodes=[]
for l in lines[start-1:end]:
    m=pat.match(l)
    if m: nodes.append((len(m.group(1)),int(m.group(2)),m.group(3).strip()))
wait=('psynch','semwait','wait_trap','mach_msg','kevent','nanosleep','__write','gettimeofday','workq','sleep_for','k_Sleep','sched_sleep','guest_block')
inc=collections.Counter(); stack=[]; total=0
for i,(d,cnt,sym) in enumerate(nodes):
    while stack and stack[-1][0]>=d: stack.pop()
    stack.append((d,sym))
    j=i+1; kids=0; cd=None
    while j<len(nodes) and nodes[j][0]>d:
        if cd is None: cd=nodes[j][0]
        if nodes[j][0]==cd: kids+=nodes[j][1]
        j+=1
    s=cnt-kids
    if s<=0 or any(w in x for _,x in stack for w in wait): continue
    total+=s
    for x in set(x for _,x in stack): inc[x]+=s
print("busy samples", total)
for k,v in inc.most_common(int(sys.argv[4])):
    if k.startswith(('fn_','body_')): print("  %4d %4.1f%% %s"%(v,100*v/total,k))
