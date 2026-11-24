p='/usr/bin/q2buttons'; s=open(p).read()
a="('eyeshift', 2, -200, 200, 0, 'px')"; assert a in s
s=s.replace(a,"('eyeshift', 6, -200, 200, 0, 'px')"); open(p,'w').write(s); print('step 6')
