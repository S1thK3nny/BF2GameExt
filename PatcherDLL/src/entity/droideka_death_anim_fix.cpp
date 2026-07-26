#include "pch.h"
#include "droideka_death_anim_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>

#include <cstring>

// =============================================================================
// Droideka death animation fix.
//
// Every stock droideka animation bank defines a death clip, and the engine
// loads it: EntityDroidekaClass::InitAnimations hardcodes
// InitAnimation(this, 3, "death01"), so FSM state 3 ("dying") holds it. It just
// never plays for more than a single frame.
//
// EntityDroideka's movement is a table-driven FSM: NextState(input) does
// SetState(sStateTable[mState].next[input]). Input 6 = "die". In the table every
// upright state's next[6] is 3 (dying) -- but state 3's next[6] is 4 (dead).
// So issuing input 6 twice walks straight past the animation.
//
// EntityDroideka::Update issues it EVERY frame while mIsDead:
//     if (mIsDead) {
//         if (mState == 4) { explode; gib; kill; return false; }
//         NextState(6);                 // <-- ungated
//     }
//     ... later in the same frame: UpdateState() / UpdateStateRolling()
// giving:
//     frame 1: mState 0 -> SetState(3), death01 starts
//     frame 2: mState 3 -> SetState(4), death01 dies ~16ms in
//     frame 3: mState 4 -> explode
//
// EntityWalker -- same FSM design, same table shape, same "death01" at index 3 --
// has NO such call in its Update; its only die transition lives in
// EntityWalker::UpdateState, gated on the current animation completing. That is
// why ATST/ATTE/ATAT death animations play. EntityDroideka::UpdateState has the
// identical gated call, but Update's ungated one always preempts it, making the
// gated one dead code for state 3.
//
// The fix: make Update's per-frame NextState(6) a no-op while mState is already
// 3. Everything else is untouched:
//   - the initial kick (upright -> 3) still happens immediately, which matters:
//     UpdateState alone would wait for the *current* (e.g. idle) animation to
//     finish before starting the death.
//   - ball-mode states (0xB/0xC/0xD) have next[6] = 4, so a droideka killed while
//     rolling still explodes instantly with no upright death animation, as before.
//   - once death01 finishes, UpdateState's gated NextState(6) does 3 -> 4 and
//     Update explodes on the next frame.
//   - if a bank has no death01, InitAnimation leaves mTimeTotals[3] = 0, so
//     UpdateState advances 3 -> 4 on the very first frame. No hang.
//
// Implementation: the call site is `PUSH 6; CALL [vtable+0x130]` with ECX = the
// entity base (verified on both builds). Redirect that 6-byte CALL to a thunk
// that either tail-jumps to the real NextState or swallows the call with RET 4 --
// exactly what NextState's own `RET 4` would have done. Register-transparent:
// NextState is a __thiscall and may clobber EAX/ECX/EDX anyway.
//
// Shield teardown on death.
//
// Now that state 3 actually holds for the length of death01 instead of being
// preempted after ~16ms, the shield stays up for the whole collapse -- it used
// to be invisible because the state lasted a single frame.
//
// WeaponShield::Update already has a complete shield-off path, gated on
// entity+0x1FC bit 3 ("alive", the same bit EntityDroideka::Update tests before
// calling Die()).  On death that bit is 0, so the engine's own rule is already
// satisfied -- destroy the effect, stop the loop sound, clear the shield bit
// (+0x1FC bit 5), remove the collision object.
//
// It never runs, because the weapon leaves the global object-update loop
// (FUN_0048fc70 on modtools: `if (!obj->vt[1](dt)) obj->vt[0](1)` over a linked
// list) the moment the droideka dies.  Runtime capture is unambiguous: with a
// hook on WeaponShield::Update logging every call whose owner passes the game's
// own EntityDroideka RTTI check, no call EVER reports state 3 or 4, while the
// same entity logs freely at states 0/11/12/13 before and after.  Widening the
// state test inside that function -- the first thing tried here -- therefore
// patched code that is never reached during death.
//
// So the fix reaches the engine's teardown once, from the death transition:
// droideka_die_handler() calls WeaponShield::Update on the droideka's shield
// weapon on the frame the FSM enters state 3.  Only once -- the off path replays
// the shield-down sound every time it runs.
//
// The weapon pointer comes from a pairing observed by the same hook while the
// droideka is alive, not from a per-build weapon-array offset: the droideka's
// mWeapon/mWeaponIndex are NOT at the EntitySoldier offsets in g_soldier, and
// the disassembly (array +0x470, index +0x480) disagrees with the Ghidra struct
// (+0x46C / +0x470), so neither is trustworthy enough to dereference.
//
// Both halves are gated on the same flag: without the death-animation fix
// state 3 lasts one frame and there is nothing to hide.
// =============================================================================

