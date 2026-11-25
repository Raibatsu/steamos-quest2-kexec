import ast
p='/usr/bin/q2headtrack'; s=open(p).read()
old="(b[0] == 0x03 and len(b) >= 4 and b[3] == 0xdd)"
assert s.count(old)==1, s.count(old)
s=s.replace(old,"(b[0] == 0x03 and len(b) >= 4 and b[3] in (0xdd, 0xc7, 0x64))")
ast.parse(s); open(p,'w').write(s); print('whitelist: SET_DATA registers 0xdd 0xc7 0x64')
