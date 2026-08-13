#include "pch.h"
#include "ai_fairness.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstring>

// =============================================================================
// AI player-focus fairness.
//
// BF2 privileges a human player over an AI bot in six separate places (see
// docs/RE/AIComparison_BF1_vs_BF2.md).  Four of them are each guarded by a
// single conditional jump and are patched here.  SWBF1 has no player term
// anywhere in its target selection, which is why its AI is remembered as
// "fairer"; these patches bring BF2 to the same footing without giving up any
// of its extra machinery.
//
// Still NOT touched, and the reason AI will always turn on you once you shoot:
//   UnitAgent::DamagedEventPlayerCentricness force-sets the attacker as the
//   target, bypassing selection entirely, and re-broadcasts the damage to
//   every AI within 20 m if the shot came from over 50 m away.
//   AI::UnitThreatManager::AddThreat also registers a player at TS_SEEN but a
//   bot at TS_UNKNOWN, worth a standing `status * 18` bonus in the same score
//   #3 patches.  Both are candidates if the four below are not enough.
//
// All four default ON.  Set an INI key to 0 for stock behaviour, in which case
// that patch is not applied and its bytes are left untouched.
//
// The identity test in every one of these sites is Controllable::mPlayerId
// (>= 0 means a human player).  Note the offset differs from Phantom: modtools
// and the retail builds both put it at +0xd4, Phantom at +0xd0.
//
// -----------------------------------------------------------------------------
// #1 — VisionHelper::MaxVisibleDist: players are visible from twice as far.
//
//   mult = mGlobalVisibilityModifier;
//   if (target->GetControllable()->mPlayerId >= 0)  mult = mult * 2;   <-- here
//   ... flyer x2 / EntityBuildingArmed x10 ...
//   return (targetRadius * 20 + 30) * mult;
//
// This is the single most impactful bias: it is why AI spot you across a map
// while ignoring a bot beside them — they genuinely cannot see the bot yet.
// The doubling is guarded by one JL, so flipping that JL to an unconditional
// JMP skips it and players are detected at exactly bot range.
//
//   modtools 0x005c9a27  7C 0C   JL  0x005c9a35    ->  EB 0C   JMP
//     (guards  D9 44 24 10  FLD [ESP+0x10] / DC C0  FADD ST0,ST0)
//   Steam    0x00670496  7C 12   JL  0x006704aa    ->  EB 12   JMP
//     (guards  MOVSS XMM0,[EBP-4] / MULSS XMM0,[2.0] / MOVSS [EBP-4],XMM0)
//
// -----------------------------------------------------------------------------
// #2 — VisionHelper::GetVisualPriority: every target BUT the player pays double.
//
// The return is a COST (lower = better target).  Every candidate's cost is
// doubled except a human player being ranked by non-vehicle AI, so to an
// infantry bot a player at 40 m ranks like a bot at 20 m.  Making the undoubled
// path unreachable doubles everyone uniformly — exactly SWBF1's behaviour,
// whose GetVisualPriority is just `dist * (21 - typeMatchup)` with no player
// term at all.
//
//   Steam    0x006710eb  7C 1D   JL  0x0067110a    ->  EB 1D   JMP
//     The JL already skips to the doubling path when the target is a bot;
//     forcing it always-taken means players take it too.
//
//   modtools 0x005c93a4  74 13   JZ  0x005c93b9    ->  90 90   NOP NOP
//     Modtools reaches the undoubled return through the IsVehicle test instead:
//         TEST AL,AL / JZ -> undoubled return
//     0x005c93b9 has exactly one xref (this JZ, confirmed), so NOPing it makes
//     the undoubled epilogue unreachable and every path falls through to
//     FILD/FADD ST0,ST0.  Same end result as the Steam patch, different shape
//     because the two builds emitted the branch differently.
//
// -----------------------------------------------------------------------------
// #3 — AI::Threat::GetPriority: the player is worth 2x a bot on eye contact.
//
//   score = 100 - (GetVisualPriority(me, threat) * 100 / 60000)
//   if (threat is a human player)
//        score *= clamp((dot(dirToMe, playerForward) - 0.6) / 0.4, floor, 1.0)
//   else score /= 2
//
// On paper this is the fair one: it prioritises whoever is genuinely dangerous
// right now, and the design clearly expected a player to often be facing away,
// dropping them to the floor (0.0 to 0.4 depending on difficulty).  In practice
// a human is nearly always looking at the AI they are near, so the multiplier
// sits pinned at 1.0 against a bot's flat 0.5 — a permanent doubling rather
// than a situational one.  That is what makes AI break off and walk past each
// other the moment they lay eyes on you.
//
// Patch: force the `PlayerControllerPtr(...) == null` jump — the branch the
// engine uses to send bots down the `/2` path — to be unconditional, so a
// player is halved too.  The tradeoff is that AI no longer react preferentially
// to being aimed at by anyone, which is a real behaviour to lose, hence its own
// INI key.  JZ rel32 -> JMP rel32 + NOP; the JMP is one byte shorter, so its
// displacement is the JZ's + 1 to land on the same target.
//
//   modtools 0x005a147a  0F 84 8E000000 -> E9 8F000000 90  (target 0x005a150e)
//   Steam    0x00669bad  0F 84 99000000 -> E9 9A000000 90  (target 0x00669c4c)
//   GOG      0x0066ac4d  identical bytes and displacement to Steam
//
// -----------------------------------------------------------------------------
// #4 — AI::UnitThreatManager::ShouldRaytestUnit: hard tunnel vision.
//
//   if (candidate is a human player)  -> normal per-LOD raytest timer
//   else {
//       current = GetVisibleObject(this);
//       if (current == NULL)          return true;
//       if (current is a human player) return false;   <-- here
//       -> normal per-LOD raytest timer
//   }
//
// This gates whether the AI spends a line-of-sight ray on a candidate at all
// (UpdatePotentiallyVisible only calls VisionTest/AddRayRequest when it returns
// true).  So while an AI is tracking YOU, it refuses to raytest any other
// enemy, which means it can never confirm one visible and can never switch.
// Unlike #1-#3 this is not a ranking bias, it is a hard gate — which is why it
// survived those patches and still reads as "once they see me they lock on".
//
// Patch: force the `JZ` that skips the `return false` to be unconditional, so
// tracking a player no longer suppresses everyone else's visibility checks.
// Identical `74 0B` -> `EB 0B` on all three builds.
//
//   modtools 0x005a1bb6   Steam 0x0066a3c0   GOG 0x0066b460
//
// Cost: AI now spend LOS rays on other candidates while engaging a player,
// where stock spent none.  The per-LOD interval table {4,4,2,1,1} seconds still
// rate-limits them, so it is bounded, but it is a real if small increase.
// =============================================================================

