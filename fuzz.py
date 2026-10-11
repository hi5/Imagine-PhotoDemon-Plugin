import random, subprocess, sys
random.seed(1234)
files=[open(p,'rb').read() for p in sys.argv[1:]]
bad=0; n=0
for it in range(500):
    d=bytearray(random.choice(files))
    mode=random.random()
    if mode<0.25: d=d[:random.randrange(0,len(d))]
    else:
        for _ in range(random.choice([1,1,2,5,20])):
            i=random.randrange(0,len(d)) if random.random()<0.7 else random.randrange(0,min(len(d),700))
            d[i]=random.randrange(256)
    open('/tmp/fz.pdi','wb').write(d)
    r=subprocess.run(['./out/pditest','/tmp/fz.pdi','/tmp/fz.pam','hook'],capture_output=True,timeout=60)
    n+=1
    if r.returncode not in (0,1,2,3) or b'ERROR' in r.stderr or b'runtime error' in r.stderr:
        bad+=1; open('/tmp/fz_bad_%d.pdi'%bad,'wb').write(d); print('BAD',r.returncode,r.stderr[:400].decode(errors='replace'))
        if bad>5: break
print('runs',n,'bad',bad)
