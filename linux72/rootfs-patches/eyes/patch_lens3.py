p='/usr/bin/q2vrtest'; s=open(p).read()
a=s.index('export Q2VR_LENSADJ=$(awk'); b=s.index('\n',a)
s=s[:a]+"export Q2VR_LENSADJ=$(awk 'NR==1{printf \"%.1f\", $1+0}' /var/lib/q2/lensadj 2>/dev/null || echo 0)"+s[b:]
open(p,'w').write(s); print('ok')
