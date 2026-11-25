import pickle,struct,collections
t0,rows=pickle.load(open('/tmp/q2srv/sbrows.pkl','rb'))
w=[r for r in rows if r[1]=='write' and len(r[3])>=4 and r[3][0]==3]
print("== reg 0xef (SET_DATA), 68 payload bytes = 4 blocks x 17")
for ts,op,dev,b,n,rv in w:
    if b[3]==0xef:
        d=b[4:]
        print('%.3fs'%(ts-t0), 'len',len(d))
        for i in range(0,len(d),17):
            blk=d[i:i+17]
            print('   ', blk.hex(), ' u32 id=%#x'%struct.unpack_from('<I',blk,0)[0], 'fields:',struct.unpack_from('<IBII',blk,4) if len(blk)>=17 else '')
print("== reg 0xa1 stream: payload 211 bytes")
a=[(ts,b[4:]) for ts,op,dev,b,n,rv in w if b[3]==0xa1]
print(len(a),'writes from %.3fs to %.3fs'%(a[0][0]-t0,a[-1][0]-t0))
d=a[0][1]
print('first:', d[:60].hex())
chg=collections.defaultdict(set)
for ts,d in a:
    for i,x in enumerate(d): chg[i].add(x)
print('offsets that change:',[i for i in sorted(chg) if len(chg[i])>1])
print('time series (grp,id,u16@8,u16@10, u16@12..)')
for ts,d in a[::10]:
    print('%.2fs'%(ts-t0), struct.unpack_from('<II',d,0), struct.unpack_from('<HHHH',d,8), d[100:116].hex())
