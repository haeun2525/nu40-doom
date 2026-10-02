// 진짜 Doom 을 돌려서, 보드로 보낼 화면 그대로를 파일에 기록한다.
// 보드 없이도 "실제로 어떻게 보이는지" 확인하기 위한 도구.
// 변환 로직은 doomgeneric_nu40.c 와 동일해야 의미가 있다.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "doomgeneric.h"
#include "doomkeys.h"

#define OUT_W    128
#define OUT_H    64
#define FB_BYTES (OUT_W * OUT_H / 8)
#define VIEW_H   168          // 하단 상태바 32픽셀을 잘라낸 실제 화면

static FILE*    out;
static FILE*    outfull;      // 상태바까지 포함한 전체 화면 (비교용)
static uint32_t s_ticks = 0;
static int      captured = 0;
static int      want = 1200;
static int      dither = 0;   // CAP_DITHER=1 이면 베이어 디더
// CAP_MODE 0=지금(전역 임계값) 1=선명화 후 임계값 2=국소 적응 임계값
static int      mode = 0;

static const uint8_t BAYER[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
static void shrink(uint8_t* fb, int src_h) {
  static uint8_t gray[OUT_W * OUT_H];
  static uint8_t hist[256];
  memset(hist, 0, sizeof(hist));
  for (int oy = 0; oy < OUT_H; oy++) {
    int y0 = oy * src_h / OUT_H, y1 = (oy + 1) * src_h / OUT_H;
    if (y1 <= y0) y1 = y0 + 1;
    for (int ox = 0; ox < OUT_W; ox++) {
      int x0 = ox * DOOMGENERIC_RESX / OUT_W, x1 = (ox + 1) * DOOMGENERIC_RESX / OUT_W;
      if (x1 <= x0) x1 = x0 + 1;
      unsigned sum = 0, n = 0, mn = 255;
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
          uint32_t p = DG_ScreenBuffer[y * DOOMGENERIC_RESX + x];
          int b = p & 0xff, g = (p >> 8) & 0xff, r = (p >> 16) & 0xff;
          unsigned l = (r * 77 + g * 151 + b * 28) >> 8;
          sum += l; n++;
          if (l < mn) mn = l;
        }
      unsigned avg = sum / n;
      // mode 3: 평균과 최솟값을 섞는다. 얇은 검은 선(글자 구멍·윤곽)이 살아남는다.
      uint8_t v = (mode == 3) ? (uint8_t)((avg + mn) / 2)
                : (mode == 4) ? (uint8_t)mn
                : (uint8_t)avg;
      gray[oy * OUT_W + ox] = v;
      hist[v]++;
    }
  }
  int total = OUT_W * OUT_H, cut = total * 3 / 100, acc = 0, lo = 0, hi = 255;
  for (int v = 0; v < 256; v++)  { acc += hist[v]; if (acc > cut) { lo = v; break; } }
  acc = 0;
  for (int v = 255; v >= 0; v--) { acc += hist[v]; if (acc > cut) { hi = v; break; } }
  if (hi - lo < 12) { lo = 0; hi = 255; }
  // 대비를 편 결과를 따로 담는다 (선명화·국소 임계값이 이걸 본다)
  static uint8_t lv[OUT_W * OUT_H];
  for (int i = 0; i < OUT_W * OUT_H; i++) {
    int v = (gray[i] - lo) * 255 / (hi - lo);
    lv[i] = v < 0 ? 0 : (v > 255 ? 255 : v);
  }

  // 1번 모드: 언샤프 마스크. 흐려진 경계를 되살려서 임계값이 덩어리를 덜 만들게 한다.
  if (mode == 1 || mode == 3 || mode == 4) {
    static uint8_t sh[OUT_W * OUT_H];
    for (int y = 0; y < OUT_H; y++)
      for (int x = 0; x < OUT_W; x++) {
        int sum = 0, n = 0;
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            int yy = y + dy, xx = x + dx;
            if (yy < 0 || yy >= OUT_H || xx < 0 || xx >= OUT_W) continue;
            sum += lv[yy * OUT_W + xx]; n++;
          }
        int blur = sum / n;
        int v = lv[y * OUT_W + x] + (lv[y * OUT_W + x] - blur);   // 세기 1.0
        sh[y * OUT_W + x] = v < 0 ? 0 : (v > 255 ? 255 : v);
      }
    memcpy(lv, sh, sizeof(lv));
  }

  memset(fb, 0, FB_BYTES);
  for (int y = 0; y < OUT_H; y++)
    for (int x = 0; x < OUT_W; x++) {
      int v = lv[y * OUT_W + x];
      int on;
      if (mode == 2) {
        // 국소 적응 임계값: 주변 9x9 평균보다 밝으면 켠다.
        // 어두운 방과 밝은 방을 같은 잣대로 자르지 않아서 디테일이 살아난다.
        int sum = 0, n = 0;
        for (int dy = -4; dy <= 4; dy++)
          for (int dx = -4; dx <= 4; dx++) {
            int yy = y + dy, xx = x + dx;
            if (yy < 0 || yy >= OUT_H || xx < 0 || xx >= OUT_W) continue;
            sum += lv[yy * OUT_W + xx]; n++;
          }
        on = v > sum / n + 4;
      } else {
        on = dither ? (v > BAYER[y & 3][x & 3] * 16 + 8) : (v > 128);
      }
      if (on) fb[(y >> 3) * OUT_W + x] |= 1u << (y & 7);
    }
}

void DG_Init(void) {
  const char* w = getenv("CAP_FRAMES"); if (w) want = atoi(w);
  const char* dd = getenv("CAP_DITHER"); if (dd) dither = atoi(dd);
  const char* mm = getenv("CAP_MODE"); if (mm) mode = atoi(mm);
  out     = fopen("cap_view.bin", "wb");
  outfull = fopen("cap_full.bin", "wb");
}

void DG_DrawFrame(void) {
  static uint8_t fb[FB_BYTES];
  // CAP_RAW=N : N번째 프레임의 320x200 원본을 raw.bin 으로 떠서 파이썬에서 실험한다
  { const char* r = getenv("CAP_RAW");
    if (r && captured == atoi(r)) {
      FILE* f = fopen("raw.bin", "wb");
      fwrite(DG_ScreenBuffer, 4, DOOMGENERIC_RESX * DOOMGENERIC_RESY, f);
      fclose(f);
    } }
  shrink(fb, VIEW_H);              // 3D 화면만 (보드에 보내는 것)
  fwrite(fb, 1, FB_BYTES, out);
  shrink(fb, DOOMGENERIC_RESY);    // 상태바까지 전부
  fwrite(fb, 1, FB_BYTES, outfull);
  if (++captured >= want) {
    fclose(out); fclose(outfull);
    fprintf(stderr, "%d frames recorded\n", captured);
    exit(0);
  }
}

// 가상 시계: 부를 때마다 조금씩 흐르게 해야 렌더 대기에 갇히지 않는다.
void DG_SleepMs(uint32_t ms) { s_ticks += ms; }
uint32_t DG_GetTicksMs(void) { s_ticks += 1; return s_ticks; }
int DG_GetKey(int* pressed, unsigned char* key) { (void)pressed; (void)key; return 0; }
void DG_SetWindowTitle(const char* t) { (void)t; }

int main(int argc, char** argv) {
  doomgeneric_Create(argc, argv);
  for (;;) doomgeneric_Tick();
  return 0;
}
