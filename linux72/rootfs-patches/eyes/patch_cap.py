p='/usr/bin/q2buttons'; s=open(p).read()
old="max(-150.0, min(150.0,"; assert old in s
s=s.replace(old,"max(-600.0, min(600.0,"); open(p,'w').write(s); print('cap 60 mm')
