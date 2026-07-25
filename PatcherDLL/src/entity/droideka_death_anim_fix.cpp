#include "pch.h"
#include "droideka_death_anim_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

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
// TODO: force the shield off when the death animation starts.
//   Now that state 3 actually holds for the length of death01 instead of being
//   preempted after ~16ms, the shield stays up for the whole collapse -- it used
//   to be invisible because the state lasted a single frame. It should be
//   dropped at the 0 -> 3 transition, i.e. in the thunk below on the first call
//   that is NOT swallowed (mState != 3 going in, so the real NextState runs).
//   Open questions before implementing:
//     - use the disable function of the weapon shield fire itself, should be raise I think.
// =============================================================================

bool g_droidekaDeathAnimEnabled = true;

// Dying state id -- build-invariant (a compile-time constant of the game's own
// state table; identical in modtools and Steam).  A macro, not a constexpr:
// MSVC inline asm can only reference operands with storage.
#define kStateDying 3

// EntityDroideka::mState offset, per build (debug vs release layout).
static uint32_t s_mStateOff = 0;

// __thiscall thunk standing in for `CALL [vtable+0x130]`.
// On entry: ECX = EntityDroideka base, [esp] = return address, [esp+4] = 6.
__declspec(naked) static void droideka_die_input_guard()
{
   __asm {
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
   default:
      return; // GOG / unknown: not derived
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
   if (!s_site) return;
   protected_write(s_site, s_orig, sizeof(s_orig)); // sections are re-protected by now
   s_site = nullptr;
}
