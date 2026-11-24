p='/usr/bin/q2buttons'; s=open(p).read()
a="('ipdmm', 1, 40, 80, _ipd_default(), 'mm')"; assert a in s
s=s.replace(a,"('ipdmm', 1, 20, 80, _ipd_default(), 'mm')"); open(p,'w').write(s); print('floor 20')
