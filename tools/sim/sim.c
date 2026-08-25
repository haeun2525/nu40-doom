// PC 시뮬레이터: 보드와 똑같은 raycast.c 를 그대로 컴파일해서
// 정해진 조작 시나리오를 돌리고 프레임(1024B)을 frames.bin 으로 뽑는다.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../firmware/nu40_doom/raycast.h"

#define DT 33   // 약 30fps


// --look x,y,deg x,y,deg ... : 지정한 위치/각도에서 한 장씩 렌더링한다 (화면 검사용)
static int look_mode(int argc, char** argv) {
  rc_state_t st;
  static uint8_t fb[RC_FB_BYTES];
  FILE* f = fopen("look.bin", "wb");
  for (int i = 2; i < argc; i++) {
    float x, y, deg;
    if (sscanf(argv[i], "%f,%f,%f", &x, &y, &deg) != 3) continue;
    rc_init(&st);
    st.px = x; st.py = y;
    float a = deg * 3.14159265f / 180.0f;
    st.dirx = cosf(a);   st.diry = sinf(a);
    st.planex = -sinf(a) * 0.66f; st.planey = cosf(a) * 0.66f;
    rc_input_t in; memset(&in, 0, sizeof(in));
    rc_update(&st, &in, 1);
    rc_render(&st, fb);
    fwrite(fb, 1, RC_FB_BYTES, f);
    fprintf(stderr, "look %.1f,%.1f @%.0fdeg\n", x, y, deg);
  }
  fclose(f);
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "--look") == 0) return look_mode(argc, argv);
  const char* out = argc > 1 ? argv[1] : "frames.bin";
  int every = argc > 2 ? atoi(argv[2]) : 1;
  rc_state_t st; rc_init(&st);
  static uint8_t fb[RC_FB_BYTES];
  FILE* f = fopen(out, "wb");
  if (!f) { perror("fopen"); return 1; }
  int total = argc > 3 ? atoi(argv[3]) : 900;
  int saved = 0;
  st.show_hud = 1;
  st.fps = 34;                       // 시뮬레이터에서는 표시용 고정값
  for (int i = 0; i < total; i++) {
    rc_input_t in;
    rc_demo_input(&st, &in);
    rc_update(&st, &in, DT);
    rc_render(&st, fb);
    if (i % every == 0) { fwrite(fb, 1, RC_FB_BYTES, f); saved++; }
  }
  fclose(f);
  fprintf(stderr, "총 %d프레임 시뮬레이션, %d장 저장 (%s)\n", total, saved, out);
  fprintf(stderr, "끝난 위치 (%.2f, %.2f) 체력 %d 탄약 %d\n", st.px, st.py, st.health, st.ammo);
  return 0;
}
