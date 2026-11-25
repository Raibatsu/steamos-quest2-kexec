#!/usr/bin/python3
# Adds generic Touch-controller register writes to /usr/bin/q2headtrack (run on the headset as root after patch_led.py; idempotent).
# Decoded from Horizon with strace (2026-11-25): a controller register request is an MCU SET_DATA/GET_DATA with a 20-byte header
#   [u64 controller id][8 zero bytes][u16 timeout 2400][1 pad][u8 controller register] + data,
# sent to MCU register 0x8f for controller registers 0x06 (=01 at connect) and 0x28 (LED sync), and to MCU register 0xe2 for 0x97 (=00 at tracking start)
# and 0x9b (haptics, 5 channel bytes). Only these four controller registers are allowed, data up to 32 bytes.
#   request file /dev/shm/q2ht-led (root only):   "reg left|right|both <REG hex> <DATA hex>"      e.g. "reg both 06 01", "reg left 9b 19 00 32 32 32 32 32"
#   saved at /var/lib/q2/ctlinit ("<REG hex> <DATA hex>" per line): applied to every controller when it connects or comes back after >1.5 s silence
import ast, shutil, os, sys
p = '/usr/bin/q2headtrack'
s = open(p).read()
if 'CTLINIT = ' in s: print('already patched'); sys.exit(0)
assert 'LEDF = ' in s, 'run patch_led.py first'
if not os.path.exists(p + '.prectlreg'): shutil.copy2(p, p + '.prectlreg')

helpers = '''CTLINIT = '/var/lib/q2/ctlinit'
CTLREGS = {0x06: 0x8f, 0x28: 0x8f, 0x97: 0xe2, 0x9b: 0xe2}      # controller register -> MCU register Horizon uses for it
def ctl_write(c, reg, data):
    if reg not in CTLREGS or len(data) > 32: log('ctlreg: refused reg %02x (%d bytes)' % (reg, len(data))); return
    LEDSEQ[0] = (LEDSEQ[0] + 1) % 32
    d = bytes([CTLREGS[reg]]) + struct.pack('<Q8xHxB', c.id, 2400, reg) + bytes(data)
    os.write(cmd, bytes([3, 0xa0 + LEDSEQ[0], len(d)]) + d)
    log('ctlreg: %s controller: register %02x = %s' % (c.name, reg, bytes(data).hex()))
def ctlinit_lines():
    try: return [l.split() for l in open(CTLINIT).read().split('\\n') if l.strip() and not l.startswith('#')]
    except Exception: return []
'''
a = 'def ctl_packet(ty, d):'
assert s.count(a) == 1
s = s.replace(a, helpers + a, 1)

old = "                try: led_set(c, _cfg[0], _cfg[1], _cfg[2], '(saved /var/lib/q2/ledsync, controller (re)connected)')\n                except Exception as e: log('led: apply failed: %s' % e)\n"
assert old in s
new = old + '''        if _gap > 1.5:
            for _w in ctlinit_lines():
                try: ctl_write(c, int(_w[0], 16), bytes(int(x, 16) for x in _w[1:]))
                except Exception as e: log('ctlreg: apply failed: %s' % e)
'''
s = s.replace(old, new, 1)

old2 = "                    elif w[0] == 'set':\n                        for c in targets: led_set(c, int(w[2]), int(w[3]), int(w[4]), '(request)')\n"
assert old2 in s
new2 = old2 + "                    elif w[0] == 'reg':\n                        for c in targets: ctl_write(c, int(w[2], 16), bytes(int(x, 16) for x in w[3:]))\n"
s = s.replace(old2, new2, 1)
ast.parse(s)
open(p, 'w').write(s)
print('patched', p)
