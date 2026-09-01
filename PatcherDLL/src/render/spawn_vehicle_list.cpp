#include "pch.h"
#include "spawn_vehicle_list.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

// =============================================================================
// See the header for what was cut and why no .hud change is needed.  This file
// documents the layouts and calling conventions it relies on.
//
// WHY WE POLL Update RATHER THAN HOOK THE POST-CHANGE EDGE
//
// The obvious hook is SpawnDisplay::SetCommandPost, the sole writer of
// mCommandPost, reached from SelectPost -- which Update calls on exactly the
// "highlighted post changed" edge:
//
//     cp = GetCommandPost(this);
//     if (cp != this->mCommandPost) SelectPost(this, cp);
//
// That works, but it is the wrong edge on its own.  The post is already selected
// while the player is still on the side-select screen, so the list appears there;
// and once the side is accepted the post usually has NOT changed, so no further
// SetCommandPost arrives and the list would never (re)appear on the unit screen
// where it belongs.  Polling Update covers the post changing, the mode changing
// and the post being captured out from under the player, in one place.
//
// SpawnDisplay::Update -- bool __thiscall(this, float dt), vtable slot 1.
// The epilogue really is RET 4 (Steam 0x0042AC89:
// `pop edi / mov al,1 / pop esi / mov esp,ebp / pop ebp / ret 4`), read off the
// disassembly rather than trusted from the decompiler's signature.
//
// mMode == 0 is side select.  The engine draws the same line: SelectPost plays
// its spawn-point-change sound only `if (mMode != 0)`, i.e. it already treats
// "not side select" as the condition for a post change being user-visible
// feedback.  We reuse that gate rather than inventing a mode whitelist.
//
// PblList<T> -- the head IS the global, and _head sits at +0:
//     +0x00  PblList* _pList     (self)
//     +0x04  Node*    _pNext
//     +0x08  Node*    _pPrev
//     +0x0C  T*       _pObject
//     +0x10  int      _iCount
// Walk from head->_pNext until the node address equals the head again.
//
// VehicleSpawn -- 368 bytes, IDENTICAL on modtools, Steam and GOG.  Verified
// field by field out of the constructors (modtools 0x00664C50, Steam 0x0066E820);
// each build's destructor confirms the list node at the same place.
//     +0x1C  Node          m_ListNode
//     +0x74  CommandPost*  mCommandPost   (a plain pointer; mNameId follows at +0x78)
//     +0x90  EntityClass*  mSpawnClass[8]  per team
//     +0xB0  EntityClass*  mFlyerClass[8]  per team
//
// CommandPost -- only the early fields are build-invariant, which is all we need:
//     +0x2C  GameObject*  mObject
//     +0x30  int          mSavedHandleId
// Read out of SetCommandPost itself on all three builds (modtools 0x0068A8D8
// `mov edx,[eax+0x2c]`, Steam 0x0042B9ED / GOG 0x0042B9AD `mov ecx,[edx+0x2c]`),
// which also shows the staleness check against GameObject +0x204.
//
// GameObject::mTeam is a 4-bit SIGNED bitfield at +0x234 -- the same extraction
// VehicleSpawn::SetProperty performs when it validates a spawn's command post.
//
// EntityClass differs between the debug and release builds because the release
// build drops the 32-byte debug filename:
//     modtools     mLabel wchar_t* +0x40, mFilename char[32] +0x20
//     Steam / GOG  mLabel wchar_t* +0x20, name hash        +0x18
// Both read straight out of SpawnDisplay::SetSlotInfo, which picks the label and
// otherwise falls back exactly as reproduced below.
//
// HUD::Event is an 8-byte POD { EventClass* mClass; union Data mData; } whose
// constructor only stores the two words (modtools 0x006AD4A0 is literally two
// movs) and whose destructor is empty, so we build one inline rather than
// calling the engine.  Event::Send forwards to EventClass::Send, which walks the
// handler list synchronously -- the string is consumed before Send returns.
// HUD::ElementText::EventText casts a type_String payload straight to wchar_t*
// and hands it to RedTextElement::SetText, which copies; a null payload clears
// the element instead of faulting.
//
// The per-player event arrays are indexed by the int at SpawnDisplay +0x2000
// with a stride of 0xCA dwords, on all three builds.  Read out of
// SpawnDisplay::Show, which fires spawnDisplay.enable through the same array.
// PC only ever runs one viewport, so this is always index 0 in practice.
// =============================================================================

