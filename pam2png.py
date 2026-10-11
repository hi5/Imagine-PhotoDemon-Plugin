import sys, zlib, struct
def readpam(p):
    d=open(p,'rb').read(); i=d.index(b'ENDHDR\n')+7; h=d[:i].split()
    w=int(h[h.index(b'WIDTH')+1]); ht=int(h[h.index(b'HEIGHT')+1]); return w,ht,d[i:]
def png(w,h,rgb,path):
    raw=b''.join(b'\0'+rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def ch(t,b): c=struct.pack('>I',len(b))+t+b; return c+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
    open(path,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))
w,h,px=readpam(sys.argv[1]); scale=int(sys.argv[3]) if len(sys.argv)>3 else 1
out=bytearray()
for y in range(h):
    row=bytearray()
    for x in range(w):
        r,g,b,a=px[(y*w+x)*4:(y*w+x)*4+4]
        bg=200 if ((x//8+y//8)&1) else 255
        row+=bytes([(r*a+bg*(255-a))//255,(g*a+bg*(255-a))//255,(b*a+bg*(255-a))//255])
    for _ in range(scale):
        out+= b''.join(row[i:i+3]*scale for i in range(0,len(row),3))
png(w*scale,h*scale,bytes(out),sys.argv[2])