bool g_aiPlayerVisionFairness   = true;
bool g_aiPlayerPriorityFairness = true;
// #3 defaults OFF.  Unlike the other three it does not remove an unfairness,
// it removes a behaviour: stock AI escalate whoever is aiming at them, and
// halving the player's score along with everyone else's takes that away.  The
// result play-tests as AI intermittently ignoring the player even while being
// aimed at, which is worse than the bias it corrects.  Opt in, do not opt out.
bool g_aiPlayerThreatFairness   = false;
bool g_aiPlayerAwarenessFairness = true;

namespace {

// One patched site: verified original bytes -> replacement bytes.
struct PatchSite {
   uint8_t* addr;
   uint8_t  orig[6];
   size_t   len;
};

PatchSite s_vision   = {};
PatchSite s_priority = {};
PatchSite s_threat   = {};
PatchSite s_raytest  = {};

// Verify `len` bytes at the resolved VA match `expect`, and if so write
// `replace` over them.  Bails (leaving .text untouched) on any mismatch, so a
// wrong address on a build we have not derived no-ops instead of corrupting
// code.  .text is RW during the install window — no VirtualProtect needed.
bool patch_verified(uintptr_t exe_base, uintptr_t va, const uint8_t* expect,
                    const uint8_t* replace, size_t len, PatchSite& out)
{
   if (va == 0) return false;

   uint8_t* p = (uint8_t*)resolve(exe_base, va);
   if (std::memcmp(p, expect, len) != 0) return false;

   std::memcpy(out.orig, p, len);
   out.addr = p;
   out.len  = len;
   std::memcpy(p, replace, len);
   return true;
}

void restore(PatchSite& s)
{
   if (!s.addr) return;
   DWORD oldProt;
   if (VirtualProtect(s.addr, s.len, PAGE_EXECUTE_READWRITE, &oldProt)) {
      std::memcpy(s.addr, s.orig, s.len);
      VirtualProtect(s.addr, s.len, oldProt, &oldProt);
   }
   s.addr = nullptr;
}

} // namespace