bool g_droidekaDeathAnimEnabled = true;

// Dying state id -- build-invariant (a compile-time constant of the game's own
// state table; identical in modtools and Steam).  A macro, not a constexpr:
// MSVC inline asm can only reference operands with storage.
#define kStateDying 3

// EntityDroideka::mState offset, per build (debug vs release layout).
static uint32_t s_mStateOff = 0;

// ---------------------------------------------------------------------------
// Shield-owner tracking.
//
// The death path needs the dying droideka's WeaponShield, and there is no
// per-build offset for it worth dereferencing: the droideka's mWeapon /
// mWeaponIndex are NOT at the EntitySoldier offsets in g_soldier, and the
// disassembly (array +0x470, index +0x480) disagrees with the Ghidra struct
// (+0x46C / +0x470).  Instead the hook below observes the (weapon, entity)
// pairing directly -- it runs every frame for every live shield -- and the death
// path looks it up.
// ---------------------------------------------------------------------------

// Weapon offset, build-invariant (see weapon/shield_channel_fix.cpp).
static constexpr int kWeapon_mOwner = 0x6C;

static uintptr_t s_rttiHashPtr  = 0;  // -> the EntityDroideka PblHash value
static uintptr_t s_shieldVtable = 0;  // captured from the first live WeaponShield

struct DkaShield {
   uintptr_t ent;
   uintptr_t wpn;
   uint32_t  lastSeen;  // tick of the last update, for eviction
};
static DkaShield s_shields[32] = {};
static uint32_t  s_tick        = 0;

// Lookup without inserting -- what the death path wants.  Inserting there would
// evict a live droideka's entry to store one that has no weapon recorded.
static DkaShield* dka_find(uintptr_t ent)
{
   for (DkaShield& e : s_shields) if (e.ent == ent) return &e;
   return nullptr;
}

// Live droidekas refresh their entry every frame, so the least-recently-seen
// entry is always a stale one (destroyed entity, or an address since reused).
static DkaShield* dka_slot(uintptr_t ent)
{
   for (DkaShield& e : s_shields) if (e.ent == ent) return &e;
   for (DkaShield& e : s_shields) if (e.ent == 0)   { e.ent = ent; e.wpn = 0; return &e; }

   DkaShield* oldest = &s_shields[0];
   for (DkaShield& e : s_shields) if (e.lastSeen < oldest->lastSeen) oldest = &e;
   oldest->ent = ent;
   oldest->wpn = 0;
   return oldest;
}

typedef void (__thiscall* fn_ShieldUpdate_t)(void* ecx, float dt);
static fn_ShieldUpdate_t s_origShieldUpdate = nullptr;

// Resolve a WeaponShield's owning entity exactly the way WeaponShield::Update
// does: (mOwner+0x18)->vt[0x20]() yields the entity.
static uintptr_t shield_owner_entity(uintptr_t wpn)
{
   const uintptr_t owner = *(uintptr_t*)(wpn + kWeapon_mOwner);
   if (!owner) return 0;
   const uintptr_t sub = owner + 0x18;
   const uintptr_t vt  = *(uintptr_t*)sub;
   if (!vt) return 0;
   typedef uintptr_t (__thiscall* GetEnt_t)(uintptr_t);
   return ((GetEnt_t)(*(uintptr_t*)(vt + 0x20)))(sub);
}

