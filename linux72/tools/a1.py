import re,sys,struct
rows=[]
for ln in open(sys.argv[1],errors='replace'):
    m=re.search(r'^\d+\s+([\d.]+)\s+write\((\d+), "((?:\\x[0-9a-f]{2})*)"(\.\.\.)?, (\d+)\)',ln)
    if not m or m.group(2)!='20': continue
    b=bytes.fromhex(m.group(3).replace('\\x',''))
    if len(b)>=211 and b[0]==3 and b[3]==0xa1: rows.append((float(m.group(1)),b))
t0=rows[0][0]
print(len(rows),'writes; payload len',len(rows[0][1])-3)
for ts,b in rows[:14]:
    d=b[4:]
    print('%5.2f'%(ts-t0),' '.join('%02x'%x for x in d[:44]))
# which bytes ever change
import collections
chg=collections.defaultdict(set)
for ts,b in rows:
    for i,x in enumerate(b[4:]): chg[i].add(x)
print('byte offsets that change:',[i for i in sorted(chg) if len(chg[i])>1][:40])
