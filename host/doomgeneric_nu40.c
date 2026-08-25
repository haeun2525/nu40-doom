/*
  A안 — 진짜 Doom 1을 맥에서 돌리고, 그 화면을 NU40DK 의 OLED 로 흘려보낸다.
  보드는 화면과 컨트롤러 역할만 한다. 게임 연산은 전부 이 프로그램(맥)이 한다.
  (보드가 혼자 3D를 돌리는 쪽은 firmware/nu40_doom 이다. 헷갈리지 말 것.)

  하는 일:
    Doom 프레임 320x200  ->  상태바를 잘라낸 320x168  ->  128x64 축소
      ->  프레임마다 자동 대비 보정  ->  1비트 임계값  ->  시리얼로 전송
    보드가 보내주는 입력 비트를 Doom 키 코드로 바꿔서 엔진에 넣는다.

  프로토콜
    PC -> 보드 : 0xA5 0x5A, 체력 1바이트, 프레임 1024바이트
    보드 -> PC : 0x49('I'), 입력비트 하위, 입력비트 상위
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/time.h>
#include <glob.h>
#include "doomgeneric.h"
#include "doomkeys.h"

// 화면 전체(200줄)를 그대로 보낸다. 3D 화면만 잘라내면(168줄) 비율은 맞지만
// Doom 의 상태바가 사라진다 — 그런데 남들이 "둠이다" 라고 알아보는 건 그 상태바다.
// 세로가 눌려 보이는 대가를 치르더라도 상태바를 남기는 쪽이 낫다.
#define VIEW_H   DOOMGENERIC_RESY
#define OUT_W    128
#define OUT_H    64
#define FB_BYTES (OUT_W * OUT_H / 8)

static int ser = -1;

// ── 보드 입력 비트 (펌웨어와 순서를 맞춰야 한다)
enum {
  IN_FORWARD = 1 << 0, IN_BACK    = 1 << 1, IN_TURNL = 1 << 2, IN_TURNR = 1 << 3,
  IN_STRAFEL = 1 << 4, IN_STRAFER = 1 << 5, IN_FIRE  = 1 << 6, IN_USE   = 1 << 7,
  IN_RUN     = 1 << 8, IN_ESC     = 1 << 9, IN_ENTER = 1 << 10,
};
static const struct { int bit; unsigned char key; } KEYMAP[] = {
  { IN_FORWARD, KEY_UPARROW },   { IN_BACK,    KEY_DOWNARROW },
  { IN_TURNL,   KEY_LEFTARROW }, { IN_TURNR,   KEY_RIGHTARROW },
  { IN_STRAFEL, KEY_STRAFE_L },  { IN_STRAFER, KEY_STRAFE_R },
  { IN_FIRE,    KEY_FIRE },      { IN_USE,     KEY_USE },
  { IN_RUN,     KEY_RSHIFT },    { IN_ESC,     KEY_ESCAPE },
  { IN_ENTER,   KEY_ENTER },
};
#define NKEYS ((int)(sizeof(KEYMAP)/sizeof(KEYMAP[0])))

// 키 이벤트 큐 (DG_GetKey 는 한 번에 하나씩만 돌려준다)
static struct { int pressed; unsigned char key; } kq[64];
static int kq_head = 0, kq_tail = 0;
static int prev_mask = 0;
static int board_ready = 1;      // 보드가 'K' 를 보내면 1. 첫 프레임은 그냥 보낸다.

static void kq_push(int pressed, unsigned char key) {
  int n = (kq_tail + 1) % 64;
  if (n == kq_head) return;      // 큐가 꽉 차면 버린다
  kq[kq_tail].pressed = pressed;
  kq[kq_tail].key = key;
  kq_tail = n;
}

static int open_serial(const char* want) {
  char path[512];
  if (want) {
    snprintf(path, sizeof(path), "%s", want);
  } else {
    glob_t g;
    if (glob("/dev/cu.usbmodem*", 0, NULL, &g) != 0 || g.gl_pathc == 0) {
      fprintf(stderr, "보드를 찾지 못했습니다. /dev/cu.usbmodem* 가 없습니다.\n");
      return -1;
    }
    snprintf(path, sizeof(path), "%s", g.gl_pathv[0]);
    globfree(&g);
  }
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) { perror(path); return -1; }
  struct termios t;
  tcgetattr(fd, &t);
  cfmakeraw(&t);
  // USB CDC 라서 이 속도값은 실제 전송률과 무관하다. 병목은 보드의 I2C 화면이다.
  cfsetispeed(&t, B115200);
  cfsetospeed(&t, B115200);
  t.c_cflag |= CLOCAL | CREAD;
  t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
  tcsetattr(fd, TCSANOW, &t);
  fprintf(stderr, "보드 연결: %s\n", path);
  return fd;
}

void DG_Init(void) {
  ser = open_serial(getenv("NU40_PORT"));
  if (ser < 0) exit(1);
  usleep(1500 * 1000);          // 보드가 리셋에서 깨어날 시간
}

// ── health_shim.c 가 Doom 내부에서 꺼내주는 것들 (여기서 한 번에 선언한다)
extern int  g_player_health(void);
extern int  g_menu_active(void);
extern int  g_menu_open(void);
extern int  g_in_level(void);
extern void g_start_title(void);
extern void g_start_e1m1(void);
extern void g_tick_give(void);

// ── 320x168 을 128x64 로 줄이고, 자동 대비 보정 후 1비트로 찍는다
static const uint8_t BAYER[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};

static void frame_to_fb(uint8_t* fb) {
  static uint8_t gray[OUT_W * OUT_H];
  static uint8_t hist[256];
  memset(hist, 0, sizeof(hist));

  for (int oy = 0; oy < OUT_H; oy++) {
    int y0 = oy * VIEW_H / OUT_H, y1 = (oy + 1) * VIEW_H / OUT_H;
    if (y1 <= y0) y1 = y0 + 1;
    for (int ox = 0; ox < OUT_W; ox++) {
      int x0 = ox * DOOMGENERIC_RESX / OUT_W, x1 = (ox + 1) * DOOMGENERIC_RESX / OUT_W;
      if (x1 <= x0) x1 = x0 + 1;
      unsigned sum = 0, n = 0;
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
          uint32_t p = DG_ScreenBuffer[y * DOOMGENERIC_RESX + x];
          int b = p & 0xff, g = (p >> 8) & 0xff, r = (p >> 16) & 0xff;
          sum += (r * 77 + g * 151 + b * 28) >> 8;
          n++;
        }
      uint8_t v = (uint8_t)(sum / n);
      gray[oy * OUT_W + ox] = v;
      hist[v]++;
    }
  }

  // 자동 대비: 아래위 3% 를 버리고 남은 구간을 0..255 로 편다.
  // 이게 없으면 Doom 의 어두운 방이 통째로 새까맣게 나온다.
  int total = OUT_W * OUT_H, cut = total * 3 / 100, acc = 0, lo = 0, hi = 255;
  for (int v = 0; v < 256; v++)   { acc += hist[v]; if (acc > cut)         { lo = v; break; } }
  acc = 0;
  for (int v = 255; v >= 0; v--)  { acc += hist[v]; if (acc > cut)         { hi = v; break; } }
  if (hi - lo < 12) { lo = 0; hi = 255; }

  // 대비를 편 결과
  static uint8_t lv[OUT_W * OUT_H];
  for (int i = 0; i < OUT_W * OUT_H; i++) {
    int v = (gray[i] - lo) * 255 / (hi - lo);
    lv[i] = v < 0 ? 0 : (v > 255 ? 255 : v);
  }

  // 언샤프 마스크. 320x200 을 128x64 로 줄이면 픽셀 2.5개를 평균 내는 셈이라
  // 경계가 흐려지고, 거기에 흑백 이분법을 걸면 덩어리가 된다. 경계를 먼저 되살린다.
  {
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
        int c = lv[y * OUT_W + x];
        int v = c + (c - sum / n);
        sh[y * OUT_W + x] = v < 0 ? 0 : (v > 255 ? 255 : v);
      }
    memcpy(lv, sh, sizeof(lv));
  }

  // 타이틀·메뉴 화면은 "국소 적응 임계값" 으로 바꾼다.
  // DOOM 로고는 글자 속이 어둡고 가장자리만 밝은 입체 글자라, 한 가지 밝기로 자르면
  // 얇은 테두리만 조각조각 남아 글자가 무너진다. 주변 평균과 비교하면 글자가 살아난다.
  // 반대로 게임 중에는 이 방식을 쓰면 전부 윤곽선이 되어 깊이감이 사라진다.
  int adaptive = !g_in_level();

  memset(fb, 0, FB_BYTES);
  for (int y = 0; y < OUT_H; y++)
    for (int x = 0; x < OUT_W; x++) {
      int v = lv[y * OUT_W + x];
      if (adaptive) {
        int sum = 0, n = 0;
        for (int dy = -4; dy <= 4; dy++)
          for (int dx = -4; dx <= 4; dx++) {
            int yy = y + dy, xx = x + dx;
            if (yy < 0 || yy >= OUT_H || xx < 0 || xx >= OUT_W) continue;
            sum += lv[yy * OUT_W + xx]; n++;
          }
        if (v > sum / n + 4) fb[(y >> 3) * OUT_W + x] |= 1u << (y & 7);
        continue;
      }
      // 임계값이 디더보다 잘 읽힌다. 굳이 디더를 쓰고 싶으면 아랫줄로 바꾼다.
      //   int on = v > BAYER[y & 3][x & 3] * 16 + 8;
      int on = v > 128;
      (void)BAYER;
      if (on) fb[(y >> 3) * OUT_W + x] |= 1u << (y & 7);
    }
}

// 체력을 읽어서 보드 LED 게이지로 보낸다 (Doom 내부 변수를 그대로 가져온다)

// 보드 -> PC 바이트를 훑어서 입력 패킷과 'K'(그리기 완료)를 처리한다
static void drain_input(void) {
  uint8_t buf[256];
  int n = read(ser, buf, sizeof(buf));
  for (int i = 0; i < n; i++) {
    if (buf[i] == 0x4B) { board_ready = 1; continue; }
    if (buf[i] != 0x49 || i + 2 >= n) continue;
    int mask = buf[i+1] | (buf[i+2] << 8);
    int diff = mask ^ prev_mask;
    // 메뉴·타이틀 화면에서는 버튼 뜻을 바꾼다.
    // 조합키(2+4)는 방향키가 같이 나가서 커서가 움직여버린다. 발사 버튼 하나로 선택하게 한다.
    int menu = g_menu_active();

    // ── 조합키는 1+3 하나뿐이다. 누르고 있던 시간으로 세 갈래로 나눈다.
    //    뗄 때 판정하므로, 길게 누르는 도중에 짧은 동작이 먼저 나가지 않는다.
    //      0.5초 미만 : 메뉴 열기/닫기 (ESC)
    //      0.5~2.5초  : 타이틀 화면으로 리셋
    //      2.5초 이상 : 어려운 모드로 바로 전투 시작 (무기 전부 지급)
    {
      static uint32_t esc_since = 0;
      if (mask & IN_ESC) {
        if (!esc_since) esc_since = DG_GetTicksMs();
      } else if (esc_since) {
        uint32_t held = DG_GetTicksMs() - esc_since;
        esc_since = 0;
        if (held < 500) {
          kq_push(1, KEY_ESCAPE);
          kq_push(0, KEY_ESCAPE);
        } else if (held < 2500) {
          fprintf(stderr, "리셋: 타이틀 화면으로\n"); fflush(stderr);
          g_start_title();
        } else {
          fprintf(stderr, "바로 시작: E1M1 어려운 모드\n"); fflush(stderr);
          g_start_e1m1();
        }
      }
    }

    for (int k = 0; k < NKEYS; k++) {
      if (!(diff & KEYMAP[k].bit)) continue;
      if (KEYMAP[k].bit == IN_ESC) continue;    // 위에서 따로 처리했다
      unsigned char key = KEYMAP[k].key;
      if (menu) {
        // 타이틀·데모 화면에서는 메뉴가 아직 안 열렸다. 그때 4번은 ESC(메뉴 열기),
        // 메뉴가 열린 뒤에는 ENTER(선택). 같은 버튼으로 끝까지 진행할 수 있게 한다.
        if (KEYMAP[k].bit == IN_FIRE) key = g_menu_open() ? KEY_ENTER : KEY_ESCAPE;
        else if (KEYMAP[k].bit == IN_USE) continue;         // 전진에 딸린 문열기는 메뉴에서 무시
        else if (KEYMAP[k].bit == IN_TURNL) key = KEY_UPARROW;    // 1번 = 위
        else if (KEYMAP[k].bit == IN_TURNR) key = KEY_DOWNARROW;  // 3번 = 아래
      }
      kq_push((mask & KEYMAP[k].bit) != 0, key);
    }
    prev_mask = mask;
    i += 2;
  }
}

void DG_DrawFrame(void) {
  static uint8_t fb[FB_BYTES];
  g_tick_give();
  // 초당 실제로 보드에 그려진 프레임 수를 stderr 로 알린다 (실측용)
  static uint32_t fps_t0 = 0, fps_n = 0, timeouts = 0;
  {
    uint32_t now = DG_GetTicksMs();
    if (fps_t0 == 0) fps_t0 = now;
    if (now - fps_t0 >= 1000) {
      fprintf(stderr, "FPS %u  (보드 응답 없음 %u회)\n", fps_n, timeouts);
      fflush(stderr);
      fps_n = 0; timeouts = 0; fps_t0 = now;
    }
    fps_n++;
  }
  frame_to_fb(fb);

  uint8_t hdr[3] = { 0xA5, 0x5A, 100 };
  int hp = g_player_health();
  hdr[2] = (uint8_t)(hp < 0 ? 0 : (hp > 200 ? 200 : hp));
  write(ser, hdr, 3);
  int off = 0;
  while (off < FB_BYTES) {
    int w = write(ser, fb + off, FB_BYTES - off);
    if (w > 0) off += w;
    else if (w < 0) { usleep(200); }
  }

  board_ready = 0;
  // 보드가 다 그릴 때까지 기다리면서 그 사이 올라오는 입력을 처리한다.
  // 최대 300ms 만 기다린다 (보드를 뽑아도 게임이 멈추지 않게).
  uint32_t t0 = DG_GetTicksMs();
  while (!board_ready && DG_GetTicksMs() - t0 < 300) {
    drain_input();
    if (!board_ready) usleep(300);
  }
  if (!board_ready) timeouts++;
}

void DG_SleepMs(uint32_t ms) { usleep(ms * 1000); }

uint32_t DG_GetTicksMs(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

int DG_GetKey(int* pressed, unsigned char* key) {
  if (kq_head == kq_tail) return 0;
  *pressed = kq[kq_head].pressed;
  *key     = kq[kq_head].key;
  kq_head  = (kq_head + 1) % 64;
  return 1;
}

void DG_SetWindowTitle(const char* title) { (void)title; }

int main(int argc, char** argv) {
  doomgeneric_Create(argc, argv);
  for (;;) doomgeneric_Tick();
  return 0;
}
