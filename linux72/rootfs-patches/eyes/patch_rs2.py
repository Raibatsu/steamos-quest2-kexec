import ast
p='/usr/bin/q2buttons'; s=open(p).read()
old="""                subprocess.run(['/usr/bin/q2vr', 'start'], timeout=30)
                log('VR restarted with the new lens setting')"""
assert s.count(old)==1
new="""                for attempt in range(4):
                    subprocess.run(['/usr/bin/q2vr', 'start'], timeout=30)
                    time.sleep(25)
                    if subprocess.run(['pgrep', 'vrcompositor'], capture_output=True).returncode == 0: break
                    log('VR did not start (try %d) - asking again' % (attempt + 1))
                log('VR restarted with the new lens setting')"""
s=s.replace(old,new); ast.parse(s); open(p,'w').write(s); print('ok')
