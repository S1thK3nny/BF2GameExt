#include "pch.h"
#include "menu_navigation.hpp"
#include "controller_support.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"
#include "lua/lua_hooks.hpp"

#include <detours.h>
#include <math.h>
#include <string.h>

#include "menu_navigation_shell.inc"

// =============================================================================
// Pad -> menu inputs
//
// Every menu reads GUIInputs, filled from RawControllerInputs::s_defUIBindings.
// The PC builds ship the pad rows of that table empty (the first four buttons
// all Accept), so a pad could never drive a menu.
// =============================================================================

enum eGUIINPUT_TYPE : int32_t {
   eGUIINPUT_NONE          = -1,
   eGUIINPUT_Accept        = 0,
   eGUIINPUT_Back          = 1,
   eGUIINPUT_Start         = 2,
   eGUIINPUT_Select        = 3,
   eGUIINPUT_Misc1         = 4,
   eGUIINPUT_Misc2         = 5,
   eGUIINPUT_Up            = 6,
   eGUIINPUT_Down          = 7,
   eGUIINPUT_Left          = 8,
   eGUIINPUT_Right         = 9,
   eGUIINPUT_LeftTrigger   = 10,
   eGUIINPUT_RightTrigger  = 11,
   eGUIINPUT_LeftTrigger2  = 12,
   eGUIINPUT_RightTrigger2 = 13,
   eGUIINPUT_LeftTrigger3  = 14,
   eGUIINPUT_RightTrigger3 = 15,
   eGUIINPUT_yAxis         = 16,
   eGUIINPUT_xAxis         = 17,
};

struct UIBindingRow {
   int32_t keyA;
   int32_t keyB;
};

static constexpr int kNumRows   = 0x4C;
static constexpr int kTableSize = kNumRows * (int)sizeof(UIBindingRow);

// The keyboard and mouse never drive the Trigger2 pair, so the shoulder
// buttons cannot collide with PageUp/PageDown paging. The triggers are not in
// here: the menu input re-fires an analog value on every change, so they go
// to Lua through GameExt_PadTriggers instead.
static const struct { int raw; int32_t gui; } kMenuNav[] = {
   { eCONTROLLERINPUT_BUTTON0,     eGUIINPUT_Accept },        // A
   { eCONTROLLERINPUT_BUTTON1,     eGUIINPUT_Back },          // B
   { eCONTROLLERINPUT_BUTTON2,     eGUIINPUT_Misc1 },         // X
   { eCONTROLLERINPUT_BUTTON3,     eGUIINPUT_Misc2 },         // Y
   { eCONTROLLERINPUT_BUTTON4,     eGUIINPUT_LeftTrigger2 },  // LB: top tab row
   { eCONTROLLERINPUT_BUTTON5,     eGUIINPUT_RightTrigger2 }, // RB
   { eCONTROLLERINPUT_BUTTON6,     eGUIINPUT_Select },        // View
   { eCONTROLLERINPUT_BUTTON7,     eGUIINPUT_Start },         // Menu
   { eCONTROLLERINPUT_HAT0_UP,     eGUIINPUT_Up },
   { eCONTROLLERINPUT_HAT0_RIGHT,  eGUIINPUT_Right },
   { eCONTROLLERINPUT_HAT0_DOWN,   eGUIINPUT_Down },
   { eCONTROLLERINPUT_HAT0_LEFT,   eGUIINPUT_Left },
   // xAxis/yAxis are assigned, not accumulated: exactly one row each.
   { eCONTROLLERINPUT_X_POS,       eGUIINPUT_xAxis },
   { eCONTROLLERINPUT_Y_NEG,       eGUIINPUT_yAxis },         // DirectInput up is -Y
};

static void build_stock_table(UIBindingRow* out)
{
   for (int i = 0; i < kNumRows; ++i) out[i] = { eGUIINPUT_NONE, eGUIINPUT_NONE };
   out[0].keyA = out[1].keyA = out[2].keyA = out[3].keyA = eGUIINPUT_Accept;
   out[0x42].keyA = eGUIINPUT_Accept;      // left mouse
   out[0x43].keyA = eGUIINPUT_Misc1;       // right mouse
   out[0x44].keyA = eGUIINPUT_LeftTrigger; // middle mouse
   out[0x4A].keyA = eGUIINPUT_Up;          // wheel up
   out[0x4B].keyA = eGUIINPUT_Down;        // wheel down
}

