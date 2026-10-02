/*
  A안 펌웨어 — 보드를 "Doom 전용 화면 + 컨트롤러"로 만든다.
  게임 연산은 맥의 host/doomgeneric_nu40.c 가 한다. 보드는 받은 그림을 그리고
  조이스틱·버튼 상태를 돌려보낸다.

  보드가 혼자 3D를 돌리는 쪽은 firmware/nu40_doom 이다.

  프로토콜
    PC -> 보드 : 0xA5 0x5A, 체력 1바이트, 프레임 1024바이트 (SSD1306 페이지 포맷)
    보드 -> PC : 0x49('I'), 입력비트 하위, 입력비트 상위   (30ms 마다, 바뀔 때는 즉시)

  배선은 firmware/nu40_doom 과 완전히 같다.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define FB_BYTES      (SCREEN_WIDTH * SCREEN_HEIGHT / 8)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
static bool displayOk = false;

static const uint8_t PIN_JOY_X = A0;
static const uint8_t PIN_JOY_Y = A1;
static const uint8_t BUTTON_PINS[4] = { 11, 12, 24, 25 };
static const uint8_t LED_PINS[4]    = { 13, 14, 15, 16 };
static int joyCenterX = 2048, joyCenterY = 2048;
static const int JOY_DEADZONE = 300;
static bool joyPresent = false;    // 안 꽂으면 뜬 핀이라 유령 조작이 생긴다. 아예 무시한다.

// host/doomgeneric_nu40.c 의 enum 과 순서가 같아야 한다
enum {
  IN_FORWARD = 1 << 0, IN_BACK    = 1 << 1, IN_TURNL = 1 << 2, IN_TURNR = 1 << 3,
  IN_STRAFEL = 1 << 4, IN_STRAFER = 1 << 5, IN_FIRE  = 1 << 6, IN_USE   = 1 << 7,
  IN_RUN     = 1 << 8, IN_ESC     = 1 << 9, IN_ENTER = 1 << 10,
};

static uint8_t  frame[FB_BYTES];
static uint16_t framePos = 0;
static uint8_t  rxState  = 0;      // 0=0xA5 대기, 1=0x5A 대기, 2=체력, 3=프레임 수신중
static uint16_t lastMask = 0xffff;
static uint32_t lastSendMs = 0;
static uint16_t frameCount = 0;
static uint32_t fpsWindowMs = 0;

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

// 중앙값과 함께 "흔들림 폭"도 재서 조이스틱이 꽂혀 있는지 판정한다.
static void calibrateJoystick() {
  long sx = 0, sy = 0;
  int loX = 4095, hiX = 0, loY = 4095, hiY = 0;
  for (int i = 0; i < 32; i++) {
    int vx = analogRead(PIN_JOY_X), vy = analogRead(PIN_JOY_Y);
    sx += vx; sy += vy;
    if (vx < loX) loX = vx;  if (vx > hiX) hiX = vx;
    if (vy < loY) loY = vy;  if (vy > hiY) hiY = vy;
    delay(4);
  }
  joyCenterX = sx / 32; joyCenterY = sy / 32;
  int spread = (hiX - loX) > (hiY - loY) ? (hiX - loX) : (hiY - loY);
  joyPresent = spread < 120 && joyCenterX > 700 && joyCenterX < 3400
                            && joyCenterY > 700 && joyCenterY < 3400;
}

static void showHealthOnLeds(int health) {
  int lit = (health + 24) / 25;
  if (lit > 4) lit = 4;
  if (lit < 0) lit = 0;
  for (int i = 0; i < 4; i++) digitalWrite(LED_PINS[i], i < lit ? HIGH : LOW);
}

// 버튼 4개만으로 조작한다 (B안과 같은 배치).
//   1 좌회전   2 전진(문 자동으로 열림)   3 우회전   4 발사
//   1+3 = ESC 조합키. 뗄 때 맥이 누른 시간으로 판정한다:
//         0.5초 미만 = 메뉴 열기/닫기, 0.5~2.5초 = 타이틀로 리셋, 2.5초 이상 = E1M1 바로 시작
//   메뉴·타이틀 화면에서는 맥(host/doomgeneric_nu40.c)이 뜻을 바꾼다:
//         1 = 위, 3 = 아래, 4 = 확인(ENTER, 메뉴가 안 열렸으면 ESC로 메뉴 열기)
//   ENTER 전용 조합키는 없다. IN_ENTER 비트는 보드에서 보내지 않는다.
// 조이스틱은 꽂으면 추가로 먹는다.
static uint16_t readInputMask() {
  uint16_t m = 0;
  bool b1 = btnPressed(0);
  bool b2 = btnPressed(1);
  bool b3 = btnPressed(2);
  bool b4 = btnPressed(3);

  // 1+3 (좌회전+우회전) 만 조합키로 쓴다. 게임 중에 동시에 누를 일이 없는 유일한 짝이다.
  // 2+4 (전진+발사) 는 조합키로 쓰면 안 된다 — 걸어가며 쏘는 가장 기본 동작이다.
  if (b1 && b3) m |= IN_ESC;
  else {
    if (b1) m |= IN_TURNL;
    if (b3) m |= IN_TURNR;
  }
  if (b4) m |= IN_FIRE;
  if (b2) m |= IN_FORWARD | IN_USE;   // 걸어가면 앞의 문이 열린다

  if (joyPresent) {
    int dx = analogRead(PIN_JOY_X) - joyCenterX;
    int dy = analogRead(PIN_JOY_Y) - joyCenterY;
    if (dy >  JOY_DEADZONE) m |= IN_FORWARD | IN_USE;
    if (dy < -JOY_DEADZONE) m |= IN_BACK;
    if (dx >  JOY_DEADZONE) m |= IN_TURNR;
    if (dx < -JOY_DEADZONE) m |= IN_TURNL;
  }
  return m;
}

void setup() {
  Serial.begin(115200);
  calibrateButtons();          // 안 눌린 상태를 기준값으로 잡는다
  for (int i = 0; i < 4; i++) { pinMode(LED_PINS[i], OUTPUT); digitalWrite(LED_PINS[i], LOW); }
  analogReadResolution(12);
  calibrateJoystick();

  Wire.begin();
  Wire.setClock(400000);
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!displayOk) displayOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  if (displayOk) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 28);
    display.println(" WAITING FOR DOOM...");
    display.display();
    Serial.write((uint8_t)0x4B);          // 첫 프레임을 받을 준비가 됐다
  } else {
    Serial.println("ERR|OLED not found. Check wiring: SDA=P0.26 / SCL=P0.27.");
  }
  fpsWindowMs = millis();
  Serial.println("READY");
}

void loop() {
  // ── 프레임 수신
  while (Serial.available()) {
    int b = Serial.read();
    if (b < 0) break;
    switch (rxState) {
      case 0: if (b == 0xA5) rxState = 1; break;
      case 1: rxState = (b == 0x5A) ? 2 : (b == 0xA5 ? 1 : 0); break;
      case 2: showHealthOnLeds(b); rxState = 3; framePos = 0; break;
      case 3:
        frame[framePos++] = (uint8_t)b;
        if (framePos >= FB_BYTES) {
          rxState = 0;
          if (displayOk) {
            memcpy(display.getBuffer(), frame, FB_BYTES);
            display.display();
          }
          frameCount++;
          // 다 그렸다고 알린다. PC 는 이걸 받아야 다음 프레임을 보낸다.
          // 이게 없으면 PC 가 보드보다 빨라서 수신 버퍼가 넘치고 화면이 찢어진다.
          Serial.write((uint8_t)0x4B);
        }
        break;
    }
  }

  // ── 입력 전송: 바뀌면 즉시, 아니면 30ms 마다 (연결 확인용 심박)
  uint32_t now = millis();
  uint16_t m = readInputMask();
  if (m != lastMask || now - lastSendMs >= 30) {
    uint8_t pkt[3] = { 0x49, (uint8_t)(m & 0xff), (uint8_t)(m >> 8) };
    Serial.write(pkt, 3);
    lastMask = m;
    lastSendMs = now;
  }

  if (now - fpsWindowMs >= 1000) {
    Serial.print("FPS|"); Serial.println(frameCount);
    frameCount = 0;
    fpsWindowMs = now;
  }
}
