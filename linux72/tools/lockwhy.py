import re, sys, collections
t = collections.Counter(); n = 0
for ln in open(sys.argv[1]):
    if 'LED frames' not in ln: continue
    n += 1
    m = re.search(r"LED frames (\d+), steps (\d+), search tries (\d+) \(unclear (\d+), tilt mismatch (\d+), out of reach (\d+), unconfirmed (\d+), fits without IMU (\d+)\), seen by 2\+ cameras (\d+), typical error ([\d.]+) px", ln)
    if m:
        for k, v in zip(('led_frames', 'steps', 'search_tries', 'unclear', 'tilt_mismatch', 'out_of_reach', 'unconfirmed', 'fit_no_imu', 'seen_2plus_cams'), map(int, m.groups()[:9])): t[k] += v
    for side in ('LEFT', 'RIGHT'):
        m = re.search(side + r" tracked (\d+) found (\d+) lost\(few dots (\d+), bad fit (\d+)\)", ln)
        if m:
            for k, v in zip(('tracked', 'found', 'lost_few_dots', 'lost_bad_fit'), map(int, m.groups())): t[side + '_' + k] += v
print('seconds', n)
for k, v in sorted(t.items()): print('  %-24s %d' % (k, v))
