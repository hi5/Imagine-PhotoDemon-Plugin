import struct, zlib, sys
def chunk(cid, payload, comp='none'):
    if comp=='none': data=payload
    elif comp=='zlib': data=zlib.compress(payload)
    elif comp=='defl':
        c=zlib.compressobj(9,zlib.DEFLATED,-15); data=c.compress(payload)+c.flush()
    body=comp.ljust(4).encode()+struct.pack('<ii',len(payload),len(data))+data
    return cid.encode()+struct.pack('<i',len(body))+body
def tag(**kw): return ''.join('<%s>%s</%s>'%(k.replace('_','-'),v,k.replace('_','-')) for k,v in kw.items())
def lhdr(name,w,h,depth=32,**kw):
    d=dict(layer_version=1,layer_id=0,type='rast',name=name,group_id=0,visible='True',opacity=100,blend_mode='norm',alpha_mode='norm',
           offset_x=0,offset_y=0,stretch_x=1,stretch_y=1,shear_x=0,shear_y=0,angle=0,rotation_center_x='.5',rotation_center_y='.5',
           sample_quality='blnr',frame_time_ms=0,mask_exists='False',mask_active='False',px_color_depth=depth,
           px_color_details='b8g8r8a8' if depth==32 else 'b8g8r8',px_width=w,px_height=h,px_stride=w*depth//8 if depth==32 else (w*3+3)//4*4,
           px_alpha_premultiplied='True'); d.update(kw)
    return '<version>1</version><photodemon-layer>'+tag(**d)+'</photodemon-layer>'
def ihdr(w,h): return '<version>1</version><photodemon-image>'+tag(image_version=1,image_id=0,width=w,height=h,x_resolution=300,y_resolution=300,dpi=300,layer_count_active=5)+'</photodemon-image>'
def dib(w,h,fn,depth=32):
    rows=[]
    for y in range(h):   # top-down (PhotoDemon DIBs)
        r=bytearray()
        for x in range(w):
            R,G,B,A=fn(x,y)
            if depth==32: r+=bytes([B*A//255,G*A//255,R*A//255,A])
            else: r+=bytes([B,G,R])
        while len(r)%4: r.append(0)
        rows.append(bytes(r))
    return b''.join(rows)
W,H=240,160
out=b'PDPKCHNKPDIF'
out+=chunk('IHDR',ihdr(W,H).encode(),'zlib')
# 1 background gradient opaque
out+=chunk('LHDR',lhdr('bg',W,H).encode()); out+=chunk('LDAT',dib(W,H,lambda x,y:(x*255//W,y*255//H,128,255)),'defl')
# 2 rotated + stretched 'F' shaped marker, 40x40 (arrow to show orientation: top bar white, left bar white)
def marker(x,y):
    if y<6 or x<6: return (255,255,255,255)
    if (x-y)==0: return (255,0,0,255)
    return (0,0,0,90)
out+=chunk('LHDR',lhdr('rot',40,40,offset_x=100,offset_y=20,angle=30,stretch_x=1.5,stretch_y=1.5).encode()); out+=chunk('LDAT',dib(40,40,marker),'zlib')
# 3 unrotated marker for reference (nearest sampling)
out+=chunk('LHDR',lhdr('ref',40,40,offset_x=10,offset_y=10,sample_quality='near').encode()); out+=chunk('LDAT',dib(40,40,marker))
# 4 multiply blend 24-bit yellow block at partial opacity, shear
out+=chunk('LHDR',lhdr('mult',60,40,depth=24,offset_x=150,offset_y=90,blend_mode='mult',opacity=80,shear_x=0.5).encode()); out+=chunk('LDAT',dib(60,40,lambda x,y:(255,220,0,255),24))
# 5 hidden layer
out+=chunk('LHDR',lhdr('hid',W,H,visible='False').encode()); out+=chunk('LDAT',dib(W,H,lambda x,y:(0,255,0,255)))
# 6 screen blend circle-ish with screen mode partially off-canvas (negative offset)
out+=chunk('LHDR',lhdr('scr',50,50,offset_x=-20,offset_y=110,blend_mode='diff').encode()); out+=chunk('LDAT',dib(50,50,lambda x,y:(255,255,255,255) if (x-25)**2+(y-25)**2<600 else (0,0,0,0)))
# 7 straight-alpha layer
out+=chunk('LHDR',lhdr('straight',30,30,offset_x=190,offset_y=10,px_alpha_premultiplied='False').encode())
sp=bytearray()
for y in range(30):
    for x in range(30): sp+=bytes([0,0,255,128])   # straight red @50%
out+=chunk('LDAT',bytes(sp))
out+=b'PEND'+struct.pack('<i',0)
open(sys.argv[1],'wb').write(out)
