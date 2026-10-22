#include "pch.h"
#include "gamepad_sdl.hpp"
#include "controller_support.hpp"
#include "util/install_log.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <detours.h>
#include <intrin.h>
#include <math.h>
#include <string.h>

// =============================================================================
// SDL3, loaded by hand so a missing SDL3.dll only turns this path off.
// Signatures and enum values from SDL 3.4 (SDL_gamepad.h, SDL_init.h).
// =============================================================================

using SDL_JoystickID = uint32_t;
struct SDL_Gamepad;

namespace sdl_enum {
constexpr uint32_t INIT_GAMEPAD = 0x00002000u;

constexpr int AXIS_LEFTX = 0, AXIS_LEFTY = 1, AXIS_RIGHTX = 2, AXIS_RIGHTY = 3;
constexpr int AXIS_LEFT_TRIGGER = 4, AXIS_RIGHT_TRIGGER = 5;

constexpr int BUTTON_SOUTH = 0, BUTTON_EAST = 1, BUTTON_WEST = 2, BUTTON_NORTH = 3;
constexpr int BUTTON_BACK = 4, BUTTON_GUIDE = 5, BUTTON_START = 6;
constexpr int BUTTON_LEFT_STICK = 7, BUTTON_RIGHT_STICK = 8;
constexpr int BUTTON_LEFT_SHOULDER = 9, BUTTON_RIGHT_SHOULDER = 10;
constexpr int BUTTON_DPAD_UP = 11, BUTTON_DPAD_DOWN = 12, BUTTON_DPAD_LEFT = 13, BUTTON_DPAD_RIGHT = 14;
constexpr int BUTTON_MISC1 = 15;
constexpr int BUTTON_RIGHT_PADDLE1 = 16, BUTTON_LEFT_PADDLE1 = 17;
constexpr int BUTTON_RIGHT_PADDLE2 = 18, BUTTON_LEFT_PADDLE2 = 19;
constexpr int BUTTON_TOUCHPAD = 20;
}

static struct {
   bool (__cdecl* Init)(uint32_t flags);
   void (__cdecl* Quit)();
   bool (__cdecl* SetHint)(const char* name, const char* value);
   const char* (__cdecl* GetError)();
   void (__cdecl* free)(void* mem);
   SDL_JoystickID* (__cdecl* GetGamepads)(int* count);
   const char* (__cdecl* GetGamepadNameForID)(SDL_JoystickID id);
   int (__cdecl* GetGamepadPlayerIndexForID)(SDL_JoystickID id);
   uint16_t (__cdecl* GetGamepadVendorForID)(SDL_JoystickID id);
   uint16_t (__cdecl* GetGamepadProductForID)(SDL_JoystickID id);
   SDL_Gamepad* (__cdecl* OpenGamepad)(SDL_JoystickID id);
   void (__cdecl* CloseGamepad)(SDL_Gamepad* pad);
   bool (__cdecl* GamepadConnected)(SDL_Gamepad* pad);
   void (__cdecl* SetGamepadEventsEnabled)(bool enabled);
   void (__cdecl* SetJoystickEventsEnabled)(bool enabled);
   void (__cdecl* UpdateGamepads)();
   int16_t (__cdecl* GetGamepadAxis)(SDL_Gamepad* pad, int axis);
   bool (__cdecl* GetGamepadButton)(SDL_Gamepad* pad, int button);
   bool (__cdecl* RumbleGamepad)(SDL_Gamepad* pad, uint16_t low, uint16_t high, uint32_t ms);
} sdl;

static HMODULE s_sdlDll = nullptr;

template <typename T>
static bool load_fn(T& fn, const char* name)
{
   fn = (T)GetProcAddress(s_sdlDll, name);
   if (!fn) install_log("[Gamepad] SDL3.dll has no %s", name);
   return fn != nullptr;
}

