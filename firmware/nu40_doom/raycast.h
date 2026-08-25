// NU40DK 단독 3D 엔진 — 플랫폼에 의존하지 않는 렌더러.
// 이 파일과 raycast.c 는 보드 펌웨어와 PC 시뮬레이터가 "그대로 같이" 쓴다.
// 그래서 PC에서 렌더링해 본 화면이 보드에서도 픽셀 단위로 똑같이 나온다.
#ifndef RAYCAST_H
#define RAYCAST_H

#include <stdint.h>

// raycast.c 는 C 로 컴파일되고 .ino 는 C++ 로 컴파일된다.
// 이걸 감싸지 않으면 이름 맹글링 때문에 링크에서 함수를 못 찾는다.
#ifdef __cplusplus
extern "C" {
#endif

#define RC_W 128      // OLED 가로
#define RC_H 64       // OLED 세로
#define RC_FB_BYTES (RC_W * RC_H / 8)   // 1024바이트. SSD1306 페이지 포맷

// 입력: 누르고 있는 동안 1
typedef struct {
  uint8_t forward, back;        // 전진 / 후진
  uint8_t turn_left, turn_right;// 좌우 회전
  uint8_t strafe_left, strafe_right;
  uint8_t fire;                 // 발사
  uint8_t use;                  // 문 열기
  uint8_t run;                  // 달리기
} rc_input_t;

typedef struct {
  float px, py;        // 플레이어 위치 (타일 단위)
  float dirx, diry;    // 보는 방향
  float planex, planey;// 카메라 평면 (시야각)
  int   health;        // 0..100
  int   ammo;
  int   fire_anim;     // 발사 반동 프레임 카운터
  uint32_t tick;
  uint8_t muzzle;      // 이번 프레임에 총구 섬광이 있는지
  uint8_t fps;         // 펌웨어가 실측해서 넣어준다
  uint8_t show_hud;    // 1이면 좌상단에 FPS, 우상단에 탄약을 그린다
} rc_state_t;

void rc_init(rc_state_t* s);
// dt_ms 만큼 시간을 진행시키며 입력을 반영한다.
void rc_update(rc_state_t* s, const rc_input_t* in, uint32_t dt_ms);
// fb 는 SSD1306 페이지 포맷 1024바이트. 한 바이트가 세로 8픽셀, 최하위 비트가 위.
void rc_render(const rc_state_t* s, uint8_t* fb);

// 데모(어트랙트) 모드: 정해진 경로를 스스로 돌아다니며 적을 쏜다.
// 조이스틱을 안 건드려도 화면이 계속 움직이므로 촬영에 쓴다.
void rc_demo_input(const rc_state_t* s, rc_input_t* out);

#ifdef __cplusplus
}
#endif

#endif