void ai_fairness_install(uintptr_t exe_base)
{
   // ---- #1 MaxVisibleDist: drop the 2x player view range --------------------
   if (g_aiPlayerVisionFairness) {
      uintptr_t va = 0;
      // Both builds guard the doubling with `JL <convergence>`; only the
      // displacement differs, so the expected bytes differ per build.
      static const uint8_t kJlModtools[] = {0x7C, 0x0C};
      static const uint8_t kJmModtools[] = {0xEB, 0x0C};
      static const uint8_t kJlSteam[]    = {0x7C, 0x12};
      static const uint8_t kJmSteam[]    = {0xEB, 0x12};

      const uint8_t *expect = nullptr, *replace = nullptr;
      switch (g_build) {
      case GameBuild::Modtools:
         va = game_addrs::modtools::vision_maxdist_player_jl;
         expect = kJlModtools; replace = kJmModtools;
         break;
      case GameBuild::Steam:
         va = game_addrs::steam::vision_maxdist_player_jl;
         expect = kJlSteam; replace = kJmSteam;
         break;
      case GameBuild::GOG:
         va = game_addrs::gog::vision_maxdist_player_jl;
         expect = kJlSteam; replace = kJmSteam; // identical codegen to Steam
         break;
      default:
         break;
      }
      if (expect) patch_verified(exe_base, va, expect, replace, 2, s_vision);
   }

   // ---- #2 GetVisualPriority: double the player's cost like everyone else ---
   if (g_aiPlayerPriorityFairness) {
      uintptr_t va = 0;
      static const uint8_t kJzModtools[]  = {0x74, 0x13};
      static const uint8_t kNopModtools[] = {0x90, 0x90};
      static const uint8_t kJlSteam[]     = {0x7C, 0x1D};
      static const uint8_t kJmSteam[]     = {0xEB, 0x1D};

      const uint8_t *expect = nullptr, *replace = nullptr;
      switch (g_build) {
      case GameBuild::Modtools:
         va = game_addrs::modtools::vision_priority_player_jz;
         expect = kJzModtools; replace = kNopModtools;
         break;
      case GameBuild::Steam:
         va = game_addrs::steam::vision_priority_player_jl;
         expect = kJlSteam; replace = kJmSteam;
         break;
      case GameBuild::GOG:
         va = game_addrs::gog::vision_priority_player_jl;
         expect = kJlSteam; replace = kJmSteam; // identical codegen to Steam
         break;
      default:
         break;
      }
      if (expect) patch_verified(exe_base, va, expect, replace, 2, s_priority);
   }

   // ---- #3 Threat::GetPriority: score the player like a bot ------------------
   // The one that keeps AI walking past each other once you make eye contact.
   // See the note above the file's #3 section.
   if (g_aiPlayerThreatFairness) {
      uintptr_t va = 0;
      // JZ rel32 -> JMP rel32 + NOP.  The JMP is one byte shorter, so its
      // displacement is the JZ's + 1 to land on the same target.
      static const uint8_t kJzModtools[]  = {0x0F, 0x84, 0x8E, 0x00, 0x00, 0x00};
      static const uint8_t kJmpModtools[] = {0xE9, 0x8F, 0x00, 0x00, 0x00, 0x90};
      static const uint8_t kJzSteam[]     = {0x0F, 0x84, 0x99, 0x00, 0x00, 0x00};
      static const uint8_t kJmpSteam[]    = {0xE9, 0x9A, 0x00, 0x00, 0x00, 0x90};

      const uint8_t *expect = nullptr, *replace = nullptr;
      switch (g_build) {
      case GameBuild::Modtools:
         va = game_addrs::modtools::threat_priority_player_jz;
         expect = kJzModtools; replace = kJmpModtools;
         break;
      case GameBuild::Steam:
         va = game_addrs::steam::threat_priority_player_jz;
         expect = kJzSteam; replace = kJmpSteam;
         break;
      case GameBuild::GOG:
         va = game_addrs::gog::threat_priority_player_jz;
         expect = kJzSteam; replace = kJmpSteam; // identical codegen to Steam
         break;
      default:
         break;
      }
      if (expect) patch_verified(exe_base, va, expect, replace, 6, s_threat);
   }

   // ---- #4 ShouldRaytestUnit: stop the AI going blind to everyone else -------
   // Identical `74 0B` -> `EB 0B` on all three builds.
   if (g_aiPlayerAwarenessFairness) {
      uintptr_t va = 0;
      static const uint8_t kJz[]  = {0x74, 0x0B};
      static const uint8_t kJmp[] = {0xEB, 0x0B};

      switch (g_build) {
      case GameBuild::Modtools: va = game_addrs::modtools::threat_raytest_player_jz; break;
      case GameBuild::Steam:    va = game_addrs::steam::threat_raytest_player_jz;    break;
      case GameBuild::GOG:      va = game_addrs::gog::threat_raytest_player_jz;      break;
      default: break;
      }
      if (va) patch_verified(exe_base, va, kJz, kJmp, 2, s_raytest);
   }
}

void ai_fairness_uninstall()
{
   restore(s_vision);
   restore(s_priority);
   restore(s_threat);
   restore(s_raytest);
}