static bool load_sdl()
{
   // SDL3.dll sits next to BF2GameExt.dll, never wherever the search path finds one.
   wchar_t path[MAX_PATH];
   HMODULE self = nullptr;
   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      (LPCWSTR)&load_sdl, &self);
   DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
   wchar_t* slash = wcsrchr(path, L'\\');
   if (!len || !slash) return false;
   wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"SDL3.dll");

   s_sdlDll = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
   if (!s_sdlDll) {
      install_log("[Gamepad] No SDL3.dll next to BF2GameExt.dll, pads use plain DirectInput");
      return false;
   }

   bool ok = true;
   ok &= load_fn(sdl.Init, "SDL_Init");
   ok &= load_fn(sdl.Quit, "SDL_Quit");
   ok &= load_fn(sdl.SetHint, "SDL_SetHint");
   ok &= load_fn(sdl.GetError, "SDL_GetError");
   ok &= load_fn(sdl.free, "SDL_free");
   ok &= load_fn(sdl.GetGamepads, "SDL_GetGamepads");
   ok &= load_fn(sdl.GetGamepadNameForID, "SDL_GetGamepadNameForID");
   ok &= load_fn(sdl.GetGamepadPlayerIndexForID, "SDL_GetGamepadPlayerIndexForID");
   ok &= load_fn(sdl.GetGamepadVendorForID, "SDL_GetGamepadVendorForID");
   ok &= load_fn(sdl.GetGamepadProductForID, "SDL_GetGamepadProductForID");
   ok &= load_fn(sdl.OpenGamepad, "SDL_OpenGamepad");
   ok &= load_fn(sdl.CloseGamepad, "SDL_CloseGamepad");
   ok &= load_fn(sdl.GamepadConnected, "SDL_GamepadConnected");
   ok &= load_fn(sdl.SetGamepadEventsEnabled, "SDL_SetGamepadEventsEnabled");
   ok &= load_fn(sdl.SetJoystickEventsEnabled, "SDL_SetJoystickEventsEnabled");
   ok &= load_fn(sdl.UpdateGamepads, "SDL_UpdateGamepads");
   ok &= load_fn(sdl.GetGamepadAxis, "SDL_GetGamepadAxis");
   ok &= load_fn(sdl.GetGamepadButton, "SDL_GetGamepadButton");
   ok &= load_fn(sdl.RumbleGamepad, "SDL_RumbleGamepad");
   if (!ok) {
      FreeLibrary(s_sdlDll);
      s_sdlDll = nullptr;
   }
   return ok;
}

// =============================================================================
// Pad slots: one per DirectInput pad the engine was given, fixed at startup
// like the engine's own device list. A pad that drops out leaves its slot
// empty, and the next pad SDL sees takes it.
// =============================================================================

static constexpr int      kMaxSlots   = 8;
static constexpr uint32_t kRumbleMs   = 500;    // refreshed while it runs
static constexpr DWORD    kRumbleIdle = 300;    // no request for this long stops it
static constexpr DWORD    kRescanMs   = 1000;

struct Slot {
   SDL_Gamepad*   pad;
   SDL_JoystickID id;
   uint16_t       vid, pid;
   char           name[64];
};

static Slot  s_slots[kMaxSlots] = {};
static int   s_slotCount = 0;
static bool  s_active    = false;
static DWORD s_lastScan  = 0;

static uint16_t s_rumbleLow = 0, s_rumbleHigh = 0;
static DWORD    s_rumbleSent = 0;
static DWORD    s_rumbleAsked = 0;

static void copy_name(char* out, size_t size, const char* utf8)
{
   size_t n = 0;
   for (; utf8 && *utf8 && n + 1 < size; ++utf8) {
      const unsigned char c = (unsigned char)*utf8;
      if (c < 0x80) out[n++] = (char)c;
      else if ((c & 0xC0) == 0xC0) out[n++] = '?';
   }
   out[n] = 0;
}

static bool slot_open(int i, SDL_JoystickID id)
{
   SDL_Gamepad* pad = sdl.OpenGamepad(id);
   if (!pad) {
      install_log("[Gamepad] Could not open SDL pad %u: %s", id, sdl.GetError());
      return false;
   }
   Slot& s = s_slots[i];
   s.pad = pad;
   s.id  = id;
   s.vid = sdl.GetGamepadVendorForID(id);
   s.pid = sdl.GetGamepadProductForID(id);
   copy_name(s.name, sizeof(s.name), sdl.GetGamepadNameForID(id));
   return true;
}

static bool slot_holds(SDL_JoystickID id)
{
   for (int i = 0; i < s_slotCount; ++i)
      if (s_slots[i].pad && s_slots[i].id == id) return true;
   return false;
}

static void fill_empty_slots()
{
   int count = 0;
   SDL_JoystickID* ids = sdl.GetGamepads(&count);
   if (!ids) return;
   for (int k = 0; k < count; ++k) {
      if (slot_holds(ids[k])) continue;
      for (int i = 0; i < s_slotCount; ++i) {
         if (s_slots[i].pad) continue;
         if (slot_open(i, ids[k]))
            install_log("[Gamepad] Pad %d connected: %s (%04X:%04X)", i + 1, s_slots[i].name,
                        s_slots[i].vid, s_slots[i].pid);
         break;
      }
   }
   sdl.free(ids);
}

static void send_rumble(DWORD now)
{
   if (s_slots[0].pad) sdl.RumbleGamepad(s_slots[0].pad, s_rumbleLow, s_rumbleHigh, kRumbleMs);
   s_rumbleSent = now;
}

// Once per frame, from the first pad's Poll.
static void gamepad_frame()
{
   sdl.UpdateGamepads();
   const DWORD now = GetTickCount();

   bool empty = false;
   for (int i = 0; i < s_slotCount; ++i) {
      Slot& s = s_slots[i];
      if (s.pad && !sdl.GamepadConnected(s.pad)) {
         install_log("[Gamepad] Pad %d disconnected: %s", i + 1, s.name);
         sdl.CloseGamepad(s.pad);
         s.pad = nullptr;
         s.id  = 0;
      }
      empty |= !s.pad;
   }
   if (empty && now - s_lastScan >= kRescanMs) {
      s_lastScan = now;
      fill_empty_slots();
   }

   // The rumble code asks every frame while the battle runs. Paused, behind an
   // objective popup or on the end screen it stops asking, so stop the motors
   // instead of holding the last value until the next battle.
   if (s_rumbleLow || s_rumbleHigh) {
      if (now - s_rumbleAsked > kRumbleIdle) {
         s_rumbleLow = s_rumbleHigh = 0;
         send_rumble(now);
      }
      else if (now - s_rumbleSent >= kRumbleMs / 2) {
         send_rumble(now);
      }
   }
}

