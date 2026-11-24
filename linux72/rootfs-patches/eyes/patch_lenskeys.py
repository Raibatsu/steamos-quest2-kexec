import ast
p='/usr/bin/q2buttons'; s=open(p).read()
old="new = round(max(-15.0, min(15.0, cur + (0.5 if up else -0.5))), 1)"
assert old in s
s=s.replace(old,"new = round(max(-150.0, min(150.0, cur + (5.0 if up else -5.0))), 1)")
s=s.replace("log('lens centre %+.1f mm toward the nose - VR restarts in 3 s' % new)","log('lens position %+.1f mm toward the nose - VR restarts after your last tap' % (new / 10.0))")
# lock the keys to the lens mode: the both-keys tap no longer switches modes
t="def toggle_volmode():\n"; assert t in s
s=s.replace("    if volmode() == 'lens':\n        if not repeat: lensstep(up)\n        return\n","    if True:\n        if not repeat: lensstep(up)\n        return\n")
ast.parse(s); open(p,'w').write(s); print('ok')
