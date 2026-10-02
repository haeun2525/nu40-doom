/*
  NU40DK 단독 3D 엔진 — 보드 혼자서 실시간 3D를 계산해 OLED에 그린다.
  PC도, 케이블도 필요 없다. 전원만 있으면 된다.

  하는 일:
    1) 조이스틱(아날로그 2축)과 보드 버튼 4개로 조작을 읽는다
    2) raycast.c 가 3D 화면을 계산해 1024바이트 프레임버퍼를 채운다
    3) 그 버퍼를 SSD1306 OLED로 밀어넣는다
    4) 체력을 보드 LED 4개로 표시한다

  배선:
    OLED   SDA -> P0.26,  SCL -> P0.27,  VCC -> 3V3,  GND -> GND
    (선택) 조이스틱 VRx -> A0(P0.02),  VRy -> A1(P0.03),  VCC -> 3V3,  GND -> GND
    ! 조이스틱은 반드시 3V3 에 연결한다. 5V 를 물리면 ADC 입력 범위를 넘긴다.
    ! 안 꽂아도 된다. 부팅할 때 꽂혔는지 알아서 판정하고, 없으면 아날로그를 무시한다.

  조작:  버튼 4개만으로 전부 된다. 조이스틱은 선택 사항.
           1=좌회전  2=전진  3=우회전  4=발사
           문은 앞에서 전진하면 자동으로 열린다
           2+4 같이 누르면 달리기, 1+3 같이 누르면 데모 모드 토글
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "raycast.h"

// ── 화면
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
static const uint8_t OLED_ADDR_MAIN = 0x3C;
static const uint8_t OLED_ADDR_ALT  = 0x3D;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
static bool displayOk = false;

// ── 입력 핀
static const uint8_t PIN_JOY_X = A0;   // P0.02
static const uint8_t PIN_JOY_Y = A1;   // P0.03
static const uint8_t BUTTON_PINS[4] = { 11, 12, 24, 25 };  // 누르면 LOW
static const uint8_t LED_PINS[4]    = { 13, 14, 15, 16 };

// 조이스틱 중앙값 (부팅할 때 실측한다. 보드마다 조금씩 다르다)
static int joyCenterX = 2048, joyCenterY = 2048;
static const int JOY_DEADZONE = 300;   // 12비트 기준. 손을 떼면 확실히 멈추게
static bool joyPresent = false;        // 조이스틱이 안 꽂혀 있으면 아날로그 입력을 아예 무시한다

// ── 게임 상태
static rc_state_t game;
static uint8_t    fb[RC_FB_BYTES];
static uint32_t   lastFrameMs = 0;
static uint32_t   lastInputMs = 0;
static bool       demoMode = false;
static bool       prevCombo = false;

// FPS 실측
static uint16_t   frameCount = 0;
static uint32_t   fpsWindowMs = 0;

// 조이스틱을 안 꽂으면 A0/A1 은 뜬 핀이라 값이 제멋대로 흔들린다.
// 그 흔들림이 데드존을 넘으면 유령 조작이 생겨 데모 모드가 계속 풀린다.
// 그래서 중앙값과 함께 "흔들림 폭"도 재서, 꽂혀 있는지를 판정한다.
// ── 버튼 읽기: 극성을 고정하지 않는다.
// 부팅할 때 안 눌린 상태를 기준으로 잡고, 거기서 "달라지면" 눌린 것으로 본다.
// 버튼이 GND 에 물렸든 VCC 에 물렸든, 보드에 외부 저항이 어느 쪽으로 걸렸든 동작한다.
static uint8_t  btnBase[4];
static uint32_t btnHeldSince[4];

static void calibrateButtons() {
  for (int i = 0; i < 4; i++) pinMode(BUTTON_PINS[i], INPUT_PULLUP);
  delay(150);
  for (int i = 0; i < 4; i++) { btnBase[i] = digitalRead(BUTTON_PINS[i]); btnHeldSince[i] = 0; }
}

static bool btnPressed(int i) {
  uint8_t v = digitalRead(BUTTON_PINS[i]);
  bool p = (v != btnBase[i]);
  uint32_t now = millis();
  if (p) {
    if (btnHeldSince[i] == 0) btnHeldSince[i] = now;
    else if (now - btnHeldSince[i] > 5000) {
      // 5초 넘게 눌린 채로 있으면 기준값을 잘못 잡은 것이다. 다시 잡는다.
      btnBase[i] = v; btnHeldSince[i] = 0; p = false;
    }
  } else btnHeldSince[i] = 0;
  return p;
}

static void calibrateJoystick() {
  long sx = 0, sy = 0;
  int loX = 4095, hiX = 0, loY = 4095, hiY = 0;
  for (int i = 0; i < 32; i++) {
    int vx = analogRead(PIN_JOY_X);
    int vy = analogRead(PIN_JOY_Y);
    sx += vx; sy += vy;
    if (vx < loX) loX = vx;  if (vx > hiX) hiX = vx;
    if (vy < loY) loY = vy;  if (vy > hiY) hiY = vy;
    delay(4);
  }
  joyCenterX = sx / 32;
  joyCenterY = sy / 32;

  // 꽂혀 있으면: 값이 조용하고(분압 저항) 중앙이 대략 한가운데에 온다.
  int spread = (hiX - loX) > (hiY - loY) ? (hiX - loX) : (hiY - loY);
  bool quiet    = spread < 120;
  bool centered = joyCenterX > 700 && joyCenterX < 3400 &&
                  joyCenterY > 700 && joyCenterY < 3400;
  joyPresent = quiet && centered;
}

static void showHealthOnLeds(int health) {
  int lit = (health + 24) / 25;          // 100->4, 75->3, 50->2, 25->1
  if (lit > 4) lit = 4;
  if (lit < 0) lit = 0;
  for (int i = 0; i < 4; i++) digitalWrite(LED_PINS[i], i < lit ? HIGH : LOW);
}

// 조작은 보드 버튼 4개만으로 끝난다. 조이스틱은 꽂으면 더 부드러워지는 선택 사항이다.
//   1 좌회전   2 전진   3 우회전   4 발사
//   문은 앞에 서서 전진하면 저절로 열린다 (따로 누를 버튼이 없다)
//   1+3 을 같이 누르면 데모 모드가 켜졌다 꺼졌다 한다
static void readInput(rc_input_t* in) {
  memset(in, 0, sizeof(*in));

  bool b1 = btnPressed(0);   // 좌회전
  bool b2 = btnPressed(1);   // 전진
  bool b3 = btnPressed(2);   // 우회전
  bool b4 = btnPressed(3);   // 발사

  bool combo = b1 && b3;                          // 데모 모드 토글용
  if (!combo) {
    in->turn_left  = b1;
    in->turn_right = b3;
  }
  in->forward = b2;
  in->fire    = b4;
  in->use     = b2;        // 전진하면 앞의 문을 자동으로 연다
  in->run     = b2 && b4;  // 전진+발사를 같이 누르면 달린다

  if (combo && !prevCombo) { demoMode = !demoMode; }
  prevCombo = combo;

  // 조이스틱을 꽂았으면 아날로그로도 움직인다 (없어도 그만)
  if (joyPresent) {
    int dx = analogRead(PIN_JOY_X) - joyCenterX;
    int dy = analogRead(PIN_JOY_Y) - joyCenterY;
    if (dx >  JOY_DEADZONE) in->turn_right = 1;
    if (dx < -JOY_DEADZONE) in->turn_left  = 1;
    if (dy >  JOY_DEADZONE) { in->forward = 1; in->use = 1; }
    if (dy < -JOY_DEADZONE) in->back    = 1;
  }

  bool touched = in->turn_right || in->turn_left || in->forward || in->back || b4;
  if (touched && !combo) {
    lastInputMs = millis();
    demoMode = false;          // 손을 대면 즉시 조작 모드로
  }
}

void setup() {
  Serial.begin(115200);

  calibrateButtons();          // 안 눌린 상태를 기준값으로 잡는다
  for (int i = 0; i < 4; i++) { pinMode(LED_PINS[i], OUTPUT); digitalWrite(LED_PINS[i], LOW); }

  analogReadResolution(12);          // 0..4095
  calibrateJoystick();

  Wire.begin();
  Wire.setClock(400000);             // 화면 갱신이 곧 프레임레이트라 최대한 올린다
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_MAIN);
  if (!displayOk) displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_ALT);
  if (!displayOk) {
    Serial.println("ERR|OLED not found. Check wiring: SDA=P0.26 / SCL=P0.27, 3V3 and GND.");
  }

  rc_init(&game);
  game.show_hud = 1;
  lastFrameMs = millis();
  fpsWindowMs = lastFrameMs;
  lastInputMs = lastFrameMs;
  Serial.print("READY|joy "); Serial.print(joyPresent ? "connected" : "none (running demo mode)");
  Serial.print(" center x="); Serial.print(joyCenterX);
  Serial.print(" y="); Serial.println(joyCenterY);

  // 조이스틱이 없으면 기다리지 말고 바로 데모 모드로 (촬영·전시용)
  if (!joyPresent) demoMode = true;
}

void loop() {
  uint32_t now = millis();
  uint32_t dt = now - lastFrameMs;
  if (dt < 1) return;                // 너무 잦은 갱신은 의미가 없다
  if (dt > 100) dt = 100;            // 멈췄다 돌아와도 순간이동하지 않게
  lastFrameMs = now;

  rc_input_t in;
  readInput(&in);

  // 20초 동안 아무 조작이 없으면 데모 모드로 넘어간다 (촬영·전시용)
  if (!demoMode && now - lastInputMs > 20000) demoMode = true;
  if (demoMode) rc_demo_input(&game, &in);

  rc_update(&game, &in, dt);
  rc_render(&game, fb);

  if (displayOk) {
    memcpy(display.getBuffer(), fb, RC_FB_BYTES);   // 포맷이 같아서 그대로 복사된다
    display.display();
  }

  showHealthOnLeds(game.health);

  // 1초마다 FPS를 재서 화면 HUD와 시리얼에 반영
  frameCount++;
  if (now - fpsWindowMs >= 1000) {
    game.fps = frameCount > 99 ? 99 : frameCount;
    Serial.print("FPS|"); Serial.print(frameCount);
    Serial.print("|HP|"); Serial.print(game.health);
    Serial.print("|AMMO|"); Serial.println(game.ammo);
    frameCount = 0;
    fpsWindowMs = now;
  }
}
