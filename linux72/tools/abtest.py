import re, sys
n = int(sys.argv[1])
L = [l for l in open('/var/log/q2/ctltrack.txt', errors='replace') if 'LED frames' in l][-n:]
res = {}
for h in ('LEFT', 'RIGHT'):
    ok = 0; leds = []
    for l in L:
        seg = l.split(h + ' tracked', 1)[1] if (h + ' tracked') in l else ''
        if seg and 'not seen' not in seg.split('|')[0]:
            ok += 1
            m = re.search(r'\((\d+) LEDs', seg)
            if m: leds.append(int(m.group(1)))
    res[h] = (ok, sum(leds) / len(leds) if leds else 0)
tries = [int(m.group(1)) for l in L for m in [re.search(r'search tries (\d+)', l)] if m]
fr = [int(m.group(1)) for l in L for m in [re.search(r'LED frames (\d+)', l)] if m]
print('  seconds analysed %d | LED frames/s %.0f | search tries/s %.1f | LEFT locked %d s (avg %.1f LEDs) | RIGHT locked %d s (avg %.1f LEDs)' %
      (len(L), sum(fr) / max(1, len(fr)), sum(tries) / max(1, len(tries)), res['LEFT'][0], res['LEFT'][1], res['RIGHT'][0], res['RIGHT'][1]))
