import ast
p='/usr/bin/q2headtrack'; s=open(p).read()
old="                    os.write(cmd, b); log('sbcam: sent %s' % b.hex())"
assert s.count(old)==1, s.count(old)
i=s.index(old); tail=s[i:i+700]
j=tail.index('time.sleep(0.05)')
new_tail=tail[:j]+"time.sleep({0x2e: 1.5, 0x2d: 0.4, 0x2f: 0.4, 0x2c: 0.3}.get(b[0], 0.05))   # the MCU initialises/stops the sensors over I2C: give it time before the next command"+tail[j+len('time.sleep(0.05)'):]
s=s[:i]+new_tail+s[i+700:]
ast.parse(s); open(p,'w').write(s); print('delays added')
