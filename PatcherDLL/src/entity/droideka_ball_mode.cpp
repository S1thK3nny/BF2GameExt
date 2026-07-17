#include "pch.h"
#include "droideka_ball_mode.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstdlib>
#include <cstring>
#include <detours.h>

// =============================================================================
// DisableBallMode - per-class ODF property for EntityDroideka.
//
//   DisableBallMode = 1
//
// The droideka is the only playable walker/ball hybrid, so a mod that wants the
// chassis as a plain walking unit has no way to take the roll away: the player
// can choose not to press the button, but the AI still rolls. This makes it a
// class property, so it binds the AI too.
//
// How the roll works (see project memory "droideka-ball-mode-system"):
//   EntityDroideka is a table-driven FSM. NextState(input) does
//   SetState(sStateTable[mState].next[input]); SetState is reachable ONLY from
//   NextState. State 0xB = roll-up transition, 0xC = ball (swaps the collision
//   model to mClass->mCollisionBall), 0xD = unroll. Input 7 = "roll": every
//   upright state's next[7] is 0xB.
//
//   EntityDroideka::UpdatePilot (entity vtable +0x120, called every frame from
//   EntityDroideka::Update) is the ONE place input 7 is ever issued, for the
//   player AND the AI:
//     - roll:   mState not in {0xB,0xC,0xD} and (player trigger held, or the AI
//               set UnitController->mLowLevel.mFoldStatus == FOLD_FOLD)
//               -> NextState(7)
//     - unroll: mState == 0xC and (trigger, or mFoldStatus == FOLD_UNFOLD) and
//               CanUnroll() -> NextState(0)  (state 0xC next[0] = 0xD)
//   UpdateState/Update only ever issue inputs 0,2,3,4,5,6 - never 7.
//
//   So while upright, UpdatePilot's *only* effect is requesting the roll:
//   returning early there is an exact and complete block, for both pilots, with
//   no other behaviour suppressed. When already ball-side we let the original
//   run so a droideka can always still unroll back out (that state is
//   unreachable once the flag is set, but the flag is per-class and the check
//   costs nothing).
// =============================================================================

// FSM state ids - build-invariant (compile-time constants in the game's own
// state table; verified identical in modtools and Steam disassembly).
static constexpr int kStateRollUp = 0x0B;
static constexpr int kStateBall   = 0x0C;
static constexpr int kStateUnroll = 0x0D;

// ---------------------------------------------------------------------------
// Per-build EntityDroideka field offsets
// ---------------------------------------------------------------------------
// Modtools is a DEBUG build, Steam a RELEASE build, and this struct is laid out
// differently between them. Each offset below is confirmed at two independent
// read sites per build:
//   mClass  modtools 0x450  (UpdateStateRolling 0x4e230c, GetMaxSpeed 0x4e2d90)
//           Steam    0x438  (UpdateStateRolling 0x4a2bf5, GetMaxSpeed 0x4a6140)
//   mState  modtools 0x1A74 (UpdatePilot 0x4e826b, NextState 0x4ed611)
//           Steam    0x1A54 (UpdatePilot 0x4a204c, NextState 0x4a3285)
struct DroidekaLayout {
   int mClass;
   int mState;
};
static constexpr DroidekaLayout kLayoutModtools = {0x450, 0x1A74};
static constexpr DroidekaLayout kLayoutSteam    = {0x438, 0x1A54};

static DroidekaLayout s_layout = kLayoutModtools;

// ---------------------------------------------------------------------------
// Game function types
// ---------------------------------------------------------------------------

using fn_hash_string_t = uint32_t(__cdecl*)(const char*);

// EntityDroidekaClass::SetProperty - __thiscall(this, uint hash, const char* value)
using fn_SetProperty_t = void(__fastcall*)(void* ecx, void* edx,
                                           unsigned int hash, const char* value);

// EntityDroideka::UpdatePilot - __thiscall(this); this = EntityDroideka base
// (no adjustor: it reads mState off ECX directly).
using fn_UpdatePilot_t = void(__fastcall*)(void* ecx, void* edx);

// EntityDroidekaClass::Derive - __thiscall(this, uint hash) -> new EntityClass*.
// ODF inheritance: allocates a fresh class and copy-constructs it from `this`.
using fn_Derive_t = void*(__fastcall*)(void* ecx, void* edx, unsigned int hash);

using fn_init_state_t = void(__cdecl*)();

// ---------------------------------------------------------------------------
// Resolved pointers / trampolines
// ---------------------------------------------------------------------------

static fn_hash_string_t fn_hash_string       = nullptr;
static fn_SetProperty_t original_SetProperty = nullptr;
static fn_UpdatePilot_t original_UpdatePilot = nullptr;
static fn_Derive_t      original_Derive      = nullptr;
static fn_init_state_t  original_init_state  = nullptr;

static uint32_t g_propHash = 0; // hash("DisableBallMode"), computed lazily

// ---------------------------------------------------------------------------
// Classes with the flag set
// ---------------------------------------------------------------------------
// Membership == disabled, so stock content leaves this empty and the UpdatePilot
// hook costs one predictable compare per droideka per frame.

static constexpr int kMaxDisabledClasses = 32;

static void* g_disabled[kMaxDisabledClasses] = {};
static int   g_disabledCount = 0;

static bool isDisabled(const void* cls)
{
   for (int i = 0; i < g_disabledCount; i++) {
      if (g_disabled[i] == cls) return true;
   }
   return false;
}

