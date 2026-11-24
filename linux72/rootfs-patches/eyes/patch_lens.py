import ast
# 1) launcher exports Q2VR_LENSADJ from /var/lib/q2/lensadj
p='/usr/bin/q2vrtest'; s=open(p).read()
key='export Q2VR_EYESWAP=$EYESWAP '
assert s.count(key)==1 and 'Q2VR_LENSADJ' not in s
s=s.replace(key,'export Q2VR_LENSADJ=$(cat /var/lib/q2/lensadj 2>/dev/null | tr -cd "0-9.+-" | head -c 8); [ -n "$Q2VR_LENSADJ" ] || Q2VR_LENSADJ=0\n'+key)
open(p,'w').write(s)
# 2) volume keys: mode lensadj (0.5 mm steps, restarts VR after 3 s) or eyeshift; both keys tapped = switch mode
p='/usr/bin/q2buttons'; s=open(p).read()
assert 'def lensstep' not in s
helper='''
_rs = {'at': 0.0, 'busy': False}
def vr_restart_soon():
    _rs['at'] = time.time() + 3.0
    if _rs['busy']: return
    _rs['busy'] = True
    def run():
        try:
            while time.time() < _rs['at']: time.sleep(0.2)
            subprocess.run(['/usr/bin/q2vr', 'stop'], timeout=30)
            t0 = time.time(); time.sleep(2)
            while time.time() - t0 < 60:
                try: st = open('/tmp/q2vr/state').read().strip()
                except Exception: st = 'stopped'
                if st not in ('running', 'stopping', 'starting'): break
                time.sleep(0.5)
            time.sleep(2); subprocess.run(['/usr/bin/q2vr', 'start'], timeout=30); log('VR restarted with the new lens setting')
        except Exception as e: log('restart: %s' % e)
        finally: _rs['busy'] = False
    threading.Thread(target=run, daemon=True).start()
def lensstep(up):
    try: cur = float(open('/var/lib/q2/lensadj').read().strip())
    except Exception: cur = 0.0
    new = round(max(-15.0, min(15.0, cur + (0.5 if up else -0.5))), 1)
    try: open('/var/lib/q2/lensadj', 'w').write('%.1f\\n' % new)
    except OSError as e: log('lensadj: %s' % e); return
    log('lens centre %+.1f mm toward the nose - VR restarts in 3 s' % new)
    vr_restart_soon()
def volmode():
    try: return open('/var/lib/q2/volmode').read().strip() or 'lens'
    except Exception: return 'lens'
def toggle_volmode():
    new = 'shift' if volmode() == 'lens' else 'lens'
    open('/var/lib/q2/volmode', 'w').write(new + '\\n'); log('volume keys now adjust: %s' % ('lens centre (mm, restarts VR)' if new == 'lens' else 'picture shift (px, live)'))
'''
m='def volkey(up, repeat):\n'
assert s.count(m)==1
s=s.replace(m, helper+m)
old="    now = time.time()\n    if repeat and now - _ipd_last[0] < 0.3: return"
assert s.count(old)==1
s=s.replace(old,"    if volmode() == 'lens':\n        if not repeat: lensstep(up)\n        return\n"+old)
# both keys tapped (<1.5 s) toggles the mode
old2="                both_since = 0.0\n                if val in (1, 2) and not combo_done: volkey("
assert s.count(old2)==1
s=s.replace(old2,"                if both_since and val == 0 and not combo_done and time.time() - both_since < 1.5: toggle_volmode(); combo_done = True\n"+old2)
ast.parse(s); open(p,'w').write(s); print('patched both')
