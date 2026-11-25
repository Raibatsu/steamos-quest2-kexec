import re,sys,collections,struct
c=collections.Counter(); ex={}
for f in sys.argv[1:]:
    t0=None
    for ln in open(f,errors='replace'):
        m=re.search(r'^\d+\s+([\d.]+)\s+write\((\d+), "((?:\\x[0-9a-f]{2})*)"(\.\.\.)?, (\d+)\)',ln)
        if not m or m.group(2)!='20': continue
        ts=float(m.group(1)); t0=t0 or ts
        b=bytes.fromhex(m.group(3).replace('\\x',''))
        if len(b)<3 or b[0] not in (2,3): continue
        key=(b[0], b[3] if len(b)>3 else -1)
        c[key]+=1; ex.setdefault(key,[]).append((round(ts-t0,2),b[2],b[3:3+40].hex()))
for k,v in sorted(c.items()): print('type %d mcureg %02x x%d  first: %s' % (k[0],k[1],v,ex[k][0]))
