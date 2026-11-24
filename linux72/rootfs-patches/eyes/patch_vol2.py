p = '/usr/bin/q2buttons'
s = open(p).read()
start = s.index('# Volume keys set the lens spacing')
end = s.index('fds = {}\ndef reopen():')
new = '''# Volume keys calibrate the lens placement live: up = pictures toward the nose (+2 px), down = away (-2 px); hold repeats.
# Each step dips the backlight briefly so a press is visible. /var/lib/q2/volkeys: "eyeshift" (default), "ipdmm" (virtual eye
# distance, 1 mm steps) or "volume" (the original system volume).
import threading
_ipd_last = [0.0]
def _ipd_default():
    try: return {'1': 58, '3': 68}.get(open('/var/lib/q2/ipdpos').read().strip(), 63)
    except Exception: return 63
def _blink(times=1):
    def run():
        try:
            bl = '/sys/class/backlight/hollywood-jdi/brightness'
            cur = int(open(bl).read().strip())
            for _ in range(times):
                open(bl, 'w').write(str(max(1, cur // 6))); time.sleep(0.09)
                open(bl, 'w').write(str(cur)); time.sleep(0.09)
        except Exception as e: log('blink: %s' % e)
    threading.Thread(target=run, daemon=True).start()
def volkey(up, repeat):
    try: mode = open('/var/lib/q2/volkeys').read().strip()
    except Exception: mode = 'eyeshift'
    if mode == 'volume':
        volume('5%+' if up else '5%-'); return
    now = time.time()
    if repeat and now - _ipd_last[0] < 0.3: return
    _ipd_last[0] = now
    f, step, lo, hi, dflt, unit = (('ipdmm', 1, 40, 80, _ipd_default(), 'mm') if mode == 'ipdmm' else ('eyeshift', 2, -200, 200, 0, 'px'))
    try: cur = int(open('/var/lib/q2/' + f).read().strip())
    except Exception: cur = dflt
    new = max(lo, min(hi, cur + (step if up else -step)))
    try: os.makedirs('/var/lib/q2', exist_ok=True); open('/var/lib/q2/' + f, 'w').write('%d\\n' % new)
    except OSError as e: log('%s: %s' % (f, e)); return
    log('%s %d %s (volume %s)' % (f, new, unit, 'up' if up else 'down'))
    _blink(2 if new in (lo, hi) else 1)
'''
s = s[:start] + new + s[end:]
import ast; ast.parse(s)
open(p, 'w').write(s)
print('patched')