// SDL comes up on the game's input thread, the first time the engine creates
// DirectInput, and only stays up if it found a pad.
static bool sdl_start()
{
   static bool tried = false;
   if (tried) return s_active;
   tried = true;
   if (!load_sdl()) return false;

   // The engine decides about focus itself; SDL never zeroes a pad behind it.
   sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
   if (!sdl.Init(sdl_enum::INIT_GAMEPAD)) {
      install_log("[Gamepad] SDL_Init failed: %s", sdl.GetError());
      return false;
   }
   // Nothing reads SDL's event queue, so do not fill it.
   sdl.SetGamepadEventsEnabled(false);
   sdl.SetJoystickEventsEnabled(false);
   sdl.UpdateGamepads();

   int count = 0;
   SDL_JoystickID* ids = sdl.GetGamepads(&count);
   if (ids) {
      // XInput pads keep their player light order; the rest follow.
      for (int a = 1; a < count; ++a) {
         for (int b = a; b > 0; --b) {
            int pa = sdl.GetGamepadPlayerIndexForID(ids[b - 1]);
            int pb = sdl.GetGamepadPlayerIndexForID(ids[b]);
            if (pa < 0) pa = 0x7FFF;
            if (pb < 0) pb = 0x7FFF;
            if (pa <= pb) break;
            const SDL_JoystickID t = ids[b - 1];
            ids[b - 1] = ids[b];
            ids[b] = t;
         }
      }
      for (int k = 0; k < count && s_slotCount < kMaxSlots; ++k)
         if (slot_open(s_slotCount, ids[k])) ++s_slotCount;
      sdl.free(ids);
   }

   if (!s_slotCount) {
      install_log("[Gamepad] SDL found no pad at startup, pads use plain DirectInput");
      sdl.Quit();
      return false;
   }
   for (int i = 0; i < s_slotCount; ++i)
      install_log("[Gamepad] Pad %d: %s (%04X:%04X)", i + 1, s_slots[i].name, s_slots[i].vid, s_slots[i].pid);
   s_lastScan = GetTickCount();
   s_active = true;
   return true;
}

static void sdl_stop()
{
   if (!s_active) return;
   s_active = false;
   for (int i = 0; i < s_slotCount; ++i) {
      if (!s_slots[i].pad) continue;
      sdl.RumbleGamepad(s_slots[i].pad, 0, 0, 0);
      sdl.CloseGamepad(s_slots[i].pad);
      s_slots[i].pad = nullptr;
   }
   sdl.Quit();
}

bool gamepad_sdl_active() { return s_active; }

bool gamepad_sdl_rumble(uint16_t low, uint16_t high)
{
   if (!s_active) return false;
   s_rumbleAsked = GetTickCount();
   if (low != s_rumbleLow || high != s_rumbleHigh) {
      s_rumbleLow  = low;
      s_rumbleHigh = high;
      send_rumble(GetTickCount());
   }
   return true;
}

// =============================================================================
// The DirectInput pad the engine sees for a slot
// =============================================================================

