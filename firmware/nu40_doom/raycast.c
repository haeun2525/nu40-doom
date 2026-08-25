// NU40DK 단독 3D 엔진 — 레이캐스팅 렌더러.
// 정수/부동소수 연산만 쓰고 하드웨어를 전혀 건드리지 않는다.
// nRF52840 은 FPU(fpv4-sp-d16)가 있어서 float 로 짜도 64MHz 에서 충분하다.
#include "raycast.h"
#include "assets.h"
#include <math.h>
#include <string.h>

// 화면을 둘로 나눈다: 위는 3D 뷰, 아래 12줄은 상태바.
// 둠 원본도 화면의 16%를 상태바가 차지한다 (32/200). 12/64 = 19% 로 거의 같은 비율.
#define BAR_H     12
#define VIEW_H    (RC_H - BAR_H)
#define HORIZON   (VIEW_H / 2)
#define TALL_MUL  1.75f     // 기둥(TALL_TILE)은 이만큼 높다
// 투영 거리: 화면 절반폭 / 카메라 평면 길이. 벽 높이는 화면 높이가 아니라 이 값으로 나눈다.
// (여기를 RC_H 로 잘못 쓰면 가로로 넓은 화면에서 벽이 납작해진다)
#define RC_PROJ   ((RC_W / 2) / 0.66f)
#define MAX_DIST  13.0f     // 이보다 먼 건 거의 까맣게
#define MOVE_SPD  0.0030f   // 타일/ms
#define RUN_MUL   1.8f
#define TURN_SPD  0.0032f   // 라디안/ms

// ── 프레임버퍼: SSD1306 페이지 포맷 (한 바이트 = 세로 8픽셀, 최하위 비트가 위)
static inline void px_on(uint8_t* fb, int x, int y) {
  if ((unsigned)x < RC_W && (unsigned)y < RC_H) fb[(y >> 3) * RC_W + x] |= 1u << (y & 7);
}
static inline void px_off(uint8_t* fb, int x, int y) {
  if ((unsigned)x < RC_W && (unsigned)y < RC_H) fb[(y >> 3) * RC_W + x] &= ~(1u << (y & 7));
}

