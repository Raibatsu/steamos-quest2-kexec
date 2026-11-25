#!/usr/bin/python3
# sbshell: run an MCU console command through SyncBoss the way libsyncboss' syncboss_shell_run_command does:
#   {0x27, seq, len, "<command>"}  then poll {0x28, seq, 1, 0xf0} every 10 ms and print what comes back.
# Run with q2-headtrack stopped. Strict allowlist: only read-only diagnostics, plus (when --allow-set) nice_set_exposure.
import os, sys, time, select
ALLOW_READ = ('help', 'nice_log_config', 'nice_log_stats', 'nice_num_slots', 'version')
ALLOW_SET = ('nice_set_exposure',)
args = sys.argv[1:]; allow_set = False
if args and args[0] == '--allow-set': allow_set = True; args = args[1:]
C = '/sys/devices/virtual/misc/syncboss0/spi/control/'
def wr(p, v):
    try:
        with open(p, 'w') as f: f.write(str(v))
    except Exception: pass
wr(C + 'transaction_length', 248); wr(C + 'transaction_period_us', 1000); wr(C + 'minimum_time_between_transactions_us', 500)
cmd = os.open('/dev/syncboss0', os.O_RDWR)
st = os.open('/dev/syncboss_stream0', os.O_RDONLY | os.O_NONBLOCK)
ctl = os.open('/dev/syncboss_control0', os.O_RDONLY | os.O_NONBLOCK)
bufs = {'S': b'', 'C': b''}; seq = [0]
def records(dur):
    out = []; end = time.time() + dur
    while time.time() < end:
        r, _, _ = select.select([st, ctl], [], [], 0.02)
        for fd, tag in ((st, 'S'), (ctl, 'C')):
            if fd not in r: continue
            try: chunk = os.read(fd, 65536)
            except BlockingIOError: continue
            bufs[tag] += chunk; b = bufs[tag]
            while len(b) >= 3:
                ty, sq, l = b[0], b[1], b[2]
                if len(b) < 3 + l: break
                d = b[3:3 + l]; b = b[3 + l:]
                if ty in (0x01, 0x50, 0x8f, 0x7d, 0xfe) and not (tag == 'C' and ty == 0x7d): continue
                out.append((tag, ty, d))
            bufs[tag] = b
    return out
def send(ty, data):
    seq[0] = (seq[0] + 1) & 0xff
    os.write(cmd, bytes([ty, seq[0], len(data)]) + data)
os.write(cmd, bytes([0x6e, 1, 0])); records(1.0)
for line in args:
    word = line.split()[0] if line.split() else ''
    if word not in ALLOW_READ and not (allow_set and word in ALLOW_SET):
        print('REFUSED (not on the allowlist):', line); continue
    assert len(line) <= 0xf0
    print('> ' + line)
    for k in range(8): send(0x28, bytes([0xf0])); records(0.03)     # drain stale output first, as libsyncboss does
    send(0x27, line.encode() + b'\0'); got = records(0.3)
    for k in range(60):
        send(0x28, bytes([0xf0])); got += records(0.05)
    for tag, ty, d in got:
        if ty == 0x46 and len(d) == 1 and d[0] == 0: continue
        txt = ''.join(chr(c) if 32 <= c < 127 or c in (9, 10) else '.' for c in d)
        print('  [%s %#04x len=%d] %s' % (tag, ty, len(d), txt.rstrip() if ty in (0x92, 0xd8, 0x46, 0x28, 0x27) or txt.strip('.') else d[:16].hex()))
