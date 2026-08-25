// Doom 내부의 플레이어 체력을 꺼내오는 작은 다리.
// doomstat.h 를 이 파일에서만 포함해서 doomgeneric_nu40.c 가 엔진 헤더에 안 얽히게 한다.
#include "doomstat.h"
int g_player_health(void) {
  return players[consoleplayer].health;
}

// 메뉴(또는 타이틀·데모 화면)가 떠 있는지. 이때는 버튼 뜻이 달라져야 한다.
#include "m_menu.h"
int g_menu_active(void) {
  return menuactive || gamestate == GS_DEMOSCREEN;
}
// 메뉴가 "실제로 열려 있는지". 타이틀·데모 화면은 아직 안 열린 상태다.
// 둠 메뉴는 ENTER 로 안 열리고 ESC 로만 열린다. 그래서 이 둘을 구분해야 한다.
int g_menu_open(void) { return menuactive; }
// 실제 게임 플레이 중인지. 타이틀·메뉴·인터미션은 글자와 로고가 주인공이라
// 화면을 흑백으로 바꾸는 방식도 달라야 한다.
int g_in_level(void) { return gamestate == GS_LEVEL && !menuactive; }

// 타이틀 화면으로 되돌린다 (촬영용 리셋).
// 둠이 게임을 끝내고 인트로로 돌아갈 때 쓰는 바로 그 함수다.
extern void D_StartTitle(void);
void g_start_title(void) {
  // 메뉴를 먼저 닫아야 한다. 안 그러면 타이틀 화면인데 메뉴가 열린 상태로 남아서
  // 버튼 뜻(ESC냐 ENTER냐)이 어긋나고, 그다음 조작이 전부 밀린다.
  extern void M_ClearMenus(void);
  M_ClearMenus();
  D_StartTitle();
}

// 메뉴를 건너뛰고 바로 E1M1 을 시작한다 (촬영용 — 128x64 에서는 메뉴 글씨가 안 읽힌다).
// 난이도는 둠 기본 커서 위치와 같은 "Hurt me plenty"(sk_medium).
#include "g_game.h"
#include "p_local.h"
#include "tables.h"
#include "info.h"
extern void M_ClearMenus(void);   // m_menu.h 에 선언이 없어서 직접 적는다
// 촬영용 시작: E1M1 을 어려운 난이도로 열고, 맵이 뜨는 즉시 무기를 다 준다.
// 128x64 화면에서는 맵을 헤매기 어렵고 권총 한 자루로는 그림도 안 산다.
static int pending_give = 0;
void g_start_e1m1(void) {
  // 예약된 데모 전환을 먼저 취소한다.
  // 이게 남아 있으면 둠이 나중에 D_DoAdvanceDemo 에서 gameaction 을 지워버리고
  // 우리가 건 새 게임 대신 자기 데모를 틀어버린다.
  extern boolean advancedemo;
  extern boolean demoplayback;
  demoplayback = false;
  advancedemo  = false;

  M_ClearMenus();
  G_DeferedInitNew(sk_hard, 1, 1);   // Ultra-Violence: 적이 훨씬 많다
  pending_give = 1;
}

// 맵 로딩은 지연 처리라, 실제로 들어간 뒤에 지급해야 한다. 매 프레임 호출한다.
void g_tick_give(void) {
  if (!pending_give || gamestate != GS_LEVEL) return;
  player_t* p = &players[consoleplayer];
  int i;
  for (i = 0; i < NUMWEAPONS; i++) p->weaponowned[i] = true;
  for (i = 0; i < NUMAMMO; i++)    p->ammo[i] = p->maxammo[i];
  p->armorpoints = 200;
  p->armortype   = 2;

  // 눈앞에 적을 세워둔다. E1M1 은 시작 방이 비어 있어서, 촬영 때 한참 걸어가야 한다.
  // (UV 로 올려도 가장 가까운 적이 851유닛 = 방 한두 개 거리다)
  {
    static const int deg[3]  = { -22, 0, 22 };
    static const mobjtype_t kind[3] = { MT_POSSESSED, MT_TROOP, MT_SHOTGUY };
    static const int tryd[4] = { 256, 200, 160, 112 };
    mobj_t* pm = p->mo;
    int i, k;
    if (pm) for (i = 0; i < 3; i++) {
      angle_t a = pm->angle + (angle_t)(((double)deg[i] / 90.0) * (double)ANG90);
      for (k = 0; k < 4; k++) {
        fixed_t x = pm->x + FixedMul(tryd[k] << FRACBITS, finecosine[a >> ANGLETOFINESHIFT]);
        fixed_t y = pm->y + FixedMul(tryd[k] << FRACBITS, finesine[a >> ANGLETOFINESHIFT]);
        mobj_t* mo = P_SpawnMobj(x, y, ONFLOORZ, kind[i]);
        if (mo && P_CheckPosition(mo, x, y)) { mo->angle = a + ANG180; break; }
        if (mo) P_RemoveMobj(mo);          // 벽 속이면 지우고 더 가까이 다시 시도
      }
    }
  }
  pending_give = 0;
}