static const GUID kIID_IUnknown            = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIID_IDirectInput8A       = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
static const GUID kIID_IDirectInputDevice8A = {0x54D41080, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};
static const GUID kGUID_XAxis  = {0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kGUID_YAxis  = {0xA36D02E1, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kGUID_ZAxis  = {0xA36D02E2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kGUID_RxAxis = {0xA36D02F4, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kGUID_RyAxis = {0xA36D02F5, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kGUID_RzAxis = {0xA36D02E3, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kGUID_Button = {0xA36D02F0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kGUID_POV    = {0xA36D02F2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// Instance GUIDs of the slot pads: Data1 low byte is the slot.
static const GUID kSlotGuidBase = {0xB2F2E000, 0x5344, 0x4C33, {'G', 'a', 'm', 'e', 'E', 'x', 't', 'P'}};
static const GUID kProductGuid  = {0xB2F2E0FF, 0x5344, 0x4C33, {'G', 'a', 'm', 'e', 'E', 'x', 't', 'P'}};

static GUID slot_guid(int slot)
{
   GUID g = kSlotGuidBase;
   g.Data1 += (unsigned long)slot;
   return g;
}

static int slot_from_guid(REFGUID g)
{
   if (g.Data2 != kSlotGuidBase.Data2 || g.Data3 != kSlotGuidBase.Data3 ||
       memcmp(g.Data4, kSlotGuidBase.Data4, sizeof(g.Data4)) != 0)
      return -1;
   const unsigned long slot = g.Data1 - kSlotGuidBase.Data1;
   return slot < (unsigned long)s_slotCount ? (int)slot : -1;
}

static void fill_instance(int slot, DIDEVICEINSTANCEA* di)
{
   const DWORD size = di->dwSize;
   memset(di, 0, size);
   di->dwSize       = size;
   di->guidInstance = slot_guid(slot);
   di->guidProduct  = kProductGuid;
   di->dwDevType    = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
   sprintf_s(di->tszInstanceName, "BF2GameExt pad %d", slot + 1);
   sprintf_s(di->tszProductName, "%s", s_slots[slot].name[0] ? s_slots[slot].name : "Gamepad");
}

static constexpr int kAxes    = 6;   // X Y Z RX RY RZ
static constexpr int kButtons = 17;

struct PadObject {
   const GUID* type;
   DWORD       ofs;
   DWORD       dwType;
   const char* name;
};

static const PadObject* pad_objects(int* count)
{
   static PadObject objects[kAxes + 1 + kButtons];
   static char      buttonNames[kButtons][12];
   static bool      built = false;
   if (!built) {
      built = true;
      const GUID* axisTypes[kAxes] = {&kGUID_XAxis, &kGUID_YAxis, &kGUID_ZAxis,
                                      &kGUID_RxAxis, &kGUID_RyAxis, &kGUID_RzAxis};
      static const char* axisNames[kAxes] = {"Left stick X", "Left stick Y", "Left trigger",
                                             "Right stick X", "Right stick Y", "Right trigger"};
      int n = 0;
      for (int a = 0; a < kAxes; ++a)
         objects[n++] = {axisTypes[a], (DWORD)(a * sizeof(LONG)), (DWORD)(DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(a)), axisNames[a]};
      objects[n++] = {&kGUID_POV, (DWORD)offsetof(DIJOYSTATE2, rgdwPOV), (DWORD)(DIDFT_POV | DIDFT_MAKEINSTANCE(0)), "D-pad"};
      for (int b = 0; b < kButtons; ++b) {
         sprintf_s(buttonNames[b], "Button %d", b);
         objects[n++] = {&kGUID_Button, (DWORD)(offsetof(DIJOYSTATE2, rgbButtons) + b),
                         (DWORD)(DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(b)), buttonNames[b]};
      }
   }
   *count = kAxes + 1 + kButtons;
   return objects;
}

static void fill_object(const PadObject& o, DIDEVICEOBJECTINSTANCEA* out)
{
   const DWORD size = out->dwSize;
   memset(out, 0, size);
   out->dwSize   = size;
   out->guidType = *o.type;
   out->dwOfs    = o.ofs;
   out->dwType   = o.dwType;
   out->dwFlags  = (o.dwType & DIDFT_AXIS) ? DIDOI_ASPECTPOSITION : 0;
   strcpy_s(out->tszName, o.name);
}

static bool foreground_is_ours()
{
   DWORD pid = 0;
   GetWindowThreadProcessId(GetForegroundWindow(), &pid);
   return pid == GetCurrentProcessId();
}

class SlotPad final : public IDirectInputDevice8A {
public:
   explicit SlotPad(int slot) : m_slot(slot)
   {
      for (Axis& a : m_axes) a = {0, 65535, 0, 10000};
   }

   // IUnknown
   HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
   {
      if (!out) return E_POINTER;
      if (IsEqualIID(riid, kIID_IUnknown) || IsEqualIID(riid, kIID_IDirectInputDevice8A)) {
         *out = this;
         AddRef();
         return S_OK;
      }
      *out = nullptr;
      return E_NOINTERFACE;
   }
   ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
   ULONG STDMETHODCALLTYPE Release() override
   {
      const ULONG refs = InterlockedDecrement(&m_refs);
      if (!refs) delete this;
      return refs;
   }

   // What the engine uses
   HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override
   {
      if (!caps || caps->dwSize < sizeof(DIDEVCAPS_DX3)) return DIERR_INVALIDPARAM;
      const DWORD size = caps->dwSize;
      memset(caps, 0, size);
      caps->dwSize    = size;
      caps->dwFlags   = DIDC_ATTACHED;
      caps->dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
      caps->dwAxes    = kAxes;
      caps->dwButtons = kButtons;
      caps->dwPOVs    = 1;
      return DI_OK;
   }

   HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) override
   {
      if (!cb) return DIERR_INVALIDPARAM;
      int count = 0;
      const PadObject* objects = pad_objects(&count);
      const DWORD wanted = flags & 0xFF;
      for (int i = 0; i < count; ++i) {
         if (wanted && !(objects[i].dwType & wanted)) continue;
         DIDEVICEOBJECTINSTANCEA info;
         info.dwSize = sizeof(info);
         fill_object(objects[i], &info);
         if (cb(&info, ref) == DIENUM_STOP) break;
      }
      return DI_OK;
   }

   HRESULT STDMETHODCALLTYPE GetProperty(REFGUID prop, LPDIPROPHEADER hdr) override
   {
      if (!hdr) return DIERR_INVALIDPARAM;
      const int axis = axis_for(hdr);
      if (axis < 0) return DIERR_UNSUPPORTED;
      const Axis& a = m_axes[axis];
      switch (prop_id(prop)) {
      case kPropRange:
         ((DIPROPRANGE*)hdr)->lMin = a.min;
         ((DIPROPRANGE*)hdr)->lMax = a.max;
         return DI_OK;
      case kPropDeadzone:   ((DIPROPDWORD*)hdr)->dwData = a.deadzone;   return DI_OK;
      case kPropSaturation: ((DIPROPDWORD*)hdr)->dwData = a.saturation; return DI_OK;
      }
      return DIERR_UNSUPPORTED;
   }

   HRESULT STDMETHODCALLTYPE SetProperty(REFGUID prop, LPCDIPROPHEADER hdr) override
   {
      if (!hdr) return DIERR_INVALIDPARAM;
      const uintptr_t id = prop_id(prop);
      if (id != kPropRange && id != kPropDeadzone && id != kPropSaturation)
         return DI_OK;   // buffer size, autocenter and the like mean nothing here

      const int only = hdr->dwHow == DIPH_DEVICE ? -1 : axis_for(hdr);
      if (hdr->dwHow != DIPH_DEVICE && only < 0) return DIERR_OBJECTNOTFOUND;
      for (int i = 0; i < kAxes; ++i) {
         if (only >= 0 && i != only) continue;
         Axis& a = m_axes[i];
         if (id == kPropRange) {
            const DIPROPRANGE* r = (const DIPROPRANGE*)hdr;
            if (r->lMin >= r->lMax) return DIERR_INVALIDPARAM;
            a.min = r->lMin;
            a.max = r->lMax;
         }
         else {
            const DWORD v = ((const DIPROPDWORD*)hdr)->dwData;
            if (v > 10000) return DIERR_INVALIDPARAM;
            (id == kPropDeadzone ? a.deadzone : a.saturation) = v;
         }
      }
      return DI_OK;
   }

   HRESULT STDMETHODCALLTYPE Acquire() override
   {
      // DI_OK even when already acquired: the engine reads anything else as lost.
      m_acquired = true;
      return DI_OK;
   }

   HRESULT STDMETHODCALLTYPE Unacquire() override
   {
      if (!m_acquired) return DI_NOEFFECT;
      m_acquired = false;
      return DI_OK;
   }

   HRESULT STDMETHODCALLTYPE Poll() override
   {
      if (!m_acquired) return DIERR_NOTACQUIRED;
      if (m_slot == 0 && s_active) gamepad_frame();
      return DI_OK;
   }

   HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID data) override
   {
      if (!m_acquired) return DIERR_NOTACQUIRED;
      if (size != sizeof(DIJOYSTATE2) || !data) return DIERR_INVALIDPARAM;
      fill_state(*(DIJOYSTATE2*)data);
      return DI_OK;
   }

   HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT format) override
   {
      // Only the c_dfDIJoystick2 layout the engine uses.
      if (!format || format->dwDataSize != sizeof(DIJOYSTATE2)) return DIERR_INVALIDPARAM;
      return DI_OK;
   }

   HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD) override { return DI_OK; }

   HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA out, DWORD obj, DWORD how) override
   {
      if (!out) return DIERR_INVALIDPARAM;
      int count = 0;
      const PadObject* objects = pad_objects(&count);
      for (int i = 0; i < count; ++i) {
         const PadObject& o = objects[i];
         const bool match = how == DIPH_BYOFFSET ? o.ofs == obj
                          : how == DIPH_BYID     ? (o.dwType & 0x00FFFFFF) == (obj & 0x00FFFFFF)
                                                 : false;
         if (!match) continue;
         fill_object(o, out);
         return DI_OK;
      }
      return DIERR_OBJECTNOTFOUND;
   }

   HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA out) override
   {
      if (!out || out->dwSize < sizeof(DIDEVICEINSTANCE_DX3A)) return DIERR_INVALIDPARAM;
      fill_instance(m_slot, out);
      return DI_OK;
   }

   // Nothing else exists on a slot pad
   HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_NOTBUFFERED; }
   HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override { return DIERR_UNSUPPORTED; }
   HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return DI_OK; }
   HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
   HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN) override { return DIERR_UNSUPPORTED; }
   HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override { return DI_OK; }
   HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA, REFGUID) override { return DIERR_UNSUPPORTED; }
   HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD) override { return DIERR_UNSUPPORTED; }
   HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD) override { return DIERR_UNSUPPORTED; }
   HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; }
   HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
   HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_UNSUPPORTED; }
   HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; }
   HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
   HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
   HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
   HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) override { return DIERR_UNSUPPORTED; }