// ── 오더드 디더: 밝기 0..255 를 1비트로. 시간적으로 안정적이라 깜빡임이 없다.
static const uint8_t BAYER[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
static inline int dith(int v, int x, int y) {
  return v > (int)BAYER[y & 3][x & 3] * 16 + 8;
}
static inline void shade_px(uint8_t* fb, int x, int y, int v) {
  if (dith(v, x, y)) px_on(fb, x, y); else px_off(fb, x, y);
}

// ── 2비트 자원 읽기
static inline int tex_at(int tile, int tx, int ty) {
  int i = (tile - 1) * (TEX_SIZE * TEX_SIZE) + ty * TEX_SIZE + tx;
  return (TEXTURES[i >> 2] >> ((i & 3) * 2)) & 3;
}
static inline int spr_at(const uint8_t* s, int w, int x, int y) {
  int i = y * w + x;
  return (s[i >> 2] >> ((i & 3) * 2)) & 3;
}

static inline int map_at(int x, int y) {
  if ((unsigned)x >= MAP_W || (unsigned)y >= MAP_H) return 1;
  return MAP[y * MAP_W + x];
}

// 거리에 따른 감쇠 (0..256). 가까울수록 밝다.
// RC_LIGHT 로 밝기 성향을 고른다. 1=어둡게(대비 위주) 2=중간 3=밝게(가독 위주)
#ifndef RC_LIGHT
#define RC_LIGHT 3
#endif
// 바닥 격자선은 항상 제곱 감쇠를 쓴다. 먼 바닥까지 밝히면 선이 뭉쳐 노이즈가 된다.
static inline int atten_floor(float d) {
  if (d >= MAX_DIST) return 0;
  float f = 1.0f - d / MAX_DIST;
  f *= f;
  int a = (int)(f * 300.0f);
  return a > 256 ? 256 : a;
}
static inline int atten(float d) {
  if (d >= MAX_DIST) return 0;
  float f = 1.0f - d / MAX_DIST;
#if RC_LIGHT == 1
  f *= f;                      // 가까운 쪽 대비를 살린다. 먼 벽은 거의 검다
  int a = (int)(f * 300.0f);
#elif RC_LIGHT == 2
  f *= (0.45f + 0.55f * f);    // 제곱과 직선의 중간. 먼 벽도 형체는 남는다
  int a = (int)(f * 330.0f);
#else
  int a = (int)(f * 340.0f);   // 직선 감쇠. 안쪽까지 환하다
#endif
  return a > 256 ? 256 : a;
}

// ── 문 상태 (0=닫힘 .. 255=열림)
static uint8_t door_open[MAP_W * MAP_H];

// ── 적
typedef struct { float x, y; int alive; int hurt; } enemy_t;
static enemy_t enemies[NUM_ENEMIES];

void rc_init(rc_state_t* s) {
  memset(s, 0, sizeof(*s));
  s->px = START_X; s->py = START_Y;
  s->dirx = 1.0f;  s->diry = 0.0f;
  s->planex = 0.0f; s->planey = 0.66f;   // 시야각 약 66도
  s->health = 100; s->ammo = 50;
  memset(door_open, 0, sizeof(door_open));
  for (int i = 0; i < NUM_ENEMIES; i++) {
    enemies[i].x = ENEMY_POS[i][0];
    enemies[i].y = ENEMY_POS[i][1];
    enemies[i].alive = 1;
    enemies[i].hurt = 0;
  }
}

static int blocked(float x, float y) {
  int t = map_at((int)x, (int)y);
  if (t == 0) return 0;
  if (t == DOOR_TILE) return door_open[(int)y * MAP_W + (int)x] < 200;
  return 1;
}

void rc_update(rc_state_t* s, const rc_input_t* in, uint32_t dt_ms) {
  s->tick += dt_ms;
  float dt = (float)dt_ms;

  // 회전
  float rot = 0.0f;
  if (in->turn_left)  rot += TURN_SPD * dt;
  if (in->turn_right) rot -= TURN_SPD * dt;
  if (rot != 0.0f) {
    float c = cosf(rot), sn = sinf(rot);
    float ox = s->dirx, opx = s->planex;
    s->dirx   = ox * c - s->diry * sn;
    s->diry   = ox * sn + s->diry * c;
    s->planex = opx * c - s->planey * sn;
    s->planey = opx * sn + s->planey * c;
  }

  // 이동 (축별로 따로 밀어서 벽에 붙어도 미끄러지게)
  float spd = MOVE_SPD * dt * (in->run ? RUN_MUL : 1.0f);
  float mx = 0.0f, my = 0.0f;
  if (in->forward)      { mx += s->dirx * spd;  my += s->diry * spd; }
  if (in->back)         { mx -= s->dirx * spd;  my -= s->diry * spd; }
  if (in->strafe_right) { mx += s->planex * spd; my += s->planey * spd; }
  if (in->strafe_left)  { mx -= s->planex * spd; my -= s->planey * spd; }
  if (!blocked(s->px + mx * 1.6f, s->py)) s->px += mx;
  if (!blocked(s->px, s->py + my * 1.6f)) s->py += my;

  // 문: 바라보는 앞 칸이 문이면 연다
  if (in->use) {
    int dx = (int)(s->px + s->dirx * 0.9f), dy = (int)(s->py + s->diry * 0.9f);
    if (map_at(dx, dy) == DOOR_TILE) door_open[dy * MAP_W + dx] = 255;
  }

  // 적: 천천히 다가오고, 붙으면 아프다
  for (int i = 0; i < NUM_ENEMIES; i++) {
    enemy_t* e = &enemies[i];
    if (!e->alive) continue;
    if (e->hurt > 0) e->hurt--;
    float ex = s->px - e->x, ey = s->py - e->y;
    float d = sqrtf(ex * ex + ey * ey);
    if (d > 0.9f && d < 9.0f) {
      float step = 0.0009f * dt;
      float nx = e->x + ex / d * step, ny = e->y + ey / d * step;
      if (!blocked(nx, e->y)) e->x = nx;
      if (!blocked(e->x, ny)) e->y = ny;
    } else if (d <= 0.9f) {
      if ((s->tick / 700) != ((s->tick - dt_ms) / 700)) {
        s->health -= 6;
        if (s->health < 0) s->health = 0;
      }
    }
  }

  // 발사: 화면 중앙 좁은 각도 안에 있는 가장 가까운 적을 맞힌다
  s->muzzle = 0;
  if (s->fire_anim > 0) s->fire_anim--;
  if (in->fire && s->fire_anim == 0 && s->ammo > 0) {
    s->fire_anim = 6;
    s->muzzle = 1;
    s->ammo--;
    int best = -1; float bestd = 1e9f;
    for (int i = 0; i < NUM_ENEMIES; i++) {
      enemy_t* e = &enemies[i];
      if (!e->alive) continue;
      float ex = e->x - s->px, ey = e->y - s->py;
      float d = sqrtf(ex * ex + ey * ey);
      if (d < 0.01f || d > MAX_DIST) continue;
      float dot = (ex * s->dirx + ey * s->diry) / d;      // 정면이면 1
      if (dot < 0.985f) continue;                          // 약 10도 안
      if (d < bestd) { bestd = d; best = i; }
    }
    if (best >= 0) { enemies[best].alive = 0; enemies[best].hurt = 0; }
  }
  if (s->health <= 0) rc_init(s);   // 죽으면 처음부터 (촬영용)
}

// ── 3x5 숫자 글꼴 (열당 5비트). HUD 전용이라 이거면 충분하다.
static const uint8_t DIGIT[10][3] = {
  {0x1f,0x11,0x1f},{0x00,0x1f,0x00},{0x1d,0x15,0x17},{0x15,0x15,0x1f},{0x07,0x04,0x1f},
  {0x17,0x15,0x1d},{0x1f,0x15,0x1d},{0x01,0x01,0x1f},{0x1f,0x15,0x1f},{0x17,0x15,0x1f},
};
static void draw_num(uint8_t* fb, int x, int y, int v, int digits) {
  for (int d = digits - 1; d >= 0; d--) {
    int n = v; for (int k = 0; k < d; k++) n /= 10;
    n %= 10;
    for (int c = 0; c < 3; c++)
      for (int r = 0; r < 5; r++)
        if (DIGIT[n][c] >> r & 1) px_on(fb, x + c, y + r); else px_off(fb, x + c, y + r);
    for (int r = 0; r < 5; r++) px_off(fb, x + 3, y + r);   // 자간
    x += 4;
  }
}

// 상태바용 2배 크기 숫자. 작은 숫자로는 "게임 HUD" 로 안 읽힌다.
static void draw_num2x(uint8_t* fb, int x, int y, int v, int digits) {
  for (int d = digits - 1; d >= 0; d--) {
    int n = v; for (int k = 0; k < d; k++) n /= 10;
    n %= 10;
    for (int c = 0; c < 3; c++)
      for (int r = 0; r < 5; r++) {
        int on = DIGIT[n][c] >> r & 1;
        for (int oy = 0; oy < 2; oy++)
          for (int ox = 0; ox < 2; ox++) {
            if (on) px_on(fb, x + c * 2 + ox, y + r * 2 + oy);
            else    px_off(fb, x + c * 2 + ox, y + r * 2 + oy);
          }
      }
    x += 7;
  }
}

// 하단 상태바. 둠을 둠으로 보이게 하는 가장 큰 요소가 이거다.
// 왼쪽 체력 / 가운데 얼굴 / 오른쪽 탄약.
static void draw_statusbar(const rc_state_t* s, uint8_t* fb) {
  const int y0 = VIEW_H;
  for (int y = y0; y < RC_H; y++)
    for (int x = 0; x < RC_W; x++) px_off(fb, x, y);
  for (int x = 0; x < RC_W; x++) px_on(fb, x, y0);        // 바 윗선

  // 얼굴 상자
  const int fx = (RC_W - FACE_W) / 2, fy = y0 + 1;
  for (int y = fy - 1; y < RC_H; y++) { px_on(fb, fx - 3, y); px_on(fb, fx + FACE_W + 2, y); }

  // 얼굴: 체력으로 고르고, 쏘는 순간에는 인상을 쓴다 (둠도 그렇다)
  const uint8_t* face = SPR_FACE_OK;
  if (s->health < 34)      face = SPR_FACE_BAD;
  else if (s->health < 70) face = SPR_FACE_HURT;
  if (s->fire_anim > 2 && s->health >= 34) face = SPR_FACE_HURT;
  for (int y = 0; y < FACE_H; y++)
    for (int x = 0; x < FACE_W; x++)
      if (spr_at(face, FACE_W, x, y) >= 2) px_on(fb, fx + x, fy + y);

  int hp = s->health < 0 ? 0 : s->health;
  draw_num2x(fb, 6, y0 + 2, hp, 3);
  draw_num2x(fb, RC_W - 24, y0 + 2, s->ammo < 0 ? 0 : s->ammo, 3);
}

void rc_render(const rc_state_t* s, uint8_t* fb) {
  static float zbuf[RC_W];
  memset(fb, 0, RC_FB_BYTES);

  // ── 1) 바닥과 천장: 실제 좌표로 역투영해 "타일 격자선"만 그린다.
  //     디더로 면을 칠하면 1비트에서는 노이즈로만 보인다. 선만 그리는 쪽이 훨씬 읽힌다.
  //     천장을 그려야 "방 안"으로 보인다. 위가 새까맣면 야외처럼 보여서 둠과 멀어진다.
  for (int y = 0; y < VIEW_H; y++) {
    if (y == HORIZON) continue;
    int below = y > HORIZON;
    float rowd = (0.5f * RC_PROJ) / (float)(below ? y - HORIZON : HORIZON - y);
    if (rowd > MAX_DIST) continue;
    float rdx0 = s->dirx - s->planex, rdy0 = s->diry - s->planey;
    float rdx1 = s->dirx + s->planex, rdy1 = s->diry + s->planey;
    float fx = s->px + rowd * rdx0, fy = s->py + rowd * rdy0;
    float sx = rowd * (rdx1 - rdx0) / RC_W, sy = rowd * (rdy1 - rdy0) / RC_W;
    int a = atten_floor(rowd);
    // 천장은 훨씬 어둡게. 바닥과 같은 세기로 그리면 위아래 격자가 서로 싸워서
    // 화면이 낙서처럼 된다. 천장은 "방 안이구나" 정도만 알려주면 된다.
    if (!below) a = a * 2 / 5;
    float lw = 0.014f * rowd;                          // 화면상 선 굵기를 일정하게
    if (lw < 0.02f) lw = 0.02f;
    if (lw > (below ? 0.30f : 0.12f)) lw = below ? 0.30f : 0.12f;
    for (int x = 0; x < RC_W; x++) {
      float gx = fx - floorf(fx), gy = fy - floorf(fy);
      int on_line = (gx < lw || gx > 1.0f - lw || gy < lw || gy > 1.0f - lw);
      shade_px(fb, x, y, on_line ? (220 * a >> 8) : 0);
      fx += sx; fy += sy;
    }
  }

  // ── 2) 벽: 열마다 DDA
  for (int x = 0; x < RC_W; x++) {
    float camx = 2.0f * x / (float)RC_W - 1.0f;
    float rdx = s->dirx + s->planex * camx;
    float rdy = s->diry + s->planey * camx;
    int mapx = (int)s->px, mapy = (int)s->py;
    float ddx = (rdx == 0.0f) ? 1e30f : fabsf(1.0f / rdx);
    float ddy = (rdy == 0.0f) ? 1e30f : fabsf(1.0f / rdy);
    int stepx, stepy, side = 0, hit = 0, tile = 1;
    float sdx, sdy;
    if (rdx < 0) { stepx = -1; sdx = (s->px - mapx) * ddx; }
    else         { stepx =  1; sdx = (mapx + 1.0f - s->px) * ddx; }
    if (rdy < 0) { stepy = -1; sdy = (s->py - mapy) * ddy; }
    else         { stepy =  1; sdy = (mapy + 1.0f - s->py) * ddy; }

    for (int guard = 0; guard < 64 && !hit; guard++) {
      if (sdx < sdy) { sdx += ddx; mapx += stepx; side = 0; }
      else           { sdy += ddy; mapy += stepy; side = 1; }
      int t = map_at(mapx, mapy);
      if (t == DOOR_TILE && door_open[mapy * MAP_W + mapx] >= 200) continue;
      if (t > 0) { hit = 1; tile = t; }
    }
    float pdist = side == 0 ? (sdx - ddx) : (sdy - ddy);
    if (pdist < 0.02f) pdist = 0.02f;
    zbuf[x] = pdist;
    if (!hit || pdist >= MAX_DIST) continue;

    int lh = (int)(RC_PROJ / pdist);
    int y1 = HORIZON + lh / 2;                       // 바닥에 닿는 지점은 높이와 무관
    int wh = (tile == TALL_TILE) ? (int)(lh * TALL_MUL) : lh;
    int y0 = y1 - wh;                                // 높은 타일은 위로 더 솟는다
    int dy0 = y0 < 0 ? 0 : y0, dy1 = y1 >= VIEW_H ? VIEW_H - 1 : y1;

    float wallx = side == 0 ? s->py + pdist * rdy : s->px + pdist * rdx;
    wallx -= floorf(wallx);
    int tx = (int)(wallx * TEX_SIZE);
    if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0)) tx = TEX_SIZE - 1 - tx;

    int a = atten(pdist);
    if (side == 1) a = a * 5 / 8;          // 옆면은 어둡게 → 모서리가 살아난다
    for (int y = dy0; y <= dy1; y++) {
      int ty = ((y - y0) * TEX_SIZE) / (wh ? wh : 1);
      if (ty > TEX_SIZE - 1) ty = TEX_SIZE - 1;
      shade_px(fb, x, y, tex_at(tile, tx, ty) * 85 * a >> 8);
    }
  }

  // ── 3) 적 스프라이트: 먼 것부터 그린다
  int order[NUM_ENEMIES]; float dist[NUM_ENEMIES]; int n = 0;
  for (int i = 0; i < NUM_ENEMIES; i++) {
    if (!enemies[i].alive) continue;
    float ex = enemies[i].x - s->px, ey = enemies[i].y - s->py;
    order[n] = i; dist[n] = ex * ex + ey * ey; n++;
  }
  for (int i = 1; i < n; i++)                       // 삽입 정렬 (최대 몇 마리라 충분)
    for (int j = i; j > 0 && dist[j] > dist[j-1]; j--) {
      float td = dist[j]; dist[j] = dist[j-1]; dist[j-1] = td;
      int ti = order[j]; order[j] = order[j-1]; order[j-1] = ti;
    }

  float inv = 1.0f / (s->planex * s->diry - s->dirx * s->planey);
  for (int k = 0; k < n; k++) {
    enemy_t* e = &enemies[order[k]];
    float ex = e->x - s->px, ey = e->y - s->py;
    float tx = inv * (s->diry * ex - s->dirx * ey);
    float ty = inv * (-s->planey * ex + s->planex * ey);   // 화면 깊이
    if (ty < 0.15f) continue;
    int scr = (int)((RC_W / 2) * (1.0f + tx / ty));
    int sh = (int)(RC_PROJ / ty);                           // 스프라이트 높이
    int sw = sh * IMP_W / IMP_H;
    int y0 = HORIZON + (int)(RC_PROJ / (2.0f * ty)) - sh;   // 발이 바닥에 닿게
    int x0 = scr - sw / 2;
    int a = atten(ty);
    if (a == 0) continue;
    for (int x = x0; x < x0 + sw; x++) {
      if (x < 0 || x >= RC_W) continue;
      if (ty >= zbuf[x]) continue;                          // 벽 뒤면 가린다
      int sxp = (x - x0) * IMP_W / (sw ? sw : 1);
      for (int y = y0; y < y0 + sh; y++) {
        if (y < 0 || y >= VIEW_H) continue;
        int syp = (y - y0) * IMP_H / (sh ? sh : 1);
        int lvl = spr_at(SPR_IMP, IMP_W, sxp, syp);
        if (lvl == 0) {
          // 테두리 한 겹은 강제로 검게 지운다.
          // 안 그러면 밝은 벽 앞에서 적의 실루엣이 벽에 먹혀 사라진다.
          if (spr_at(SPR_IMP, IMP_W, sxp > 0 ? sxp-1 : 0, syp)
           || spr_at(SPR_IMP, IMP_W, sxp < IMP_W-1 ? sxp+1 : IMP_W-1, syp)
           || spr_at(SPR_IMP, IMP_W, sxp, syp > 0 ? syp-1 : 0)
           || spr_at(SPR_IMP, IMP_W, sxp, syp < IMP_H-1 ? syp+1 : IMP_H-1))
            px_off(fb, x, y);
          continue;                                         // 투명
        }
        shade_px(fb, x, y, lvl * 85 * a >> 8);
      }
    }
  }

  // ── 4) 무기: 하단 중앙 고정, 발사하면 살짝 내려간다
  {
    int kick = s->fire_anim > 3 ? 3 : s->fire_anim;
    int wx = (RC_W - PISTOL_W) / 2;
    int wy = VIEW_H - PISTOL_H + kick;
    for (int y = 0; y < PISTOL_H; y++)
      for (int x = 0; x < PISTOL_W; x++) {
        int lvl = spr_at(SPR_PISTOL, PISTOL_W, x, y);
        if (lvl == 0) continue;
        if (lvl == 3) px_on(fb, wx + x, wy + y);            // 외곽선은 항상 흰색
        else shade_px(fb, wx + x, wy + y, lvl * 85);
      }
    if (s->muzzle) {
      int fx = (RC_W - FLASH_W) / 2, fy = wy - FLASH_H + 4;
      for (int y = 0; y < FLASH_H; y++)
        for (int x = 0; x < FLASH_W; x++)
          if (spr_at(SPR_FLASH, FLASH_W, x, y)) px_on(fb, fx + x, fy + y);
    }
  }

  // ── 5) 하단 상태바 (체력 / 얼굴 / 탄약)
  draw_statusbar(s, fb);

  // ── 6) 좌상단 FPS. "보드가 직접 돌린다"의 증거라 화면에 남겨둔다.
  if (s->show_hud) {
    for (int y = 0; y < 7; y++) for (int x = 0; x < 9; x++) px_off(fb, x, y);
    draw_num(fb, 1, 1, s->fps, 2);
  }
}

