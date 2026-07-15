#include "pch.h"
#include "land_on_arrival_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>

// =============================================================================
// LandOnArrival path-node property fix (ported from RJP1992's dinput-hook
// branch, land_on_arrival_patch.cpp; all addresses re-verified per build)
// =============================================================================
// The .pth NodeProperties.mLandOnArrival flag (+0x22) is parsed for every
// node, but two engine bugs stop it from working as authored:
//
//   1. EntityPathFollower::Update only checks it while mIndex < 1.  The
//      source logic (recovered from the Phantom build, 0x55d380):
//
//         if (mPath && mIndex < 1 && mIndex < GetNumPoints(mPath) &&
//             GetNodeProperties(mPath, mIndex)->mLandOnArrival)
//            mbLandNow = true;
//
//      The `mIndex < 1` test compiles to a JG right after `cmp mIndex, 0`
//      (modtools 0x5ED340 `7F 25`, Steam/GOG 0x4D52FD `7F 42`).  NOPing the
//      JG makes arrival at ANY node honor the flag.
//
//   2. mbLandNow (EntityPathFollower+0xD1; EntityFlyer+0x4A1 modtools /
//      +0x49D release via mPathFollower at +0x3CC) is never cleared once
//      set, so EntityFlyer::Land re-triggers every frame and the flyer can
//      never take off again.  We detour Land(): clear mbLandNow after the
//      original runs, and — when the landing was path-triggered — call
//      EntityPathFollower::Reset so path control releases the vehicle and
//      AI/players can enter it.  (Reset's pathClass argument is dead: the
//      release builds never read it, and in the source family it's only
//      forwarded to RemovePathRef, which ignores it — Phantom 0x55b810.
//      Passing null is safe on all three builds.)
// =============================================================================

// ---------------------------------------------------------------------------
// Per-build address set
// ---------------------------------------------------------------------------
struct LandOnArrivalAddrs {
   uintptr_t jg_site;          // JG instruction gating the check to node 0
   uint8_t   jg_disp;          // expected JG rel8 displacement (site = 7F disp)
   uintptr_t land_func;        // EntityFlyer::Land
   uintptr_t reset_func;       // EntityPathFollower::Reset
   unsigned  mbLandNow_off;    // EntityFlyer -> mbLandNow
   unsigned  pathFollower_off; // EntityFlyer -> embedded EntityPathFollower
};

static constexpr LandOnArrivalAddrs MODTOOLS_ADDRS = {
   game_addrs::modtools::path_follower_land_jg, 0x25,
   game_addrs::modtools::entity_flyer_land,
   game_addrs::modtools::path_follower_reset,
   0x4A1,   // 0x3CC + 0xD5 (debug TargetInfo-style +4 shift vs release)
   0x3CC,
};

static constexpr LandOnArrivalAddrs STEAM_ADDRS = {
   game_addrs::steam::path_follower_land_jg, 0x42,
   game_addrs::steam::entity_flyer_land,
   game_addrs::steam::path_follower_reset,
   0x49D,   // 0x3CC + 0xD1 (verified: `mov byte [edi+0xD1],1` at 0x4D5330)
   0x3CC,
};

static constexpr LandOnArrivalAddrs GOG_ADDRS = {
   game_addrs::gog::path_follower_land_jg, 0x42,
   game_addrs::gog::entity_flyer_land,
   game_addrs::gog::path_follower_reset,
   0x49D,
   0x3CC,
};

// ---------------------------------------------------------------------------
// EntityFlyer::Land detour
// ---------------------------------------------------------------------------
using fn_Land          = void(__thiscall*)(void* flyer);
using fn_ResetFollower = void(__thiscall*)(void* pathFollower, void* pathClass);

static fn_Land          original_Land     = nullptr;
static fn_ResetFollower s_resetFollower   = nullptr;
static unsigned         s_mbLandNowOff    = 0;
static unsigned         s_pathFollowerOff = 0;

static uint8_t* s_jgSite    = nullptr;
static uint8_t  s_jgDisp    = 0;
static bool     s_jgPatched = false;

static void __fastcall hooked_Land(void* flyer, void* /*edx*/)
{
   // mbLandNow set before the call means this landing was LandOnArrival-
   // triggered (player/AI landings go through Land without setting it).
   uint8_t* pLandNow = (uint8_t*)flyer + s_mbLandNowOff;
   const bool wasLandOnArrival = (*pLandNow != 0);

   original_Land(flyer);

   // Clear mbLandNow so the flyer doesn't re-land every frame.
   *pLandNow = 0;

   // Path-triggered landing: detach the path follower so the vehicle is
   // released from path control.
   if (wasLandOnArrival && s_resetFollower)
      s_resetFollower((uint8_t*)flyer + s_pathFollowerOff, nullptr);
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------
void land_on_arrival_install(uintptr_t exe_base)
{
   const LandOnArrivalAddrs* addrs = nullptr;
   switch (g_build) {
      case GameBuild::Modtools: addrs = &MODTOOLS_ADDRS; break;
      case GameBuild::Steam:    addrs = &STEAM_ADDRS;    break;
      case GameBuild::GOG:      addrs = &GOG_ADDRS;      break;
      default:                  return; // unsupported build
   }

   // 1. NOP the mIndex JG so every node's LandOnArrival is honored.
   //    Verify the site still holds the expected `7F disp` first.
   s_jgSite = (uint8_t*)resolve(exe_base, addrs->jg_site);
   s_jgDisp = addrs->jg_disp;
   if (s_jgSite[0] == 0x7F && s_jgSite[1] == addrs->jg_disp) {
      DWORD oldProt;
      if (VirtualProtect(s_jgSite, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
         s_jgSite[0] = 0x90;
         s_jgSite[1] = 0x90;
         VirtualProtect(s_jgSite, 2, oldProt, &oldProt);
         s_jgPatched = true;
      }
   }
   if (!s_jgPatched)
      return; // byte mismatch (patched exe?) — leave Land un-hooked too

   // 2. Detour EntityFlyer::Land to clear mbLandNow + release the path.
   s_resetFollower   = (fn_ResetFollower)resolve(exe_base, addrs->reset_func);
   s_mbLandNowOff    = addrs->mbLandNow_off;
   s_pathFollowerOff = addrs->pathFollower_off;
   original_Land     = (fn_Land)resolve(exe_base, addrs->land_func);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_Land, hooked_Land);
   if (DetourTransactionCommit() != NO_ERROR)
      original_Land = nullptr;
}

void land_on_arrival_uninstall()
{
   if (s_jgPatched) {
      DWORD oldProt;
      if (VirtualProtect(s_jgSite, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
         s_jgSite[0] = 0x7F;
         s_jgSite[1] = s_jgDisp;
         VirtualProtect(s_jgSite, 2, oldProt, &oldProt);
      }
      s_jgPatched = false;
   }

   if (original_Land) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(&(PVOID&)original_Land, hooked_Land);
      DetourTransactionCommit();
      original_Land = nullptr;
   }
}