private:
   // DIPROP_* are MAKEDIPROP(n): the GUID reference's address is the id.
   static constexpr uintptr_t kPropRange = 4, kPropDeadzone = 5, kPropSaturation = 6;
   static uintptr_t prop_id(REFGUID prop) { return (uintptr_t)&prop; }

   struct Axis {
      LONG  min, max;
      DWORD deadzone, saturation;   // 0..10000 of the half range, as DirectInput
   };

   // The axis a property header names, -1 for none.
   static int axis_for(LPCDIPROPHEADER hdr)
   {
      if (hdr->dwHow == DIPH_BYOFFSET) {
         return hdr->dwObj % sizeof(LONG) == 0 && hdr->dwObj / sizeof(LONG) < kAxes
                   ? (int)(hdr->dwObj / sizeof(LONG)) : -1;
      }
      if (hdr->dwHow == DIPH_BYID && (hdr->dwObj & DIDFT_AXIS)) {
         const int axis = DIDFT_GETINSTANCE(hdr->dwObj);
         return axis < kAxes ? axis : -1;
      }
      return -1;
   }

   // n is -1..1 for a stick and 0..1 for a trigger, which rests at the centre.
   LONG axis_value(int axis, float n) const
   {
      const Axis& a = m_axes[axis];
      const float dz  = a.deadzone / 10000.0f;
      const float sat = a.saturation / 10000.0f;
      float mag = fabsf(n);
      if (mag <= dz)        mag = 0.0f;
      else if (mag >= sat)  mag = 1.0f;
      else                  mag = (mag - dz) / (sat - dz);
      const float centre = (a.min + (float)a.max) * 0.5f;
      const float half   = ((float)a.max - a.min) * 0.5f;
      return (LONG)lroundf(centre + (n < 0.0f ? -mag : mag) * half);
   }

   void fill_state(DIJOYSTATE2& st) const
   {
      memset(&st, 0, sizeof(st));
      for (DWORD& pov : st.rgdwPOV) pov = 0xFFFFFFFF;

      SDL_Gamepad* pad = s_active ? s_slots[m_slot].pad : nullptr;
      // Mirrors DirectInput's foreground cooperative level.
      if (!pad || !foreground_is_ours()) {
         st.lX  = axis_value(0, 0.0f);
         st.lY  = axis_value(1, 0.0f);
         st.lZ  = axis_value(2, 0.0f);
         st.lRx = axis_value(3, 0.0f);
         st.lRy = axis_value(4, 0.0f);
         st.lRz = axis_value(5, 0.0f);
         return;
      }

      using namespace sdl_enum;
      auto stick = [&](int axis) {
         const float v = sdl.GetGamepadAxis(pad, axis) / 32767.0f;
         return v < -1.0f ? -1.0f : v;
      };
      st.lX  = axis_value(0, stick(AXIS_LEFTX));
      st.lY  = axis_value(1, stick(AXIS_LEFTY));
      st.lZ  = axis_value(2, stick(AXIS_LEFT_TRIGGER));
      st.lRx = axis_value(3, stick(AXIS_RIGHTX));
      st.lRy = axis_value(4, stick(AXIS_RIGHTY));
      st.lRz = axis_value(5, stick(AXIS_RIGHT_TRIGGER));

      static const int kButtonMap[kButtons] = {
         BUTTON_SOUTH, BUTTON_EAST, BUTTON_WEST, BUTTON_NORTH,
         BUTTON_LEFT_SHOULDER, BUTTON_RIGHT_SHOULDER, BUTTON_BACK, BUTTON_START,
         BUTTON_LEFT_STICK, BUTTON_RIGHT_STICK, BUTTON_GUIDE, BUTTON_MISC1,
         BUTTON_RIGHT_PADDLE1, BUTTON_LEFT_PADDLE1, BUTTON_RIGHT_PADDLE2, BUTTON_LEFT_PADDLE2,
         BUTTON_TOUCHPAD,
      };
      for (int b = 0; b < kButtons; ++b)
         st.rgbButtons[b] = sdl.GetGamepadButton(pad, kButtonMap[b]) ? 0x80 : 0;

      const int x = sdl.GetGamepadButton(pad, BUTTON_DPAD_RIGHT) - sdl.GetGamepadButton(pad, BUTTON_DPAD_LEFT);
      const int y = sdl.GetGamepadButton(pad, BUTTON_DPAD_DOWN) - sdl.GetGamepadButton(pad, BUTTON_DPAD_UP);
      if (x || y) {
         // Clockwise from up, in hundredths of a degree.
         static const DWORD kAngles[3][3] = {
            {31500,     0,  4500},   // up row:   left, centre, right
            {27000,     0,  9000},   // middle row (centre unused)
            {22500, 18000, 13500},   // down row
         };
         st.rgdwPOV[0] = kAngles[y + 1][x + 1];
      }
   }

   LONG m_refs = 1;
   int  m_slot;
   bool m_acquired = false;
   Axis m_axes[kAxes];
};

