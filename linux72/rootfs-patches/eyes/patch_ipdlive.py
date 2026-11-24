import ast
p='/usr/bin/q2buttons'; s=open(p).read()
old="    if True:\n        if not repeat: lensstep(up)\n        return\n"
assert s.count(old)==1
s=s.replace(old,"")
ast.parse(s); open(p,'w').write(s); print('lens-restart mode removed')