static UIBindingRow* s_table = nullptr;
static UIBindingRow  s_tableOrig[kNumRows] = {};

static void install_ui_table(uintptr_t exe_base)
{
   if (!g_addr->gui_ui_bindings_table) return;
   UIBindingRow* const table = (UIBindingRow*)resolve(exe_base, g_addr->gui_ui_bindings_table);

   // Whole-table match, so a moved table or another patch is left alone.
   UIBindingRow stock[kNumRows];
   build_stock_table(stock);
   if (memcmp(table, stock, kTableSize) != 0) {
      install_log("[MenuNav] UI binding table is not the stock one, left alone");
      return;
   }

   memcpy(s_tableOrig, table, kTableSize);
   s_table = table;
   for (const auto& b : kMenuNav) table[b.raw] = { b.gui, eGUIINPUT_NONE };
}


// =============================================================================
// Pad state
// =============================================================================

// RawControllerInputs = FLInputManager + 0x428; mRawInputFloat[10][76] at +0x14.
// Axis rows are signed: X_POS holds x, X_NEG holds -x.
constexpr uint32_t kRawInputs    = 0x428 + 0x14;
constexpr int      kRawPerDevice = 76;
constexpr int      kNumDevices   = 10;

static const float* s_raw = nullptr;

static float pad(int raw)
{
   float v = 0.0f;
   for (int d = 0; d < kNumDevices; ++d) {
      const float f = s_raw[d * kRawPerDevice + raw];
      if (f > v) v = f;
   }
   return v;
}

// XInput reports LT and RT separately. DirectInput folds them into one Z axis,
// whose sign differs between drivers, so it is only the fallback.
struct XIGamepad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XIState   { DWORD packet; XIGamepad pad; };
using fn_XInputGetState = DWORD(WINAPI*)(DWORD, XIState*);

static fn_XInputGetState s_xiGetState = nullptr;
static bool              s_xiTried    = false;

