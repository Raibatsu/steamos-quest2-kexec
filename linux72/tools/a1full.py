import pickle,struct
t0,rows=pickle.load(open('/tmp/q2srv/sbrows.pkl','rb'))
a=[(ts,b[4:],b[1]) for ts,op,dev,b,n,rv in rows if op=='write' and len(b)>=214 and b[0]==3 and b[3]==0xa1]
def show(d):
    for i in range(0,len(d),16): print('   %3d: %s'%(i,d[i:i+16].hex(' ')))
for k in (0,40):
    ts,d,sq=a[k]; print('message',k,'t=%.2f seq=%02x len=%d'%(ts-t0,sq,len(d))); show(d)
