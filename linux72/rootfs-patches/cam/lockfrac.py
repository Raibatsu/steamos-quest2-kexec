import re, sys
for f in sys.argv[1:]:
    steps = 0; tr = {'LEFT': 0, 'RIGHT': 0}; fd = {'LEFT': 0, 'RIGHT': 0}; t = []
    for ln in open(f):
        m = re.search(r"q2ctltrack: ([\d.]+) LED frames (\d+), steps (\d+)", ln)
        if not m: continue
        t.append(float(m.group(1))); steps += int(m.group(3))
        for side in tr:
            mm = re.search(side + r" tracked (\d+) found (\d+)", ln)
            if mm: tr[side] += int(mm.group(1)); fd[side] += int(mm.group(2))
    span = (t[-1] - t[0]) if len(t) > 1 else 0
    print('%-14s lines %3d span %5.0fs steps %6d | LEFT tracked %5d (%.1f%% of steps) found %3d | RIGHT tracked %5d (%.1f%%) found %3d' % (f.split('/')[-1], len(t), span, steps, tr['LEFT'], 100.0 * tr['LEFT'] / max(1, steps), fd['LEFT'], tr['RIGHT'], 100.0 * tr['RIGHT'] / max(1, steps), fd['RIGHT']))