static bool entity_is_droideka(uintptr_t ent)
{
   if (!ent || !s_rttiHashPtr) return false;
   typedef char (__thiscall* IsA_t)(uintptr_t, uint32_t);
   return ((IsA_t)(**(uintptr_t**)ent))(ent, *(uint32_t*)s_rttiHashPtr) != 0;
}

// Records which WeaponShield belongs to which droideka.  Deliberately does no
// other work -- the shield behaviour itself is entirely the engine's.
static void __fastcall hooked_ShieldUpdate(void* ecx, void* /*edx*/, float dt)
{
   __try {
      const uintptr_t wpn = (uintptr_t)ecx;
      const uintptr_t ent = shield_owner_entity(wpn);
      if (entity_is_droideka(ent)) {
         if (!s_shieldVtable) s_shieldVtable = *(uintptr_t*)wpn;
         DkaShield* const slot = dka_slot(ent);
         if (slot) { slot->wpn = wpn; slot->lastSeen = ++s_tick; }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A torn-down owner here must not take the game down.
   }

   s_origShieldUpdate(ecx, dt);
}

void droideka_shield_tracker_install(uintptr_t exe_base)
{
   // Deliberately does NOT depend on droideka_death_anim_install having run:
   // lua_hooks_install (our caller) is dllmain.cpp:215, that one is :239.
   // Deriving s_mStateOff here keeps this independent of call order.
   uintptr_t updVA = 0;
   switch (g_build) {
   case GameBuild::Modtools:
      updVA = game_addrs::modtools::weapon_shield_update;
      s_rttiHashPtr = (uintptr_t)resolve(exe_base, game_addrs::modtools::entity_droideka_rtti_hash);
      s_mStateOff   = 0x1A74;
      break;
   case GameBuild::Steam:
      updVA = game_addrs::steam::weapon_shield_update;
      s_rttiHashPtr = (uintptr_t)resolve(exe_base, game_addrs::steam::entity_droideka_rtti_hash);
      s_mStateOff   = 0x1A54;
      break;
   case GameBuild::GOG:
      updVA = game_addrs::gog::weapon_shield_update;
      s_rttiHashPtr = (uintptr_t)resolve(exe_base, game_addrs::gog::entity_droideka_rtti_hash);
      s_mStateOff   = 0x1A54; // same release layout as Steam
      break;
   default:
      return; // unknown build
   }
   if (!updVA || !s_rttiHashPtr) return;

   s_origShieldUpdate = (fn_ShieldUpdate_t)resolve(exe_base, updVA);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)s_origShieldUpdate, hooked_ShieldUpdate);
   DetourTransactionCommit();
}

// Called from the naked thunk below with the dying entity, before the state
// advances.
//
// The weapon leaves the global object-update loop the moment the droideka dies,
// so WeaponShield::Update stops running and mShieldEffect is never destroyed --
// the shield renders through the whole collapse.  entity+0x1FC bit 3 ("alive") is
// already 0 here, which is precisely the condition that function tests to run its
// full teardown: destroy effect, stop the loop sound, clear the shield bit,
// remove the collision object.  One call reaches it, so the engine does its own
// cleanup rather than us reimplementing it.
//
// Only on the transition frame (incoming state != dying), i.e. once per death:
// the off path replays the shield-down GameSound every time it runs, so calling
// it on every dead frame would stutter the sound.
//
// Declared before the thunk because inline asm needs the symbol to exist.
static void __cdecl droideka_die_handler(uintptr_t ent)
{
   __try {
      if (*(int*)(ent + s_mStateOff) == kStateDying) return;  // already dying
      if (!s_origShieldUpdate) return;

      DkaShield* const slot = dka_find(ent);
      if (!slot || !slot->wpn) return;

      // The recorded weapon is only touched again here, one or more frames after
      // it was last seen alive, so validate before calling into it: the vtable
      // must still be a WeaponShield, and it must still belong to this entity.
      // Guards against the object having been freed or its pool slot reused.
      const uintptr_t wpn = slot->wpn;
      if (s_shieldVtable && *(uintptr_t*)wpn != s_shieldVtable) { slot->wpn = 0; return; }
      if (shield_owner_entity(wpn) != ent)                      { slot->wpn = 0; return; }

      s_origShieldUpdate((void*)wpn, 0.0f);
      slot->wpn = 0;  // torn down; nothing more to do for this death
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // Never let shield cleanup break the death animation.
   }
}

