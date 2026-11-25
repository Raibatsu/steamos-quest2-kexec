#!/usr/bin/python3
# Adds Touch-controller IR-LED sync control to /usr/bin/q2headtrack (run on the headset as root; idempotent; keeps /usr/bin/q2headtrack.preled).
#
# Protocol (decoded 2026-11-26 from Meta's libsyncboss + the controllers' own firmware, verified on hardware: write returns status 0, read-back matches):
#   controller register 0x28 = IR-LED sync trigger: { u32 period_us, u32 on_time_us, s32 offset_us }   (controller defaults seen: 39996, 34, 29830)
#   write = MCU SET_DATA (type 3) register 0x8f, read = GET_DATA (type 2) register 0x8f, data = 20-byte header + (write only) the 12 bytes:
#     header = [u64 controller id][8 zero bytes][u16 timeout ms = 2400][1 pad][u8 controller register = 0x28]
#   the answer to a read arrives on /dev/syncboss_control0 as a type 0x8f record with the same header and the 12 data bytes at offset 20.
#   firmware asserts (jedi-app sync_helpers.c): period != 0, offset < period, period > |offset|.  We additionally bound: 1000 <= period <= 200000, 1 <= on_time <= 2000.
# Features added to the tracker:
#   * /dev/shm/q2ht-led (root-owned, like q2ht-sbcam): lines "get left|right|both" and "set left|right|both PERIOD ONTIME OFFSET"
#   * /var/lib/q2/ledsync "PERIOD ONTIME OFFSET": applied to every controller when it connects or reappears after >1.5 s of silence (the controllers fall back to
#     their own schedule after sleeping)
import ast, os, shutil, sys
p = '/usr/bin/q2headtrack'
s = open(p).read()
if 'LEDF = ' in s: print('already patched'); sys.exit(0)
shutil.copy2(p, p + '.preled') if not os.path.exists(p + '.preled') else None

helpers = '''# ---- Touch controller IR-LED sync: controller register 0x28 = {u32 period_us, u32 on_time_us, s32 offset_us} (see rootfs-patches/led/patch_led.py in the port repo)
LEDF = '/var/lib/q2/ledsync'
LEDC = {'t': 0.0, 'cfg': None}
def led_ok(p, o, f): return 1000 <= p <= 200000 and 1 <= o <= 2000 and abs(f) < p
def led_cfg():
    if time.time() - LEDC['t'] > 5.0:
        LEDC['t'] = time.time()
        try:
            v = open(LEDF).read().split(); c = (int(v[0]), int(v[1]), int(v[2]))
            LEDC['cfg'] = c if led_ok(*c) else None
        except Exception: LEDC['cfg'] = None
    return LEDC['cfg']
LEDSEQ = [0]
def led_frame(ty, cid, data=b''):
    LEDSEQ[0] = (LEDSEQ[0] + 1) % 32
    d = bytes([0x8f]) + struct.pack('<Q8xHxB', cid, 2400, 0x28) + data
    return bytes([ty, 0xa0 + LEDSEQ[0], len(d)]) + d
def led_set(c, p, o, f, why=''):
    if not led_ok(p, o, f): log('led: refused %s' % ((p, o, f),)); return
    os.write(cmd, led_frame(3, c.id, struct.pack('<IIi', p, o, f)))
    log('led: %s controller: period %d us, on-time %d us, offset %d us %s' % (c.name, p, o, f, why))
def led_get(c):
    os.write(cmd, led_frame(2, c.id)); log('led: read of register 0x28 requested for the %s controller (answer on /dev/syncboss_control0)' % c.name)
'''
a = 'def ctl_packet(ty, d):'
assert s.count(a) == 1
s = s.replace(a, helpers + a)

hook_after = "    c = ctl_get(cid, ptype); c.npk += 1\n"
assert s.count(hook_after) == 1
hook = hook_after + '''    if reg < 2 and not FAKE:                      # LED sync: (re)apply the saved setting when a controller connects or comes back
        _now = time.time(); _gap = _now - getattr(c, 'lastpk', 0.0); c.lastpk = _now
        if _gap > 1.5:
            _cfg = led_cfg()
            if _cfg:
                try: led_set(c, _cfg[0], _cfg[1], _cfg[2], '(saved /var/lib/q2/ledsync, controller (re)connected)')
                except Exception as e: log('led: apply failed: %s' % e)
'''
s = s.replace(hook_after, hook)

trig_before = 'def axes_str(m):'
assert s.count(trig_before) == 1
trig = '''    # LED sync requests (root-owned file in /dev/shm): "get left|right|both" / "set left|right|both PERIOD ONTIME OFFSET"
    if not FAKE and os.path.exists(TRIG + 'led'):
        try:
            st_ = os.stat(TRIG + 'led'); lines = open(TRIG + 'led').read().split('\\n')
        except Exception:
            st_ = None; lines = []
        try: os.unlink(TRIG + 'led')
        except Exception: pass
        if st_ is not None and st_.st_uid == 0:
            for ln in lines[:8]:
                w = ln.split()
                if not w: continue
                try:
                    targets = [c for c in CTLS.values() if w[1] == 'both' or c.name == w[1]]
                    if not targets: log('led: no %s controller connected' % w[1]); continue
                    if w[0] == 'get':
                        for c in targets: led_get(c)
                    elif w[0] == 'set':
                        for c in targets: led_set(c, int(w[2]), int(w[3]), int(w[4]), '(request)')
                    else: log('led: unknown request %r' % ln)
                except Exception as e: log('led: bad request %r: %s' % (ln, e))
'''
s = s.replace(trig_before, trig + trig_before)
ast.parse(s)
open(p, 'w').write(s)
print('patched', p)
