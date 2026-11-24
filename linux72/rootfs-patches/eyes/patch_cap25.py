import ast
p='/usr/bin/q2buttons'; s=open(p).read()
old="max(-600.0, min(600.0,"; assert old in s
s=s.replace(old,"max(-250.0, min(250.0,"); ast.parse(s); open(p,'w').write(s); print('cap 25 mm')
