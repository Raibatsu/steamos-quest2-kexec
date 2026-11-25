import re,sys,struct
rows=[]
for ln in open(sys.argv[1],errors='replace'):
    m=re.search(r'^\d+\s+([\d.]+)\s+write\((\d+), "((?:\\x[0-9a-f]{2})*)"(\.\.\.)?, (\d+)\)',ln)
    if not m or m.group(2)!='20': continue
    b=bytes.fromhex(m.group(3).replace('\\x',''))
    if len(b)>=211 and b[0]==3 and b[3]==0xa1: rows.append((float(m.group(1)),b[4:]))
t0=rows[0][0]
for ts,d in rows[8:41:3]:
    g,i=struct.unpack_from('<II',d,0); e=struct.unpack_from('<HH',d,8); f=struct.unpack_from('<IIII',d,100)
    print('%5.2f grp=%d id=%02x  u16@8=%d u16@10=%d  @100..: %s' % (ts-t0,g,i,e[0],e[1],' '.join('%d'%x for x in f)))