// [Features] SpawnVehicleList
bool g_spawnVehicleListEnabled = true;

// ---- layout constants -------------------------------------------------------

// PblList / Node
static constexpr int kNode_Next            = 0x04;
static constexpr int kNode_Object          = 0x0C;

// VehicleSpawn
static constexpr int kVS_CommandPost       = 0x74;
static constexpr int kVS_SpawnClass        = 0x90;
static constexpr int kVS_FlyerClass        = 0xB0;

// CommandPost
static constexpr int kCP_Object            = 0x2C;
static constexpr int kCP_SavedHandleId     = 0x30;

// GameObject
static constexpr int kGO_HandleId          = 0x204;
static constexpr int kGO_TeamBitfield      = 0x234;

// SpawnDisplay
static constexpr int kSD_EventIndex        = 0x2000;

// SpawnDisplay::Mode.  Only the side-select value matters to us.
static constexpr int kMode_SideSelect      = 0;

// HUD event array stride, in dwords, between consecutive local players.
static constexpr int kEventPlayerStride    = 0xCA;

static constexpr int kMaxTeams             = 8;
static constexpr int kMaxLocalPlayers      = 4;
static constexpr int kMaxShown             = 32;   // matches the engine's own cap
static constexpr int kTextChars            = 256;  // matches SpawnVehicleList::mVehicleListStr

// A corrupt or mid-construction list must not spin the update thread forever.
// A busy map runs a few dozen vehicle spawns; this is a wide safety margin.
static constexpr int kMaxListWalk          = 4096;

// ---- resolved engine entry points -------------------------------------------

// __thiscall(this, float) reached as __fastcall: ECX = this, EDX unused, dt on
// the stack (floats never go in registers), callee cleans (RET 4).
using fn_update_t = bool(__fastcall*)(void* ecx, void* edx, float dt);

// HUD::Event::Send -- __thiscall(Event*), no stack args, plain RET.
using fn_event_send_t = void(__fastcall*)(void* ecx, void* edx);

static fn_update_t     original_Update     = nullptr;
static fn_event_send_t g_eventSend         = nullptr;
static void**          g_vehicleSpawnList  = nullptr;
static void**          g_vehicleEventArray = nullptr;
static bool            g_installed         = false;

// Cached per-build shape, resolved once at install.
static int g_clsLabelOff    = 0;
static int g_clsFilenameOff = 0;  // 0 = this build has no debug filename
static int g_clsHashOff     = 0;  // 0 = this build has no name hash
static int g_sdModeOff      = 0;
static int g_sdPostOff      = 0;

// What we last pushed to each local player's element, so a frame that changes
// nothing costs three compares and no work.
struct SentState {
   const void* post;
   int         team;
   bool        sent;
};
static SentState g_sent[kMaxLocalPlayers];

// The event carries a bare wchar_t*.  Dispatch is synchronous, but a static
// buffer costs nothing and removes any question about the pointer outliving us.
static wchar_t g_text[kTextChars];