static bool xinput_triggers(float& lt, float& rt)
{
   if (!s_xiTried) {
      s_xiTried = true;
      for (const char* dll : { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" }) {
         if (HMODULE h = LoadLibraryA(dll)) {
            s_xiGetState = (fn_XInputGetState)GetProcAddress(h, "XInputGetState");
            if (s_xiGetState) break;
         }
      }
   }
   if (!s_xiGetState) return false;

   // Polling an empty slot is slow, so only rescan the others once a second.
   static DWORD slot = 0, nextScan = 0;
   XIState st = {};
   if (s_xiGetState(slot, &st) != ERROR_SUCCESS) {
      const DWORD now = GetTickCount();
      if (now < nextScan) return false;
      nextScan = now + 1000;
      DWORD i = 0;
      for (; i < 4 && s_xiGetState(i, &st) != ERROR_SUCCESS; ++i) {}
      if (i == 4) return false;
      slot = i;
   }
   lt = st.pad.lt / 255.0f;
   rt = st.pad.rt / 255.0f;
   return true;
}

// GameExt_PadTriggers() -> lt, rt as 1/0, with hysteresis so a resting finger
// does not chatter.
static int __cdecl lua_pad_triggers(lua_State* L)
{
   static bool ltOn = false, rtOn = false;
   float lt = 0.0f, rt = 0.0f;
   if (!xinput_triggers(lt, rt) && s_raw) {
      lt = fmaxf(pad(eCONTROLLERINPUT_BUTTON10), pad(eCONTROLLERINPUT_Z_POS));
      rt = fmaxf(pad(eCONTROLLERINPUT_BUTTON11), pad(eCONTROLLERINPUT_Z_NEG));
   }
   ltOn = ltOn ? lt > 0.2f : lt > 0.5f;
   rtOn = rtOn ? rt > 0.2f : rt > 0.5f;
   g_lua.pushnumber(L, ltOn ? 1.0f : 0.0f);
   g_lua.pushnumber(L, rtOn ? 1.0f : 0.0f);
   return 2;
}

// GameExt_PadDir() -> 1 while a d-pad direction or the left stick is pushed.
// Tells a pad Up/Down from the mouse wheel, which sends the same inputs.
static int __cdecl lua_pad_dir(lua_State* L)
{
   bool held = false;
   if (s_raw) {
      for (int raw : { eCONTROLLERINPUT_HAT0_UP, eCONTROLLERINPUT_HAT0_RIGHT, eCONTROLLERINPUT_HAT0_DOWN,
                       eCONTROLLERINPUT_HAT0_LEFT, eCONTROLLERINPUT_X_POS, eCONTROLLERINPUT_X_NEG,
                       eCONTROLLERINPUT_Y_POS, eCONTROLLERINPUT_Y_NEG })
         if (pad(raw) > 0.5f) held = true;
   }
   g_lua.pushnumber(L, held ? 1.0f : 0.0f);
   return 1;
}

// GameExt_PadButton(n) -> 1 while pad button n (0 = A, 1 = B, 2 = X, 3 = Y) is down.
// Tells a pad X/Y from the right mouse button, which is Misc too.
static int __cdecl lua_pad_button(lua_State* L)
{
   const int n = g_lua.tonumber ? (int)g_lua.tonumber(L, 1) : -1;
   const bool down = s_raw && n >= 0 && n < 16 && pad(eCONTROLLERINPUT_BUTTON0 + n) >= 0.5f;
   g_lua.pushnumber(L, down ? 1.0f : 0.0f);
   return 1;
}

// GUIInputs for player 0: FLInputManager + 0x2D14 on all builds.
constexpr uint32_t kGuiInputsOffset = 0x2D14;
constexpr uint32_t kGuiProcessed    = 0x68;  // float[26]
constexpr uint32_t kGuiMouse        = 0x138; // int x, y
static uint8_t*    s_gui            = nullptr;

// GameExt_MouseMoved() -> 1 if the menu cursor moved since the last call.
static int __cdecl lua_mouse_moved(lua_State* L)
{
   static int lastX = 0, lastY = 0;
   bool moved = false;
   if (s_gui) {
      const int* mouse = (const int*)(s_gui + kGuiMouse);
      moved = mouse[0] != lastX || mouse[1] != lastY;
      lastX = mouse[0];
      lastY = mouse[1];
   }
   g_lua.pushnumber(L, moved ? 1.0f : 0.0f);
   return 1;
}

// GameExt_PadAny() -> 1 while any button, the d-pad or the left stick is in use.
static int __cdecl lua_pad_any(lua_State* L)
{
   bool any = false;
   if (s_raw) {
      for (int b = eCONTROLLERINPUT_BUTTON0; b <= eCONTROLLERINPUT_BUTTON11 && !any; ++b)
         any = pad(b) >= 0.5f;
      for (int raw : { eCONTROLLERINPUT_HAT0_UP, eCONTROLLERINPUT_HAT0_RIGHT, eCONTROLLERINPUT_HAT0_DOWN,
                       eCONTROLLERINPUT_HAT0_LEFT, eCONTROLLERINPUT_X_POS, eCONTROLLERINPUT_X_NEG,
                       eCONTROLLERINPUT_Y_POS, eCONTROLLERINPUT_Y_NEG })
         if (pad(raw) > 0.5f) any = true;
   }
   g_lua.pushnumber(L, any ? 1.0f : 0.0f);
   return 1;
}

// GameExt_PadStick() -> x, y of the left stick and d-pad, right and up positive,
// the way the console ScriptCB_ReadLeftstick reports them.
static int __cdecl lua_pad_stick(lua_State* L)
{
   float x = 0.0f, y = 0.0f;
   if (s_raw) {
      x = pad(eCONTROLLERINPUT_X_POS) - pad(eCONTROLLERINPUT_X_NEG)
        + pad(eCONTROLLERINPUT_HAT0_RIGHT) - pad(eCONTROLLERINPUT_HAT0_LEFT);
      y = pad(eCONTROLLERINPUT_Y_NEG) - pad(eCONTROLLERINPUT_Y_POS)
        + pad(eCONTROLLERINPUT_HAT0_UP) - pad(eCONTROLLERINPUT_HAT0_DOWN);
   }
   g_lua.pushnumber(L, fmaxf(-1.0f, fminf(1.0f, x)));
   g_lua.pushnumber(L, fmaxf(-1.0f, fminf(1.0f, y)));
   return 2;
}

// =============================================================================
// Menu screens
//
// The PC screens act on whatever the mouse is over. menu_navigation_shell.inc
// gives them a pad focus. It runs in the shell's Lua state right after
// ShellLoop::Init has loaded every screen, and in the mission's state right
// after ReadDataFile("ingame.lvl") has run game_interface. Both are fresh
// states each time.
// =============================================================================

using fn_lua_dobuffer    = int(__cdecl*)(lua_State* L, const char* buff, unsigned int size, const char* name);
using fn_shell_loop_init = int(__cdecl*)();
using fn_lua_cfunction   = int(__cdecl*)(lua_State* L);

static fn_shell_loop_init s_origShellLoopInit = nullptr;
static fn_lua_cfunction   s_origReadDataFile  = nullptr;
static fn_lua_dobuffer    s_luaDoBuffer       = nullptr;
static lua_State**        s_luaStatePtr       = nullptr;

static void run_pad_script(lua_State* L)
{
   if (!L) return;
   lua_register_func(L, "GameExt_PadTriggers", lua_pad_triggers);
   lua_register_func(L, "GameExt_PadDir", lua_pad_dir);
   lua_register_func(L, "GameExt_PadButton", lua_pad_button);
   lua_register_func(L, "GameExt_PadStick", lua_pad_stick);
   lua_register_func(L, "GameExt_PadAny", lua_pad_any);
   lua_register_func(L, "GameExt_MouseMoved", lua_mouse_moved);
   const int rc = s_luaDoBuffer(L, kShellPadLua, (unsigned int)(sizeof(kShellPadLua) - 1),
                                "@BF2GameExt_menu_navigation");
   if (rc != 0)
      if (auto log = get_gamelog()) log("[MenuNav] pad script failed (%d)\n", rc);
}

static int __cdecl hooked_shell_loop_init()
{
   const int r = s_origShellLoopInit();
   run_pad_script(*s_luaStatePtr);
   return r;
}

static int __cdecl hooked_lua_read_data_file(lua_State* L)
{
   const char* name = (L && g_lua.tolstring) ? g_lua.tolstring(L, 1, nullptr) : nullptr;
   const bool ingame = name && _stricmp(name, "ingame.lvl") == 0;
   const int r = s_origReadDataFile(L);
   if (ingame) run_pad_script(L);
   return r;
}

// =============================================================================
// Spawn screen
//
// SpawnDisplay is C++ and only knows "Accept while the mouse is over a hotspot".
// A pad press is turned into exactly that: for one UpdateInput call the mouse
// is placed inside the target's hotspot and Accept is held, so the engine's own
// class, team and spawn logic runs unchanged.
//
// Up/down pick the class, left/right step through the spawn points on the map,
// LB/RB switch team, A spawns.
// =============================================================================

namespace spawn {
   constexpr uint32_t kMode        = 0x18;
   constexpr uint32_t kSlotBorders = 0x5D8; // RedElement*[10]
   constexpr uint32_t kSideIcons   = 0x604; // RedElement*[2]
   constexpr uint32_t kAcceptBox   = 0x61C; // the Ok label
   constexpr uint32_t kElemFlags   = 0x14;  // bit 8 = shown
   constexpr uint32_t kElemHotspot = 0x18;
   constexpr int      kModeChangeClass = 3;
   constexpr int      kNumSlots = 10;
   constexpr uint32_t kPlayer      = 0x1FFC;
   // Map objects hang off spawn_map_list; same layout on every build.
   constexpr uint32_t kMapNodePlayer = 0x24B00; // node - this = player index
   constexpr uint32_t kMapNodeBase   = 0x24C68; // node - this = map
   constexpr uint32_t kMapHotspots   = 0x1ECA8; // RedHotSpot*, flags at -4
   constexpr uint32_t kMapStride     = 0x100;
   // spawn_map_posts: 16 records of 0x30, the first field a post pointer.
   constexpr int      kNumPosts      = 16;
   constexpr uint32_t kPostRecStride = 0x30;
   constexpr uint32_t kPostEntity    = 0x2C;  // CommandPost*, valid while +0x30 matches
   constexpr uint32_t kPostHandle    = 0x30;
   constexpr uint32_t kEntityHandle  = 0x204;
   constexpr uint32_t kEntityTeam    = 0x234; // low 4 bits, signed
}

using fn_update_input = void(__thiscall*)(uint8_t* self);
using fn_point_inside = bool(__thiscall*)(void* hotspot, float x, float y);

static fn_update_input s_origUpdateInput = nullptr;
static fn_point_inside s_pointInside     = nullptr;
static uint32_t        s_classIndexOff   = 0;
static uint32_t        s_teamOff         = 0;
static uint32_t        s_postOff         = 0;
static uint8_t*        s_mapList         = nullptr;
static uint8_t*        s_posts           = nullptr;

struct Repeat {
   bool   held = false;
   double next = 0.0;
};

static double now_seconds()
{
   static LARGE_INTEGER freq = {};
   if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return (double)t.QuadPart / (double)freq.QuadPart;
}

static bool fire(Repeat& r, bool down, double now, bool repeat = true)
{
   if (!down) { r.held = false; return false; }
   if (!r.held) { r.held = true; r.next = now + 0.4; return true; }
   if (!repeat || now < r.next) return false;
   r.next = now + 0.15;
   return true;
}

static uint8_t* elem(uint8_t* self, uint32_t off) { return *(uint8_t**)(self + off); }
static void* hotspot(uint8_t* e) { return e ? *(void**)(e + spawn::kElemHotspot) : nullptr; }
static bool shown(uint8_t* e) { return e && ((*(uint32_t*)(e + spawn::kElemFlags) >> 8) & 1); }

static uint8_t* next_slot(uint8_t* self, int dir)
{
   for (int i = *(int*)(self + s_classIndexOff) + dir; i >= 0 && i < spawn::kNumSlots; i += dir) {
      uint8_t* e = elem(self, spawn::kSlotBorders + i * 4);
      if (shown(e) && hotspot(e)) return e;
   }
   return nullptr;
}

static uint8_t* spawn_map(uint8_t* self)
{
   if (!s_mapList) return nullptr;
   const int player = *(int*)(self + spawn::kPlayer);
   for (uint8_t* n = *(uint8_t**)s_mapList; n && n != s_mapList; n = *(uint8_t**)n)
      if (*(int*)(n - spawn::kMapNodePlayer) == player) return n - spawn::kMapNodeBase;
   return nullptr;
}

// The hotspot of the next post on the map the team can spawn at, in the same
// order and with the same tests as SpawnDisplay::FindPost.
static void* next_post(uint8_t* self, int dir)
{
   uint8_t* map = spawn_map(self);
   if (!map || !s_posts) return nullptr;
   const int   team = *(int*)(self + s_teamOff);
   const void* cur  = *(void**)(self + s_postOff);
   void* spots[spawn::kNumPosts];
   int n = 0, at = -1;
   for (int i = 0; i < spawn::kNumPosts; ++i) {
      uint8_t* rec = *(uint8_t**)(s_posts + i * spawn::kPostRecStride);
      if (!rec) continue;
      uint8_t* post = *(uint8_t**)(rec + spawn::kPostEntity);
      if (!post || *(int*)(post + spawn::kEntityHandle) != *(int*)(rec + spawn::kPostHandle)) continue;
      int postTeam = *(int*)(post + spawn::kEntityTeam) & 0xF;
      if (postTeam & 8) postTeam -= 16;
      if (postTeam != team) continue;
      uint8_t* slot = map + spawn::kMapHotspots + i * spawn::kMapStride;
      void* hs = *(void**)slot;
      if (!hs || !((*(uint32_t*)(slot - 4) >> 8) & 1)) continue;
      if (rec == cur) at = n;
      spots[n++] = hs;
   }
   if (!n) return nullptr;
   const int next = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n;
   return next == at ? nullptr : spots[next];
}

// Any point the engine itself calls inside: the centroid of a pixel grid.
static bool point_in(void* hs, int& ox, int& oy)
{
   constexpr int kStep = 6, kMaxW = 4096, kMaxH = 2400;
   long long sx = 0, sy = 0, n = 0;
   for (int y = kStep / 2; y < kMaxH; y += kStep)
      for (int x = kStep / 2; x < kMaxW; x += kStep)
         if (s_pointInside(hs, (float)x, (float)y)) { sx += x; sy += y; ++n; }
   if (!n) return false;
   ox = (int)(sx / n);
   oy = (int)(sy / n);
   return true;
}

static bool click(uint8_t* self, void* hs, bool padAccept)
{
   int x, y;
   if (!hs || !point_in(hs, x, y)) return false;

   int*   mouse = (int*)(s_gui + kGuiMouse);
   float* accept = (float*)(s_gui + kGuiProcessed);
   const int   mx = mouse[0], my = mouse[1];
   const float acc = *accept;
   mouse[0] = x;
   mouse[1] = y;
   *accept = 1.0f;
   s_origUpdateInput(self);
   mouse[0] = mx;
   mouse[1] = my;
   // A pad Accept is spent: Update would otherwise test it against the real cursor.
   *accept = padAccept ? 0.0f : acc;
   return true;
}

static void __fastcall hooked_update_input(uint8_t* self, void* /*edx*/)
{
   static Repeat up, down, left, right, lb, rb;
   static double lastCall = 0.0;

   const double now = now_seconds();
   const bool dUp    = pad(eCONTROLLERINPUT_HAT0_UP)    >= 0.5f || pad(eCONTROLLERINPUT_Y_NEG) >= 0.5f;
   const bool dDown  = pad(eCONTROLLERINPUT_HAT0_DOWN)  >= 0.5f || pad(eCONTROLLERINPUT_Y_POS) >= 0.5f;
   const bool dLeft  = pad(eCONTROLLERINPUT_HAT0_LEFT)  >= 0.5f || pad(eCONTROLLERINPUT_X_NEG) >= 0.5f;
   const bool dRight = pad(eCONTROLLERINPUT_HAT0_RIGHT) >= 0.5f || pad(eCONTROLLERINPUT_X_POS) >= 0.5f;
   const bool dLB    = pad(eCONTROLLERINPUT_BUTTON4) >= 0.5f;
   const bool dRB    = pad(eCONTROLLERINPUT_BUTTON5) >= 0.5f;

   // Re-entering the screen: anything still held from before is not a press.
   if (now - lastCall > 0.25) {
      up.held = dUp; down.held = dDown; left.held = dLeft; right.held = dRight;
      lb.held = dLB; rb.held = dRB;
      up.next = down.next = left.next = right.next = now + 0.4;
   }
   lastCall = now;

   const bool goUp = fire(up, dUp, now), goDown = fire(down, dDown, now);
   const bool goTeam = fire(lb, dLB, now, false) | fire(rb, dRB, now, false);
   const bool goPrev = fire(left, dLeft, now), goNext = fire(right, dRight, now);
   const bool canSwitch = *(int*)(self + spawn::kMode) != spawn::kModeChangeClass;
   const bool padAccept = ((float*)(s_gui + kGuiProcessed))[eGUIINPUT_Accept] >= 0.125f &&
                          pad(eCONTROLLERINPUT_BUTTON0) >= 0.5f;

   void* target = nullptr;
   if (padAccept)
      target = hotspot(elem(self, spawn::kAcceptBox));
   else if (goUp || goDown)
      target = hotspot(next_slot(self, goUp ? -1 : 1));
   else if (goTeam && canSwitch)
      target = hotspot(elem(self, spawn::kSideIcons + (*(int*)(self + s_teamOff) & 1) * 4));
   else if ((goPrev || goNext) && canSwitch)
      target = next_post(self, goNext ? 1 : -1);

   if (!(target && click(self, target, padAccept)))
      s_origUpdateInput(self);
}

// =============================================================================
// Install
// =============================================================================

void menu_navigation_install(uintptr_t exe_base)
{
   if (!g_controllerEnabled) return;

   install_ui_table(exe_base);

   if (g_addr->controller_base_global) {
      const uintptr_t input = (uintptr_t)resolve(exe_base, g_addr->controller_base_global);
      s_raw = (const float*)(input + kRawInputs);
      s_gui = (uint8_t*)(input + kGuiInputsOffset);
   }

   const bool lua = g_addr->lua_dobuffer && g_addr->g_lua_state_ptr && g_lua.pushnumber;
   const bool shell = lua && g_addr->shell_loop_init;
   const bool ingame = lua && g_addr->lua_read_data_file;
   const bool spawnScreen = g_addr->spawn_display_update_input && g_addr->red_hotspot_is_point_inside && s_raw;

   s_luaDoBuffer = (fn_lua_dobuffer)resolve(exe_base, g_addr->lua_dobuffer);
   s_luaStatePtr = (lua_State**)resolve(exe_base, g_addr->g_lua_state_ptr);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (shell) {
      s_origShellLoopInit = (fn_shell_loop_init)resolve(exe_base, g_addr->shell_loop_init);
      DetourAttach(&(PVOID&)s_origShellLoopInit, hooked_shell_loop_init);
   }
   if (ingame) {
      s_origReadDataFile = (fn_lua_cfunction)resolve(exe_base, g_addr->lua_read_data_file);
      DetourAttach(&(PVOID&)s_origReadDataFile, hooked_lua_read_data_file);
   }
   if (spawnScreen) {
      s_pointInside   = (fn_point_inside)resolve(exe_base, g_addr->red_hotspot_is_point_inside);
      s_classIndexOff = g_build == GameBuild::Modtools ? 0x20A0 : 0x2064;
      s_teamOff       = g_build == GameBuild::Modtools ? 0x2090 : 0x2054;
      s_postOff       = g_build == GameBuild::Modtools ? 0x2094 : 0x2058;
      if (g_addr->spawn_map_list && g_addr->spawn_map_posts) {
         s_mapList = (uint8_t*)resolve(exe_base, g_addr->spawn_map_list);
         s_posts   = (uint8_t*)resolve(exe_base, g_addr->spawn_map_posts);
      }
      s_origUpdateInput = (fn_update_input)resolve(exe_base, g_addr->spawn_display_update_input);
      DetourAttach(&(PVOID&)s_origUpdateInput, hooked_update_input);
   }
   if (DetourTransactionCommit() != NO_ERROR) {
      s_origShellLoopInit = nullptr;
      s_origReadDataFile = nullptr;
      s_origUpdateInput = nullptr;
      install_log("[MenuNav] hooks failed to install");
      return;
   }
   install_log("[MenuNav] pad menu navigation: table %s, shell %s, in game %s, spawn screen %s",
               s_table ? "on" : "off", shell ? "on" : "off", ingame ? "on" : "off",
               spawnScreen ? "on" : "off");
}

void menu_navigation_uninstall()
{
   if (s_table) {
      protected_write(s_table, s_tableOrig, kTableSize);
      s_table = nullptr;
   }
   if (!s_origShellLoopInit && !s_origReadDataFile && !s_origUpdateInput) return;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (s_origShellLoopInit) DetourDetach(&(PVOID&)s_origShellLoopInit, hooked_shell_loop_init);
   if (s_origReadDataFile)  DetourDetach(&(PVOID&)s_origReadDataFile, hooked_lua_read_data_file);
   if (s_origUpdateInput)   DetourDetach(&(PVOID&)s_origUpdateInput, hooked_update_input);
   DetourTransactionCommit();
   s_origShellLoopInit = nullptr;
   s_origReadDataFile = nullptr;
   s_origUpdateInput = nullptr;
}
