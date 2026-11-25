import pickle,sys,struct
t0,rows=pickle.load(open(sys.argv[1],'rb'))
w=[r for r in rows if r[1]=='write' and len(r[3])>=3]
seen=set()
for ts,op,dev,b,n,rv in w:
    key=(b[0],b[3] if len(b)>3 else -1)
    if key in seen and key[1] in (0xa1,0x7c,0x8f): continue
    seen.add(key)
    if b[0] in (2,3) and len(b)>3 and b[3]==0xa1 and ('a1',) in seen and len(seen)>0: pass
    print('%8.3fs ty=%02x sq=%02x len=%-3d ret=%d  %s'%(ts-t0,b[0],b[1],b[2],rv,b[3:3+80].hex()))