// =============================================================================
// IDirectInput8A wrapper: lists the slot pads first and hides the real devices
// SDL already reads
// =============================================================================

// VID/PID of a HID device, from the "PIDVID" form DirectInput gives guidProduct.
static bool product_vid_pid(const GUID& g, uint16_t* vid, uint16_t* pid)
{
   static const BYTE kPidVid[6] = {'P', 'I', 'D', 'V', 'I', 'D'};
   if (memcmp(g.Data4 + 2, kPidVid, sizeof(kPidVid)) != 0) return false;
   *vid = LOWORD(g.Data1);
   *pid = HIWORD(g.Data1);
   return true;
}

// An XInput pad has "IG_" in its device path.
static bool is_xinput(IDirectInput8A* real, LPCDIDEVICEINSTANCEA inst)
{
   IDirectInputDevice8A* dev = nullptr;
   if (FAILED(real->CreateDevice(inst->guidInstance, &dev, nullptr)) || !dev) return false;
   DIPROPGUIDANDPATH path = {};
   path.diph.dwSize       = sizeof(path);
   path.diph.dwHeaderSize = sizeof(DIPROPHEADER);
   path.diph.dwHow        = DIPH_DEVICE;
   const bool xinput = SUCCEEDED(dev->GetProperty(DIPROP_GUIDANDPATH, &path.diph)) &&
                       (wcsstr(path.wszPath, L"IG_") || wcsstr(path.wszPath, L"ig_"));
   dev->Release();
   return xinput;
}

