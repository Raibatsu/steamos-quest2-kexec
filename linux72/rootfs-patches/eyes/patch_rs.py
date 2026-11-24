import ast
p='/usr/bin/q2buttons'; s=open(p).read()
a=s.index("_rs = {'at'"); b=s.index("def lensstep(up):")
new='''_rs = {'at': 0.0, 'busy': False}
def vr_restart_soon():
    _rs['at'] = time.time() + 6.0
    if _rs['busy']: return
    _rs['busy'] = True
    def run():
        try:
            while True:
                while time.time() < _rs['at']: time.sleep(0.2)
                began = time.time()
                subprocess.run(['/usr/bin/q2vr', 'stop'], timeout=30)
                t0 = time.time(); time.sleep(3)
                while time.time() - t0 < 90:
                    try: st = open('/tmp/q2vr/state').read().strip()
                    except Exception: st = 'stopped'
                    if st not in ('running', 'stopping', 'starting') and not os.path.exists('/tmp/q2vr/stop'): break
                    time.sleep(0.5)
                time.sleep(3)
                subprocess.run(['systemctl', 'restart', 'q2-vr'], timeout=60); time.sleep(5)
                subprocess.run(['/usr/bin/q2vr', 'start'], timeout=30)
                log('VR restarted with the new lens setting')
                if _rs['at'] <= began + 1.0: break      # no taps during the cycle
        except Exception as e: log('restart: %s' % e)
        finally: _rs['busy'] = False
    threading.Thread(target=run, daemon=True).start()
'''
s=s[:a]+new+s[b:]
ast.parse(s); open(p,'w').write(s); print('ok')
