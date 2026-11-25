import sys
for cam in range(4):
    d = open('/var/log/q2/camlive_cam%d_t8s_pgm.bin' % cam, 'rb').read(); parts = d.split(b'\n', 3); w, h = map(int, parts[1].split()); px = parts[3][:w*h]
    s = sorted(px); n = len(s)
    print('cam%d mean %.2f  p50 %d  p99 %d  p99.9 %d  p99.99 %d  max %d' % (cam, sum(px)/n, s[n//2], s[int(n*.99)], s[int(n*.999)], s[int(n*.9999)], s[-1]))
