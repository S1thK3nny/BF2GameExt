#include "pch.h"
#include "award_disable.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>

// =============================================================================
// DisableAwardBuffs / DisableAwardWeapons
// =============================================================================
// Every combat-award effect in the game reads one function:
//
//   bool __thiscall MedalsMgr::IsAwardAvailable(MedalsMgr* this, int index)
//       { return gEnableAllAwards || (this->mAwardAvailable & (1 << index)); }
//
// mAwardAvailable is a ushort at MedalsMgr+0x4C. The function is never inlined:
// 23 call sites on modtools, Steam and GOG alike, matching the Phantom debug
// build one for one.
//
// TWO hooks are needed, because the availability bit has two writers.
//
// 1. MedalsMgr::Update - the singleplayer / net-host writer. Once per frame it
//    walks index 0..8 and diffs IsAwardAvailableInternal (has this been earned:
//    career level + medal points) against IsAwardAvailable (is the bit set):
//
//        earned = IsAwardAvailableInternal(this, i);
//        if (IsAwardAvailable(this, i) != earned) {
//            if (earned) { mAwardAvailable |= 1<<i; HudIndication(i, ...); }
//            else        { mAwardAvailable &= ~(1<<i); }
//        }
//
//    Hooking only IsAwardAvailable makes those two permanently disagree, so
//    every frame the loop re-takes the `earned` branch and re-fires the
//    "award unlocked" HUD message - the top-left message area spams and fades
//    forever. Suppressing IsAwardAvailableInternal instead keeps both sides
//    saying false: no transition, no message, and the bit is never set in the
//    first place. It also actively clears a bit that was already set, via the
//    else branch, which is silent.
//
//    MedalsMgr::ClearAllMedalPoints reads Internal too; a false there just
//    zeroes that award's points and timestamp, which is what "no award" means.
//
// 2. MedalsMgr::NetRead - on a multiplayer CLIENT the whole 16-bit mask arrives
//    over the wire, and both Update and IsAwardAvailableInternal bail out early
//    (Internal returns false outright when netOnClient). So a client's bits
//    never pass through hook 1 at all, and the read-side IsAwardAvailable hook
//    is what keeps the effects off there. NetRead fires its own HudIndication,
//    but only on a genuine old->new mask transition, so it cannot spam.
//
// With both hooks in place the Update diff compares false against false and
// stays quiet, while every consumer - host and client alike - reads false.
//
// Award index -> effect (MedalsMgr::sMedalPoints[9], name order):
//
//   0 gunslinger  weapon
//   1 frenzy      weapon
//   2 demolition  weapon
//   3 technician  weapon + PASSIVE: GameObject::AddPilotSkillCount(+1) from
//                 Controllable::SetPilot, i.e. the piloted-vehicle perk
//   4 marksman    weapon
//   5 regulator   weapon
//   6 endurance   PASSIVE: read as `push 6` by EntitySoldier::Update,
//                 EntityDroid::Update, EntityDroideka::Update,
//                 EntityFlyer::Update, EntityHover::Move,
//                 EntityWalker::UpdateState
//   7 guardian    PASSIVE: EntitySoldier/EntityDroideka::UpdateBuffTimers set
//                 mBuffDefenseTimer = 340.0f, mBuffDefenseMult = 0.5f
//   8 warhero     PASSIVE: same two, mBuffOffenseTimer = 340.0f,
//                 mBuffOffenseMult = 1.5f
//
// Why those buffs are permanent, and why removing them does not touch the
// ordinary buff system: UpdateBuffTimers only decrements a timer that is
// <= 300.0f. The award path writes 340.0f, so it never counts down - that is
// the whole implementation of "permanent". Officer buff weapons and buff
// pickups write timers below 300 and expire normally, so they are unaffected by
// anything here.
//
// Index 3 straddles the two groups: the same availability bit unlocks the
// technician award weapon AND the pilot-skill passive, with no second flag to
// separate them. It is grouped with the buffs, since the permanent passive is
// the part these toggles exist to remove; DisableAwardBuffs therefore also
// keeps the technician award weapon locked. Setting both keys disables all nine.
// =============================================================================

bool g_disableAwardBuffs   = false;
bool g_disableAwardWeapons = false;

// Awards granting a permanent passive (3 also carries a weapon - see above).
static constexpr unsigned kBuffAwards   = (1u << 3) | (1u << 6) | (1u << 7) | (1u << 8);
// Awards granting only a weapon.
static constexpr unsigned kWeaponAwards = (1u << 0) | (1u << 1) | (1u << 2) |
                                          (1u << 4) | (1u << 5);

static unsigned g_suppressed = 0; // bitmask of indices forced unavailable

// Both are bool __thiscall(this, int index).  As __fastcall the index lands on
// the stack and the callee cleans 4 bytes, matching the originals' RET 4.
using fn_award_query_t = bool(__fastcall*)(void* ecx, void* edx, int index);

static fn_award_query_t original_IsAwardAvailable         = nullptr;
static fn_award_query_t original_IsAwardAvailableInternal = nullptr;

static inline bool suppressed(int index)
{
   return (unsigned)index < 9u && (g_suppressed & (1u << index)) != 0;
}

static bool __fastcall hooked_IsAwardAvailable(void* ecx, void* /*edx*/, int index)
{
   if (suppressed(index)) return false;
   return original_IsAwardAvailable(ecx, nullptr, index);
}

static bool __fastcall hooked_IsAwardAvailableInternal(void* ecx, void* /*edx*/, int index)
{
   if (suppressed(index)) return false;
   return original_IsAwardAvailableInternal(ecx, nullptr, index);
}

void award_disable_install(uintptr_t exe_base)
{
   g_suppressed = 0;
   if (g_disableAwardBuffs)   g_suppressed |= kBuffAwards;
   if (g_disableAwardWeapons) g_suppressed |= kWeaponAwards;
   if (g_suppressed == 0) return;

   if (g_addr->medals_is_award_available == 0 ||
       g_addr->medals_is_award_available_internal == 0)
      return;

   original_IsAwardAvailable =
      (fn_award_query_t)resolve(exe_base, g_addr->medals_is_award_available);
   original_IsAwardAvailableInternal =
      (fn_award_query_t)resolve(exe_base, g_addr->medals_is_award_available_internal);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_IsAwardAvailable, hooked_IsAwardAvailable);
   DetourAttach(&(PVOID&)original_IsAwardAvailableInternal, hooked_IsAwardAvailableInternal);
   if (DetourTransactionCommit() != NO_ERROR) {
      original_IsAwardAvailable = nullptr;
      original_IsAwardAvailableInternal = nullptr;
   }
}

void award_disable_uninstall()
{
   if (!original_IsAwardAvailable) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_IsAwardAvailable, hooked_IsAwardAvailable);
   DetourDetach(&(PVOID&)original_IsAwardAvailableInternal, hooked_IsAwardAvailableInternal);
   DetourTransactionCommit();
   original_IsAwardAvailable = nullptr;
   original_IsAwardAvailableInternal = nullptr;
}
