#!/usr/bin/env python3
"""frames.bin(1024B/장, SSD1306 페이지 포맷)을 확대 대비 시트 PNG로."""
import zlib, struct, sys, os, re
W, H, FB = 128, 64, 1024
FONT_PATH = os.path.expanduser("~/Documents/Arduino/libraries/Adafruit_GFX_Library/glcdfont.c")
def load_font():
    t = open(FONT_PATH, errors="ignore").read()
    b = t[t.index("{"):t.rindex("}")]
    return [int(v,16) for v in re.findall(r"0[xX]([0-9a-fA-F]{2})", b)][:1280]
FONT = load_font()
def text(buf, bw, bh, x, y, s, col=(255,255,255)):
    for ch in s:
        c = ord(ch) if ord(ch) < 256 else 63
        for cx in range(5):
            bits = FONT[c*5+cx]
            for ry in range(8):
                if bits >> ry & 1:
                    px, py = x+cx, y+ry
                    if 0 <= px < bw and 0 <= py < bh: buf[py*bw+px] = col
        x += 6
def png(path, pix, w, h):
    raw = b"".join(b"\x00"+b"".join(bytes(pix[y*w+x]) for x in range(w)) for y in range(h))
    def ck(t,d):
        c=t+d; return struct.pack(">I",len(d))+c+struct.pack(">I",zlib.crc32(c)&0xffffffff)
    open(path,"wb").write(b"\x89PNG\r\n\x1a\n"
        + ck(b"IHDR", struct.pack(">IIBBBBB",w,h,8,2,0,0,0))
        + ck(b"IDAT", zlib.compress(raw,9)) + ck(b"IEND",b""))
def get(fr, x, y):
    return (fr[(y>>3)*W + x] >> (y & 7)) & 1

src = sys.argv[1] if len(sys.argv)>1 else "frames.bin"
picks = [int(v) for v in sys.argv[2].split(",")] if len(sys.argv)>2 and sys.argv[2] else None
out = sys.argv[3] if len(sys.argv)>3 else "sheet.png"
data = open(src,"rb").read()
n = len(data)//FB
frames = [data[i*FB:(i+1)*FB] for i in range(n)]
if picks is None:
    picks = [round(i*(n-1)/8) for i in range(9)]
Z, PAD, LBL, TOP = 4, 12, 11, 8
cols = 3; rows = (len(picks)+cols-1)//cols
PW, PH = W*Z, H*Z
sw = PAD + cols*(PW+PAD); sh = TOP + rows*(PH+LBL+PAD)
sheet = [(18,18,20)]*(sw*sh)
for i, fi in enumerate(picks):
    fr = frames[fi]; c, r = i%cols, i//cols
    ox = PAD + c*(PW+PAD); oy = TOP + r*(PH+LBL+PAD) + LBL
    text(sheet, sw, sh, ox, oy-LBL+1, f"frame {fi}", (120,220,255))
    for y in range(PH):
        for x in range(PW):
            sheet[(oy+y)*sw+ox+x] = (235,245,255) if get(fr, x//Z, y//Z) else (8,10,14)
png(out, sheet, sw, sh)
print(f"{out}  ({picks} of {n} frames)")