static bool sdl_reads(IDirectInput8A* real, LPCDIDEVICEINSTANCEA inst)
{
   uint16_t vid, pid;
   if (product_vid_pid(inst->guidProduct, &vid, &pid)) {
      for (int i = 0; i < s_slotCount; ++i)
         if (s_slots[i].vid == vid && s_slots[i].pid == pid) return true;
   }

   // An XInput pad is always one of SDL's, even when the ids SDL reports for
   // it differ from DirectInput's.
   return is_xinput(real, inst);
}

struct XInputProbe {
   IDirectInput8A* real;
   bool            found;
};

static BOOL CALLBACK probe_xinput(LPCDIDEVICEINSTANCEA inst, LPVOID ctx)
{
   XInputProbe* p = (XInputProbe*)ctx;
   p->found = is_xinput(p->real, inst);
   return p->found ? DIENUM_STOP : DIENUM_CONTINUE;
}

static bool xinput_attached(IDirectInput8A* real)
{
   XInputProbe probe = {real, false};
   real->EnumDevices(DI8DEVCLASS_GAMECTRL, probe_xinput, &probe, DIEDFL_ATTACHEDONLY);
   return probe.found;
}

struct EnumFilter {
   IDirectInput8A*         real;
   LPDIENUMDEVICESCALLBACKA cb;
   LPVOID                  ref;
};

static BOOL CALLBACK enum_real_device(LPCDIDEVICEINSTANCEA inst, LPVOID ctx)
{
   const EnumFilter* f = (const EnumFilter*)ctx;
   if (sdl_reads(f->real, inst)) {
      install_log("[Gamepad] Hid \"%s\" from the game, SDL reads it", inst->tszProductName);
      return DIENUM_CONTINUE;
   }
   return f->cb(inst, f->ref);
}

class DirectInputWrapper final : public IDirectInput8A {
public:
   explicit DirectInputWrapper(IDirectInput8A* real) : m_real(real) {}

   HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
   {
      if (!out) return E_POINTER;
      if (IsEqualIID(riid, kIID_IUnknown) || IsEqualIID(riid, kIID_IDirectInput8A)) {
         *out = this;
         AddRef();
         return S_OK;
      }
      return m_real->QueryInterface(riid, out);
   }
   ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
   ULONG STDMETHODCALLTYPE Release() override
   {
      const ULONG refs = InterlockedDecrement(&m_refs);
      if (!refs) {
         m_real->Release();
         sdl_stop();
         delete this;
      }
      return refs;
   }

   HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) override
   {
      const int slot = slot_from_guid(guid);
      if (slot < 0) return m_real->CreateDevice(guid, out, outer);
      if (!out) return DIERR_INVALIDPARAM;
      *out = new SlotPad(slot);
      return DI_OK;
   }

   HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags) override
   {
      if (!cb) return DIERR_INVALIDPARAM;
      const DWORD kind = type & 0xFF;
      const bool pads = kind == DI8DEVCLASS_ALL || kind == DI8DEVCLASS_GAMECTRL || kind == DI8DEVTYPE_GAMEPAD;
      if (!pads || !s_active) return m_real->EnumDevices(type, cb, ref, flags);

      for (int i = 0; i < s_slotCount; ++i) {
         DIDEVICEINSTANCEA di;
         di.dwSize = sizeof(di);
         fill_instance(i, &di);
         if (cb(&di, ref) == DIENUM_STOP) return DI_OK;
      }
      EnumFilter filter = {m_real, cb, ref};
      return m_real->EnumDevices(type, enum_real_device, &filter, flags);
   }

   HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override
   {
      return slot_from_guid(guid) >= 0 ? DI_OK : m_real->GetDeviceStatus(guid);
   }
   HRESULT STDMETHODCALLTYPE RunControlPanel(HWND wnd, DWORD flags) override { return m_real->RunControlPanel(wnd, flags); }
   HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE inst, DWORD version) override { return m_real->Initialize(inst, version); }
   HRESULT STDMETHODCALLTYPE FindDevice(REFGUID cls, LPCSTR name, LPGUID out) override { return m_real->FindDevice(cls, name, out); }
   HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR user, LPDIACTIONFORMATA format,
                                                    LPDIENUMDEVICESBYSEMANTICSCBA cb, LPVOID ref, DWORD flags) override
   {
      return m_real->EnumDevicesBySemantics(user, format, cb, ref, flags);
   }
   HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK cb, LPDICONFIGUREDEVICESPARAMSA params,
                                              DWORD flags, LPVOID ref) override
   {
      return m_real->ConfigureDevices(cb, params, flags, ref);
   }

