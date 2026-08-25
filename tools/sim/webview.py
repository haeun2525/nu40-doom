#!/usr/bin/env python3
"""frames.bin 을 브라우저에서 재생하는 preview.html 을 만든다.
   보드가 그릴 픽셀과 완전히 같은 데이터라, 여기서 본 게 곧 보드 화면이다."""
import base64, sys, os
W, H, FB = 128, 64, 1024
src = sys.argv[1] if len(sys.argv) > 1 else "frames.bin"
out = sys.argv[2] if len(sys.argv) > 2 else "preview.html"
data = open(src, "rb").read()
n = len(data) // FB
# 압축하지 않는다: file:// 로 열어도 DecompressionStream 같은 API에 기대지 않게
b64 = base64.b64encode(data).decode()

HTML = """<!doctype html><meta charset="utf-8"><title>NU40DK 3D — 미리보기</title>
<style>
 :root{color-scheme:dark}
 body{margin:0;background:#0b0d10;color:#cfd6e4;font:14px/1.5 -apple-system,BlinkMacSystemFont,"Apple SD Gothic Neo",sans-serif;
      display:flex;flex-direction:column;align-items:center;gap:14px;padding:26px}
 h1{font-size:15px;font-weight:600;letter-spacing:.02em;margin:0;color:#7fd4ff}
 .screen{padding:14px;background:#05070a;border:1px solid #1e2530;border-radius:10px;
         box-shadow:0 0 60px rgba(90,180,255,.10)}
 canvas{image-rendering:pixelated;display:block;width:768px;height:384px}
 .row{display:flex;align-items:center;gap:12px}
 button{background:#161d27;color:#cfd6e4;border:1px solid #2a3442;border-radius:7px;
        padding:7px 15px;font:inherit;cursor:pointer}
 button:hover{background:#1e2733}
 input[type=range]{width:520px}
 .meta{color:#6d7a8c;font-size:12px;font-variant-numeric:tabular-nums}
</style>
<h1>NU40DK 단독 3D 엔진 — 실제 OLED 픽셀 (128 x 64, 1비트)</h1>
<div class="screen"><canvas id="c" width="128" height="64"></canvas></div>
<div class="row">
  <button id="pp">일시정지</button>
  <input type="range" id="sl" min="0" value="0">
  <span class="meta" id="lbl"></span>
</div>
<div class="row meta" id="info"></div>
<script>
const RAW="__DATA__", NF=__N__, W=128, H=64, FB=1024;
const frames=Uint8Array.from(atob(RAW),c=>c.charCodeAt(0));
const cv=document.getElementById("c"), cx=cv.getContext("2d");
const img=cx.createImageData(W,H);
let i=0, playing=true;
document.getElementById("sl").max=NF-1;
document.getElementById("info").textContent =
  NF+"프레임 · 30fps 재생 · 프레임 1장 = "+FB+"바이트 · 탭이 뒤에 있으면 브라우저가 재생을 늦춘다";
requestAnimationFrame(tick);
function draw(f){
  const off=f*FB, d=img.data;
  for(let y=0;y<H;y++)for(let x=0;x<W;x++){
    const on=(frames[off+(y>>3)*W+x]>>(y&7))&1, p=(y*W+x)*4;
    d[p]=on?232:9; d[p+1]=on?243:11; d[p+2]=on?255:15; d[p+3]=255;
  }
  cx.putImageData(img,0,0);
  document.getElementById("lbl").textContent=(f+1)+" / "+NF;
  document.getElementById("sl").value=f;
}
let last=0;
function tick(t){
  if(playing && t-last>33){ i=(i+1)%NF; draw(i); last=t; }
  requestAnimationFrame(tick);
}
document.getElementById("pp").onclick=e=>{
  playing=!playing; e.target.textContent=playing?"일시정지":"재생";
};
document.getElementById("sl").oninput=e=>{
  playing=false; document.getElementById("pp").textContent="재생";
  i=+e.target.value; draw(i);
};
</script>"""
open(out, "w").write(HTML.replace("__DATA__", b64).replace("__N__", str(n)))
print(f"{out}  ({n}프레임, {len(b64)//1024}KB)")