// ── 데모 모드 ──────────────────────────────────────────────
// 맵을 한 바퀴 도는 경유지. 문 앞에서는 알아서 문을 열고,
// 정면에 적이 있으면 쏜다.
static const float WAYPT[][2] = {
  {6.0f, 3.5f}, {6.0f, 6.5f}, {10.0f, 6.5f}, {10.0f, 9.5f},
  {16.0f, 9.5f}, {19.5f, 9.5f}, {19.5f, 2.0f}, {10.5f, 2.0f},
  {9.0f, 6.5f}, {5.5f, 6.5f}, {4.5f, 10.3f}, {4.5f, 13.5f},
  {4.5f, 20.5f}, {10.0f, 20.5f}, {16.0f, 20.5f}, {19.5f, 20.0f},
  {19.5f, 13.0f}, {19.5f, 19.5f}, {10.0f, 21.5f}, {4.5f, 20.0f},
  {4.5f, 13.0f}, {4.5f, 10.3f}, {5.5f, 6.5f}, {6.0f, 3.5f},
};
#define NUM_WAYPT ((int)(sizeof(WAYPT) / sizeof(WAYPT[0])))
static int wp_idx = 0;

void rc_demo_input(const rc_state_t* s, rc_input_t* out) {
  memset(out, 0, sizeof(*out));
  float tx = WAYPT[wp_idx][0] - s->px, ty = WAYPT[wp_idx][1] - s->py;
  float d = sqrtf(tx * tx + ty * ty);
  if (d < 0.45f) { wp_idx = (wp_idx + 1) % NUM_WAYPT; return; }

  // 목표 방향과 현재 방향의 각도 차 (외적 부호로 좌/우 판단)
  float cross = s->dirx * ty - s->diry * tx;
  float dot   = (s->dirx * tx + s->diry * ty) / d;
  if (cross > 0.06f)       out->turn_left = 1;
  else if (cross < -0.06f) out->turn_right = 1;
  if (dot > 0.55f) out->forward = 1;

  // 앞 칸이 문이면 연다
  int dx = (int)(s->px + s->dirx * 1.1f), dy = (int)(s->py + s->diry * 1.1f);
  if (map_at(dx, dy) == DOOR_TILE) out->use = 1;

  // 정면에 살아있는 적이 있으면 쏜다
  for (int i = 0; i < NUM_ENEMIES; i++) {
    if (!enemies[i].alive) continue;
    float ex = enemies[i].x - s->px, ey = enemies[i].y - s->py;
    float ed = sqrtf(ex * ex + ey * ey);
    if (ed < 0.01f || ed > 9.0f) continue;
    if ((ex * s->dirx + ey * s->diry) / ed > 0.97f) { out->fire = 1; break; }
  }
}
