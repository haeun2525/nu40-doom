#!/usr/bin/env python3
"""밝기 3안을 같은 장면으로 나란히 비교. 세로=안, 가로=장면"""
import zlib, struct, os, re, sys
W,H,FB=128,64,1024
FONT_PATH=os.path.expanduser("~/Documents/Arduino/libraries/Adafruit_GFX_Library/glcdfont.c")
t=open(FONT_PATH,errors="ignore").read(); b=t[t.index("{"):t.rindex("}")]
FONT=[int(v,16) for v in re.findall(r"0[xX]([0-9a-fA-F]{2})",b)][:1280]
def text(buf,bw,bh,x,y,s,col=(255,255,255)):
    for ch in s:
        c=ord(ch) if ord(ch)<256 else 63
        for cx in range(5):
            bits=FONT[c*5+cx]
            for ry in range(8):
                if bits>>ry&1:
                    px,py=x+cx,y+ry
                    if 0<=px<bw and 0<=py<bh: buf[py*bw+px]=col
        x+=6
def png(path,pix,w,h):
    raw=b"".join(b"\x00"+b"".join(bytes(pix[y*w+x]) for x in range(w)) for y in range(h))
    def ck(t,d):
        c=t+d; return struct.pack(">I",len(d))+c+struct.pack(">I",zlib.crc32(c)&0xffffffff)
    open(path,"wb").write(b"\x89PNG\r\n\x1a\n"+ck(b"IHDR",struct.pack(">IIBBBBB",w,h,8,2,0,0,0))+ck(b"IDAT",zlib.compress(raw,9))+ck(b"IEND",b""))
def get(fr,x,y): return (fr[(y>>3)*W+x]>>(y&7))&1

import json
VARIANTS=json.loads(sys.argv[4]) if len(sys.argv)>4 else [("frames_l1.bin","1"),("frames_l2.bin","2"),("frames_l3.bin","3")]
PICKS=[int(v) for v in sys.argv[1].split(",")]
LABELS=sys.argv[2].split("|") if len(sys.argv)>2 else [str(p) for p in PICKS]
OUT=sys.argv[3] if len(sys.argv)>3 else "light_compare.png"
Z,PAD,LBL,TOP=4,14,11,20
cols,rows=len(PICKS),len(VARIANTS)
cw,chh=W*Z,H*Z+LBL
iw=PAD+cols*(cw+PAD); ih=TOP+PAD+rows*(chh+PAD)
pix=[(24,24,28)]*(iw*ih)
for ci,lab in enumerate(LABELS):
    text(pix,iw,ih,PAD+ci*(cw+PAD)+4,4,lab,(150,210,255))
for ri,(src,name) in enumerate(VARIANTS):
    data=open(src,"rb").read()
    for ci,idx in enumerate(PICKS):
        fr=data[idx*FB:(idx+1)*FB]
        ox=PAD+ci*(cw+PAD); oy=TOP+PAD+ri*(chh+PAD)
        text(pix,iw,ih,ox,oy,name,(255,220,120))
        for y in range(H):
            for x in range(W):
                c=(255,255,255) if get(fr,x,y) else (0,0,0)
                for dy in range(Z):
                    row=(oy+LBL+y*Z+dy)*iw+ox+x*Z
                    for dx in range(Z): pix[row+dx]=c
png(OUT,pix,iw,ih)
print(OUT,iw,"x",ih)