private:
   LONG            m_refs = 1;
   IDirectInput8A* m_real;
};

// =============================================================================
// The function the game's DirectInput8Create import resolves to
//
// Shader Patch replaces that import with its own hook, which never calls what
// it replaced, so rewriting the import is lost whenever Shader Patch loads
// after us. Its hook ends in this same function, so detouring the function
// itself reaches the game's DirectInput either way, underneath Shader Patch.
// =============================================================================

using fn_di8_create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static fn_di8_create_t s_origCreate = nullptr;
static void (*s_layoutListener)(bool xbox) = nullptr;

void gamepad_set_layout_listener(void (*fn)(bool xbox)) { s_layoutListener = fn; }

static bool called_by_sdl(void* ret)
{
   HMODULE caller = nullptr;
   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      (LPCWSTR)ret, &caller);
   return s_sdlDll && caller == s_sdlDll;
}

static HRESULT WINAPI hooked_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
   void* const ret = _ReturnAddress();
   const HRESULT hr = s_origCreate(inst, version, riid, out, outer);
   if (FAILED(hr) || !out || !*out || outer || !IsEqualIID(riid, kIID_IDirectInput8A)) return hr;
   // SDL's own DirectInput backend gets the real thing.
   if (called_by_sdl(ret)) return hr;
   if (!g_controllerEnabled) return hr;
   IDirectInput8A* const real = (IDirectInput8A*)*out;
   const bool sdl = sdl_start();
   if (sdl) *out = static_cast<IDirectInput8A*>(new DirectInputWrapper(real));
   if (s_layoutListener) s_layoutListener(sdl || xinput_attached(real));
   return hr;
}

// The exe's import address table entry for dll!fn.
static void** find_import(uintptr_t exe_base, const char* dll, const char* fn)
{
   const auto* dos = (const IMAGE_DOS_HEADER*)exe_base;
   const auto* nt  = (const IMAGE_NT_HEADERS32*)(exe_base + dos->e_lfanew);
   const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
   if (!dir.VirtualAddress) return nullptr;

   HMODULE module = GetModuleHandleA(dll);
   void* resolved = module ? (void*)GetProcAddress(module, fn) : nullptr;

   for (auto* imp = (const IMAGE_IMPORT_DESCRIPTOR*)(exe_base + dir.VirtualAddress); imp->Name; ++imp) {
      if (_stricmp((const char*)(exe_base + imp->Name), dll) != 0) continue;
      auto* iat = (IMAGE_THUNK_DATA32*)(exe_base + imp->FirstThunk);
      if (imp->OriginalFirstThunk) {
         auto* names = (const IMAGE_THUNK_DATA32*)(exe_base + imp->OriginalFirstThunk);
         for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            const auto* byName = (const IMAGE_IMPORT_BY_NAME*)(exe_base + names->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, fn) == 0) return (void**)&iat->u1.Function;
         }
      }
      else if (resolved) {
         for (; iat->u1.Function; ++iat)
            if ((void*)(uintptr_t)iat->u1.Function == resolved) return (void**)&iat->u1.Function;
      }
   }
   return nullptr;
}

void gamepad_sdl_install(uintptr_t exe_base)
{
   if (!g_controllerEnabled) return;
   void** entry = find_import(exe_base, "dinput8.dll", "DirectInput8Create");
   if (!entry || !*entry) {
      install_log("[Gamepad] DirectInput8Create import not found, pads use plain DirectInput");
      return;
   }

   char owner[MAX_PATH] = "?";
   HMODULE module = nullptr;
   if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)*entry, &module))
      GetModuleFileNameA(module, owner, MAX_PATH);

   s_origCreate = (fn_di8_create_t)*entry;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)s_origCreate, hooked_DirectInput8Create);
   if (DetourTransactionCommit() != NO_ERROR) {
      s_origCreate = nullptr;
      install_log("[Gamepad] Could not hook DirectInput8Create in %s, pads use plain DirectInput", owner);
      return;
   }
   install_log("[Gamepad] DirectInput8Create hooked in %s, SDL pads start with the game's input", owner);
}
