import ast
p='/usr/bin/q2headtrack'; s=open(p).read()
old='b[0] in (0x2a, 0x2c, 0x2d, 0x28)'
assert s.count(old)==1
s=s.replace(old,'b[0] in (0x2a, 0x2c, 0x2d, 0x28, 0x2e, 0x2f)')
s=s.replace('# on: 0x2c start, 0x2d stop, 0x2a exposure/gain, 0x28 camera probe,','# on: 0x2c start, 0x2d stop, 0x2e init, 0x2f deinit (all with a 4-byte LE camera mask, as libsyncboss sends them), 0x2a exposure/gain, 0x28 camera probe,')
ast.parse(s); open(p,'w').write(s); print('whitelist now includes 0x2e 0x2f')
