import re,sys,collections,pickle
def unhex(s): return bytes.fromhex(s.replace('\\x','')).decode('latin1')
f=sys.argv[1]; fds={}; rows=[]; t0=None
for ln in open(f,errors='replace'):
    m=re.search(r'^(\d+)\s+([\d.]+)\s+openat\([^"]*"((?:\\x[0-9a-f]{2})*)".*\) = (\d+)',ln)
    if m:
        p=unhex(m.group(3)); t0=t0 or float(m.group(2))
        if '/dev/' in p: fds[m.group(4)]=p
        continue
    m=re.search(r'^(\d+)\s+([\d.]+)\s+(write|read)\((\d+), "((?:\\x[0-9a-f]{2})*)"(\.\.\.)?, (\d+)\)\s+= (-?\d+)',ln)
    if m and fds.get(m.group(4),'').startswith('/dev/syncboss'):
        rows.append((float(m.group(2)),m.group(3),fds[m.group(4)],bytes.fromhex(m.group(5).replace('\\x','')),int(m.group(7)),int(m.group(8))))
print('devices opened:',sorted(set(fds.values())))
w=[r for r in rows if r[1]=='write']
print('syncboss writes:',len(w),'reads:',len(rows)-len(w))
c=collections.Counter(); first={}
for ts,op,dev,b,n,rv in w:
    if len(b)<3: continue
    key=(dev.split('/')[-1],b[0],b[3] if len(b)>3 else -1)
    c[key]+=1; first.setdefault(key,(round(ts-t0,3),len(b),b[:40].hex()))
for k,v in sorted(c.items(), key=lambda kv: first[kv[0]][0]): print('%-14s type %02x reg %02x x%-4d @%.3fs len %-4d %s'%(k[0],k[1],k[2],v,first[k][0],first[k][1],first[k][2]))
pickle.dump((t0,rows),open(sys.argv[2],'wb'))
