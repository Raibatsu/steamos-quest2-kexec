p = '/usr/bin/q2buttons'
s = open(p).read()
assert 'def volkey(' not in s
helper = '''
# Volume keys set the lens spacing (eye separation) live, for calibrating by eye: up = +1 mm, down = -1 mm, hold repeats.
# /var/lib/q2/volkeys containing "volume" gives the original system-volume behaviour back.
_ipd_last = [0.0]
def _ipd_default():
    try: return {'1': 58, '3': 68}.get(open('/var/lib/q2/ipdpos').read().strip(), 63)
    except Exception: return 63
def volkey(up, repeat):
    try: mode = open('/var/lib/q2/volkeys').read().strip()
    except Exception: mode = 'ipd'
    if mode == 'volume':
        volume('5%+' if up else '5%-'); return
    now = time.time()
    if repeat and now - _ipd_last[0] < 0.35: return
    _ipd_last[0] = now
    try: cur = int(open('/var/lib/q2/ipdmm').read().strip())
    except Exception: cur = _ipd_default()
    new = max(40, min(80, cur + (1 if up else -1)))
    try: os.makedirs('/var/lib/q2', exist_ok=True); open('/var/lib/q2/ipdmm', 'w').write('%d\\n' % new)
    except OSError as e: log('ipd: %s' % e); return
    log('eye separation %d mm (volume %s)' % (new, 'up' if up else 'down'))
'''
marker = "fds = {}\ndef reopen():"
assert s.count(marker) == 1
s = s.replace(marker, helper + marker)
old = "if val in (1, 2) and not combo_done: volume('5%+' if code == KEY_VOLUP else '5%-')"
assert s.count(old) == 1
s = s.replace(old, "if val in (1, 2) and not combo_done: volkey(code == KEY_VOLUP, val == 2)")
import ast; ast.parse(s)
open(p, 'w').write(s)
print('patched q2buttons')