static void setDisabled(void* cls, bool on)
{
   for (int i = 0; i < g_disabledCount; i++) {
      if (g_disabled[i] != cls) continue;
      if (!on) g_disabled[i] = g_disabled[--g_disabledCount]; // ODF override back to 0
      return;
   }
   if (!on) return;
   if (g_disabledCount >= kMaxDisabledClasses) {
      get_gamelog()("[DisableBallMode] class table full (%d), ignoring\n",
                    kMaxDisabledClasses);
      return;
   }
   g_disabled[g_disabledCount++] = cls;
}

// ---------------------------------------------------------------------------
// Hook: EntityDroidekaClass::SetProperty
// ---------------------------------------------------------------------------

static void __fastcall hooked_SetProperty(void* ecx, void* /*edx*/,
                                          unsigned int hash, const char* value)
{
   // Lazy init: cannot call game code during install - dllmain's install window
   // leaves the exe sections PAGE_READWRITE (no EXEC) and the Steam exe is
   // DEP-enabled. ODF parsing happens long after, with .text executable again.
   if (g_propHash == 0 && fn_hash_string)
      g_propHash = fn_hash_string("DisableBallMode");

   if (g_propHash != 0 && hash == g_propHash) {
      // Consume it: the stock parser has no case for this hash.
      // atoi matches how the engine reads its own int properties (sscanf %d),
      // so a non-numeric value reads as 0 here exactly as it would there.
      if (ecx && value) setDisabled(ecx, std::atoi(value) != 0);
      return;
   }

   original_SetProperty(ecx, nullptr, hash, value);
}

// ---------------------------------------------------------------------------
// Hook: EntityDroidekaClass::Derive
// ---------------------------------------------------------------------------
// ODF inheritance. Every walkerdroid ODF class is created as base->Derive(hash);
// a `ClassParent = "..."` ODF derives from that parent's class instead. Derive
// copy-constructs the new class from the parent, so every stock property is
// inherited by virtue of living in the struct. Our flag lives in a side table
// keyed by class pointer, so without this it would NOT inherit and a ClassParent
// child of a DisableBallMode=1 droideka would silently roll again.
//
// The child's own ODF lines are applied after Derive returns, so a child that
// restates `DisableBallMode = 0` still overrides the parent correctly.

static void* __fastcall hooked_Derive(void* ecx, void* /*edx*/, unsigned int hash)
{
   void* derived = original_Derive(ecx, nullptr, hash);
   if (derived && ecx && isDisabled(ecx)) setDisabled(derived, true);
   return derived;
}

// ---------------------------------------------------------------------------
// Hook: EntityDroideka::UpdatePilot
// ---------------------------------------------------------------------------

static void __fastcall hooked_UpdatePilot(void* ecx, void* /*edx*/)
{
   if (g_disabledCount > 0 && ecx) {
      void* cls = *(void**)((uintptr_t)ecx + s_layout.mClass);
      if (cls && isDisabled(cls)) {
         const int state = *(int*)((uintptr_t)ecx + s_layout.mState);
         // Upright: the only thing the original would do is request the roll.
         if (state != kStateRollUp && state != kStateBall && state != kStateUnroll)
            return;
         // Ball-side: fall through so it can still unroll back out.
      }
   }

   original_UpdatePilot(ecx, nullptr);
}

// ---------------------------------------------------------------------------
// Hook: init_state (non-modtools only - see droideka_ball_mode_reset)
// ---------------------------------------------------------------------------

static void __cdecl hooked_init_state()
{
   original_init_state();
   droideka_ball_mode_reset();
}

// ---------------------------------------------------------------------------
// Install / Uninstall / Reset
// ---------------------------------------------------------------------------

void droideka_ball_mode_install(uintptr_t exe_base)
{
   switch (g_build) {
   case GameBuild::Modtools: s_layout = kLayoutModtools; break;
   case GameBuild::Steam:    s_layout = kLayoutSteam;    break;
   default: return; // GOG / unknown: offsets not derived yet
   }

   if (g_addr->hash_string == 0 || g_addr->droideka_class_set_property == 0 ||
       g_addr->droideka_update_pilot == 0 || g_addr->droideka_class_derive == 0)
      return;

   fn_hash_string = (fn_hash_string_t)resolve(exe_base, g_addr->hash_string);
   g_propHash     = 0; // computed lazily in hooked_SetProperty - see note there

   original_SetProperty = (fn_SetProperty_t)resolve(exe_base, g_addr->droideka_class_set_property);
   original_UpdatePilot = (fn_UpdatePilot_t)resolve(exe_base, g_addr->droideka_update_pilot);
   original_Derive      = (fn_Derive_t)     resolve(exe_base, g_addr->droideka_class_derive);

   // On modtools lua_hooks already detours init_state and calls our reset from
   // its hooked_init_state; only take it ourselves elsewhere, so the function
   // is never detoured twice.
   const bool ownInitState = (g_build != GameBuild::Modtools) && g_addr->init_state != 0;
   if (ownInitState)
      original_init_state = (fn_init_state_t)resolve(exe_base, g_addr->init_state);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   DetourAttach(&(PVOID&)original_UpdatePilot, hooked_UpdatePilot);
   DetourAttach(&(PVOID&)original_Derive,      hooked_Derive);
   if (ownInitState)
      DetourAttach(&(PVOID&)original_init_state, hooked_init_state);
   DetourTransactionCommit();
}

void droideka_ball_mode_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_SetProperty) DetourDetach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   if (original_UpdatePilot) DetourDetach(&(PVOID&)original_UpdatePilot, hooked_UpdatePilot);
   if (original_Derive)      DetourDetach(&(PVOID&)original_Derive,      hooked_Derive);
   if (original_init_state)  DetourDetach(&(PVOID&)original_init_state,  hooked_init_state);
   DetourTransactionCommit();
}

void droideka_ball_mode_reset()
{
   std::memset(g_disabled, 0, sizeof(g_disabled));
   g_disabledCount = 0;
}
