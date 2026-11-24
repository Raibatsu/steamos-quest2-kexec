p='/usr/bin/q2vrtest'; s=open(p).read()
a=s.index('export Q2VR_LENSADJ=$(cat'); b=s.index('\n',a)
s=s[:a]+"export Q2VR_LENSADJ=$(awk 'NR==1{printf \"%.1f\", $1+0}' /var/lib/q2/lensadj 2>/dev/null); [ -n \"$Q2VR_LENSADJ\" ] || Q2VR_LENSADJ=0"+s[b:]
open(p,'w').write(s); print('ok')