// __thiscall thunk standing in for `CALL [vtable+0x130]`.
// On entry: ECX = EntityDroideka base, [esp] = return address, [esp+4] = 6.
__declspec(naked) static void droideka_die_input_guard()
{
   __asm {
      // pushad/pushfd keep this register-transparent around the C call.
      pushad
      pushfd
      push ecx
      call droideka_die_handler
      add  esp, 4
      popfd
      popad

      mov  eax, [s_mStateOff]
      cmp  dword ptr [ecx + eax], kStateDying
      je   swallow
      mov  eax, [ecx]                 // primary vtable
      jmp  dword ptr [eax + 0x130]    // NextState(6); its RET 4 returns to our caller
   swallow:
      ret  4                          // already dying: drop the input, let death01 play
   }
}

// Positive-ID signatures: the CALL itself plus the 6 bytes before it, so a wrong
// address on an un-derived build no-ops instead of corrupting code.
//   modtools: MOV EDX,[ESI]; PUSH 6; MOV ECX,ESI   -> CALL [EDX+0x130]
//   Steam:    MOV EAX,[EBX]; MOV ECX,EBX; PUSH 6   -> CALL [EAX+0x130]
static const uint8_t kSiteModtools[] = {0xFF, 0x92, 0x30, 0x01, 0x00, 0x00};
static const uint8_t kPrevModtools[] = {0x8B, 0x16, 0x6A, 0x06, 0x8B, 0xCE};
static const uint8_t kSiteSteam[]    = {0xFF, 0x90, 0x30, 0x01, 0x00, 0x00};
static const uint8_t kPrevSteam[]    = {0x8B, 0x03, 0x8B, 0xCB, 0x6A, 0x06};

static uint8_t* s_site    = nullptr;
static uint8_t  s_orig[6] = {};

void droideka_death_anim_install(uintptr_t exe_base)
{
   if (!g_droidekaDeathAnimEnabled) return;

   uintptr_t siteVA;
   const uint8_t *kSite, *kPrev;
   switch (g_build) {
   case GameBuild::Modtools:
      siteVA = game_addrs::modtools::droideka_update_nextstate_call;
      kSite = kSiteModtools; kPrev = kPrevModtools;
      s_mStateOff = 0x1A74;
      break;
   case GameBuild::Steam:
      siteVA = game_addrs::steam::droideka_update_nextstate_call;
      kSite = kSiteSteam; kPrev = kPrevSteam;
      s_mStateOff = 0x1A54;
      break;
   case GameBuild::GOG:
      siteVA = game_addrs::gog::droideka_update_nextstate_call;
      // Byte-identical call site and lead-in on GOG (verified against the exe).
      kSite = kSiteSteam; kPrev = kPrevSteam;
      s_mStateOff = 0x1A54;
      break;
   default:
      return; // unknown build
   }
   if (siteVA == 0) return;

   uint8_t* site = (uint8_t*)resolve(exe_base, siteVA);
   if (std::memcmp(site, kSite, sizeof(kSiteModtools)) != 0) return;
   if (std::memcmp(site - 6, kPrev, 6) != 0) return;

   s_site = site;
   std::memcpy(s_orig, site, sizeof(s_orig));

   // E8 rel32 (CALL thunk) over bytes 0..4, NOP the 6th.  .text is RW during
   // install (dllmain re-protects afterwards), so no VirtualProtect needed.
   const int32_t rel = (int32_t)((uintptr_t)&droideka_die_input_guard - ((uintptr_t)site + 5));
   site[0] = 0xE8;
   *(int32_t*)(site + 1) = rel;
   site[5] = 0x90;
}

void droideka_death_anim_uninstall()
{
   if (s_origShieldUpdate) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(&(PVOID&)s_origShieldUpdate, hooked_ShieldUpdate);
      DetourTransactionCommit();
      s_origShieldUpdate = nullptr;
   }

   if (!s_site) return;
   protected_write(s_site, s_orig, sizeof(s_orig)); // sections are re-protected by now
   s_site = nullptr;
}