// install_log() is the ONLY logger that may run during install: dllmain holds the
// exe sections at PAGE_READWRITE (non-executable) then, so calling the engine's
// own logger there is an EXEC access violation on DEP builds.  Same split as
// hud_weapon_icon_fix.cpp.
static void install_log(const char* fmt, ...)
{
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

// ---------------------------------------------------------------------------
// Reading the engine
// ---------------------------------------------------------------------------

// Resolve a command post's GameObject through its handle, rejecting a stale one
// the same way the engine does.
static const uint8_t* cp_object(const uint8_t* cp)
{
   if (!cp) return nullptr;
   const uint8_t* obj = *(const uint8_t* const*)(cp + kCP_Object);
   if (!obj) return nullptr;
   if (*(const int*)(obj + kGO_HandleId) != *(const int*)(cp + kCP_SavedHandleId))
      return nullptr;
   return obj;
}

// GameObject::mTeam, a 4-bit signed bitfield.  Returns -1 when it is not a team
// index we can use as an array subscript.
static int object_team(const uint8_t* obj)
{
   if (!obj) return -1;
   const int32_t raw  = *(const int32_t*)(obj + kGO_TeamBitfield);
   const int     team = (int)((raw << 28) >> 28);
   return (team >= 0 && team < kMaxTeams) ? team : -1;
}

// ---------------------------------------------------------------------------
// Building the list
// ---------------------------------------------------------------------------

static void append_class_name(const uint8_t* cls)
{
   wchar_t        scratch[64];
   const wchar_t* name = nullptr;

   const wchar_t* label = *(const wchar_t* const*)(cls + g_clsLabelOff);
   if (label) {
      name = label;
   }
   else if (g_clsFilenameOff) {
      // Debug build: a NUL-terminated char[32] sitting inside the class.
      const char* fn = (const char*)(cls + g_clsFilenameOff);
      size_t      n  = 0;
      while (n < 63 && fn[n]) { scratch[n] = (wchar_t)(unsigned char)fn[n]; ++n; }
      scratch[n] = L'\0';
      name = scratch;
   }
   else if (g_clsHashOff) {
      // Release build: no filename survives, so show the hash the same way the
      // engine's own unit-name fallback does.
      _snwprintf_s(scratch, _TRUNCATE, L"0x%08X", *(const unsigned*)(cls + g_clsHashOff));
      name = scratch;
   }
   if (!name || !name[0]) return;

   if (g_text[0]) wcsncat_s(g_text, L", ", _TRUNCATE);
   wcsncat_s(g_text, name, _TRUNCATE);
}

// Fill g_text with the comma-joined vehicle names that spawn at `cp` for `team`.
// Leaves it empty when there are none, which blanks the element.
static void build_text(const uint8_t* cp, int team)
{
   g_text[0] = L'\0';
   if (!cp || team < 0 || !g_vehicleSpawnList) return;

   const void* shown[kMaxShown];
   int         shownCount = 0;

   uint8_t* const head = (uint8_t*)g_vehicleSpawnList;
   uint8_t*       node = *(uint8_t**)(head + kNode_Next);

   for (int guard = 0; node && node != head && guard < kMaxListWalk; ++guard) {
      uint8_t* vs = *(uint8_t**)(node + kNode_Object);
      node = *(uint8_t**)(node + kNode_Next);
      if (!vs) continue;

      if (*(const uint8_t* const*)(vs + kVS_CommandPost) != cp) continue;

      const uint8_t* entries[2] = {
         *(const uint8_t* const*)(vs + kVS_SpawnClass + team * 4),
         *(const uint8_t* const*)(vs + kVS_FlyerClass + team * 4),
      };

      for (const uint8_t* cls : entries) {
         if (!cls) continue;

         bool dup = false;
         for (int i = 0; i < shownCount; ++i)
            if (shown[i] == cls) { dup = true; break; }
         if (dup) continue;

         if (shownCount >= kMaxShown) return; // engine caps at 31 and warns; we just stop
         shown[shownCount++] = cls;

         append_class_name(cls);
      }
   }
}

// ---------------------------------------------------------------------------
// The hook
// ---------------------------------------------------------------------------

static void send_text(int idx)
{
   void* cls = g_vehicleEventArray[idx * kEventPlayerStride];
   if (!cls) return; // HUD::GameEvents::Open has not run yet

   struct { void* mClass; const wchar_t* mData; } ev = { cls, g_text };
   g_eventSend(&ev, nullptr);
}

static void refresh(void* self)
{
   const uint8_t* s   = (const uint8_t*)self;
   const int      idx = *(const int*)(s + kSD_EventIndex);
   if (idx < 0 || idx >= kMaxLocalPlayers) return;

   SentState& st = g_sent[idx];

   const int      mode = *(const int*)(s + g_sdModeOff);
   const uint8_t* post = *(const uint8_t* const*)(s + g_sdPostOff);

   // Side select is not the unit screen; the element is already on-screen there,
   // so it has to be actively blanked rather than just left alone.
   int team = -1;
   if (mode != kMode_SideSelect && post)
      team = object_team(cp_object(post));

   if (team < 0) {
      if (st.sent) {
         g_text[0] = L'\0';
         send_text(idx);
         st = SentState{ nullptr, -1, false };
      }
      return;
   }

   // The team matters as well as the post: a post captured while the player is
   // looking at it swaps the whole list.
   if (st.sent && st.post == post && st.team == team) return;

   build_text(post, team);
   send_text(idx);
   st = SentState{ post, team, true };
}

static bool __fastcall hooked_Update(void* self, void* edx, float dt)
{
   const bool r = original_Update(self, edx, dt);

   if (g_spawnVehicleListEnabled && self && g_eventSend && g_vehicleEventArray)
      refresh(self);

   return r;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void spawn_vehicle_list_install(uintptr_t exe_base)
{
   if (!g_spawnVehicleListEnabled) return;

   if (g_addr->spawndisplay_update == 0 || g_addr->hud_event_send == 0 ||
       g_addr->hud_event_spawn_vehicle == 0 || g_addr->vehicle_spawn_list == 0 ||
       g_addr->entityclass_label_off == 0) {
      install_log("[SpawnVehicleList] NOT installed: no address set for this build");
      return;
   }

   void* update = resolve(exe_base, g_addr->spawndisplay_update);

   // Prologue guard.  A mismatch is almost always an address derived from the
   // wrong image, and a silent decline is expensive to diagnose from a play test.
   // Steam and GOG share these ten bytes; they diverge at the 11th.
   static constexpr uint8_t kModtools[] = {
      0xA1, 0x80, 0x02, 0xB3, 0x00, // mov  eax,[in-game movie state global]
      0x83, 0xEC, 0x2C,             // sub  esp,0x2C
      0x85, 0xC0                    // test eax,eax
   };
   static constexpr uint8_t kRetail[] = {
      0x55, 0x8B, 0xEC,             // push ebp / mov ebp,esp
      0x83, 0xE4, 0xF0,             // and  esp,-0x10
      0x83, 0xEC, 0x28,             // sub  esp,0x28
      0x83                          // cmp  dword ptr [...],0
   };
   static_assert(sizeof(kModtools) == sizeof(kRetail), "guard sizes");

   const bool     isModtools = (g_build == GameBuild::Modtools);
   const uint8_t* expected   = isModtools ? kModtools : kRetail;

   if (std::memcmp(update, expected, sizeof(kModtools)) != 0) {
      const uint8_t* a = (const uint8_t*)update;
      install_log("[SpawnVehicleList] NOT installed: prologue mismatch. "
                  "SpawnDisplay::Update @0x%08X = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                  (unsigned)g_addr->spawndisplay_update,
                  a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]);
      return;
   }

   g_clsLabelOff    = (int)g_addr->entityclass_label_off;
   g_clsFilenameOff = (int)g_addr->entityclass_filename_off;
   g_clsHashOff     = (int)g_addr->entityclass_hash_off;
   g_sdModeOff      = (int)g_addr->spawndisplay_mode_off;
   g_sdPostOff      = (int)g_addr->spawndisplay_command_post_off;

   g_eventSend         = (fn_event_send_t)resolve(exe_base, g_addr->hud_event_send);
   g_vehicleEventArray = (void**)resolve(exe_base, g_addr->hud_event_spawn_vehicle);
   g_vehicleSpawnList  = (void**)resolve(exe_base, g_addr->vehicle_spawn_list);

   original_Update = (fn_update_t)update;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)original_Update, hooked_Update);
   DetourTransactionCommit();

   g_installed = (r == NO_ERROR);
   install_log("[SpawnVehicleList] %s (Update 0x%08X, Event::Send 0x%08X, "
               "event 0x%08X, list 0x%08X)",
               g_installed ? "installed" : "DetourAttach failed",
               (unsigned)g_addr->spawndisplay_update,
               (unsigned)g_addr->hud_event_send,
               (unsigned)g_addr->hud_event_spawn_vehicle,
               (unsigned)g_addr->vehicle_spawn_list);
}

void spawn_vehicle_list_uninstall()
{
   if (!g_installed) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_Update, hooked_Update);
   DetourTransactionCommit();

   g_installed = false;
}
