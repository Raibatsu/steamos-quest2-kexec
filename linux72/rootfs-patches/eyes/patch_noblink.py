p = '/usr/bin/q2buttons'
s = open(p).read()
old = "    _blink(2 if new in (lo, hi) else 1)\n"
assert s.count(old) == 1
s = s.replace(old, "")
import ast; ast.parse(s)
open(p, 'w').write(s)
print('blink removed')
