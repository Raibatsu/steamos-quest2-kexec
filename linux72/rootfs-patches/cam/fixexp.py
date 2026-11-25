#!/usr/bin/python3
import sys, time
exec(open('/usr/bin/maskprobe.py').read().split("a = sys.argv[1:]")[0])
e, g = int(sys.argv[1]), int(sys.argv[2])
for _ in range(3): setgrp((1, 8), e, g); setgrp((4, 2), e, g)
time.sleep(0.8); print('fixed exposure', e, 'gain', g)
