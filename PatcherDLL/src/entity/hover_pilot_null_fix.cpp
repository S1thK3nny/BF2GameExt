#include "pch.h"
#include "hover_pilot_null_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstring>

// =============================================================================
// EntityHover self-piloted crash fixes.
//
// A hover vehicle with PilotType=self in its ODF crashes the game, because
// several code paths fetch the vehicle's pilot and dereference it with no null
// check.  Every stock hover is soldier-entered (PilotType=vehicle) so the pilot
// is always valid and these were never guarded; a self-piloted hover has none.
// Two independent crash sites, both patched here (always on, no INI toggle):
//
//   1. EntityHover::UpdateIndirect (AI obstacle-avoidance).  Calls a "get pilot,
//      or null if self-piloted" getter and immediately does pilot->GetGameObject()
//      to exclude it from a collision raycast:
//        modtools 0x515e39  E8 rel32       CALL  get_active_pilot   ; EAX=pilot|0
//                 0x515e3e  8B 50 18       MOV   EDX,[EAX+0x18]     ; AV when EAX=0
//                 ...       8D 48 18 / FF 52 20 -> pilot->GetGameObject()
//      Fix: redirect the getter CALL to a shim that substitutes the hover itself
//      when the getter returns null (harmless double entry in the ignore list).
//      Fixed on modtools + Steam.
//
//   2. EntitySoldier::Update (modtools 0x549bf8) — the event-0x1a/0x1b order-
//      acknowledge block that runs when a unit is given an order.  It walks
//      target->[0x20C]->[0x0C]->pilot(+0xCC)->[0x148]->GetGameObject(); the pilot
//      link is null for a self-piloted hover:
//        0x549bf2  8B BF CC000000  MOV EDI,[EDI+0xCC]   ; pilot (null)
//        0x549bf8  8B BF 48010000  MOV EDI,[EDI+0x148]  ; AV read 0x148
//      Fix: overwrite the 6-byte load with a jump to a guard that, when the pilot
//      is null, jumps to the block's convergence point, skipping the acknowledge.
//      Fixed on modtools + Steam (registers and pending-stack state differ per
//      build - see the two guards below).
// =============================================================================

// -----------------------------------------------------------------------------
// #1 — EntityHover::UpdateIndirect getter shim.
// -----------------------------------------------------------------------------
// Resolved address of the game's pilot getter (modtools FUN_004d49f0 / Steam
// FUN_0043aad0): __fastcall, takes the hover Controllable in ECX, returns
// mPilot or null.  Set by install.
static uintptr_t s_getActivePilot = 0;

// Patched call site + its original rel32 operand (for uninstall).
static uint8_t* s_site    = nullptr;
static int32_t  s_origRel = 0;

// The unguarded pilot->GetGameObject() idiom that must immediately follow the
// CALL - used to positively identify the site before patching (so a wrong
// address on an un-derived build no-ops instead of corrupting code).  Same
// semantics on both builds, different codegen:
//   modtools: MOV EDX,[EAX+0x18]; LEA ECX,[EAX+0x18];   CALL [EDX+0x20]
//   Steam:    LEA ECX,[EAX+0x18]; MOV EAX,[ECX]; MOV EAX,[EAX+0x20]; CALL EAX
static const uint8_t kIdiomModtools[] = {
   0x8B, 0x50, 0x18,  // MOV EDX,[EAX+0x18]
   0x8D, 0x48, 0x18,  // LEA ECX,[EAX+0x18]
   0xFF, 0x52, 0x20,  // CALL [EDX+0x20]
};
static const uint8_t kIdiomSteam[] = {
   0x8D, 0x48, 0x18,  // LEA ECX,[EAX+0x18]
   0x8B, 0x01,        // MOV EAX,[ECX]
   0x8B, 0x40, 0x20,  // MOV EAX,[EAX+0x20]
   0xFF, 0xD0,        // CALL EAX
};

// __fastcall shim: ECX = the hover Controllable (the getter's argument).
// Calls the real getter; on a null return (self-piloted) yields the hover
// itself instead, so the caller's GetGameObject() can't fault.
__declspec(naked) static void hover_pilot_getter_guarded()
{
   __asm {
      push ecx                    // preserve the hover Controllable (getter arg)
      mov  eax, [s_getActivePilot]
      call eax                    // __fastcall: ECX in, EAX = mPilot | 0
      pop  ecx                    // restore the hover Controllable
      test eax, eax
      jnz  done
      mov  eax, ecx               // self-piloted: use the hover itself
   done:
      ret
   }
}

static void install_updateindirect_shim(uintptr_t exe_base)
{
   uintptr_t     callSiteVA, getterVA;
   const uint8_t* idiom;
   size_t         idiomLen;
   switch (g_build) {
   case GameBuild::Modtools:
      callSiteVA = game_addrs::modtools::hover_updateindirect_pilot_call;
      getterVA   = game_addrs::modtools::controllable_get_active_pilot;
      idiom = kIdiomModtools; idiomLen = sizeof(kIdiomModtools);
      break;
   case GameBuild::Steam:
      callSiteVA = game_addrs::steam::hover_updateindirect_pilot_call;
      getterVA   = game_addrs::steam::controllable_get_active_pilot;
      idiom = kIdiomSteam; idiomLen = sizeof(kIdiomSteam);
      break;
   case GameBuild::GOG:
      callSiteVA = game_addrs::gog::hover_updateindirect_pilot_call;
      getterVA   = game_addrs::gog::controllable_get_active_pilot;
      idiom = kIdiomSteam; idiomLen = sizeof(kIdiomSteam); // same codegen as Steam
      break;
   default:
      return; // unknown build
   }
   if (callSiteVA == 0 || getterVA == 0) return;

   uint8_t* site = (uint8_t*)resolve(exe_base, callSiteVA);

   // Positive-ID the site: `E8 rel32` (CALL) immediately followed by the
   // build's unguarded GetGameObject idiom.  Bail (no-op) on any mismatch.
   if (site[0] != 0xE8) return;
   if (std::memcmp(site + 5, idiom, idiomLen) != 0) return;

   s_getActivePilot = (uintptr_t)resolve(exe_base, getterVA);

   // Redirect the CALL to our shim.  rel32 is relative to the next instruction.
   // .text is RW during install (dllmain re-protects afterwards), so no
   // VirtualProtect is needed here.
   s_site    = site;
   s_origRel = *(int32_t*)(site + 1);
   *(int32_t*)(site + 1) =
      (int32_t)((uintptr_t)&hover_pilot_getter_guarded - ((uintptr_t)site + 5));
}

// -----------------------------------------------------------------------------
// #2 — EntitySoldier::Update order-acknowledge null-pilot guard.
// -----------------------------------------------------------------------------
// The crash is a 6-byte `MOV reg,[reg+0x148]` where reg already holds the null
// pilot link loaded by the preceding `MOV reg,[reg+0xCC]`.  We overwrite the load
// with a jump to a build-specific guard that, on null, jumps to the block's
// convergence point (the GetNumCameras()==0 branch target), skipping the whole
// order-acknowledge; otherwise it runs the original load and continues.
//
// The two builds differ in registers and in stack state at the crash:
//   modtools 0x549bf8: null in EDI; `MOV EDI,[EDI+0x148]`; the preceding 0x1a-
//     event call's 5 args (0x14 bytes) are still pending here (cleaned by an
//     ADD ESP,0x14 *after* the crash), so the skip must discard them first,
//     then jump to 0x549cfb.
//   Steam 0x4ece30:    null in EAX; `MOV ECX,[EAX+0x148]`; the preceding call's
//     args were already cleaned before the chain, so ESP is at block level and
//     the skip is a bare jump to 0x4ecb1e.
static void*    s_cmdContinue = nullptr; // crash_site + 6 (non-null path)
static void*    s_cmdSkip     = nullptr; // block convergence point
static uint8_t* s_cmdSite     = nullptr;
static uint8_t  s_cmdOrig[6]  = {};

__declspec(naked) static void hover_cmd_null_guard_modtools()
{
   __asm {
      test edi, edi
      jz   skip
      mov  edi, [edi + 0x148]     // original overwritten instruction
      jmp  [s_cmdContinue]
   skip:
      add  esp, 0x14              // discard the pending 0x1a-call args
      jmp  [s_cmdSkip]
   }
}

__declspec(naked) static void hover_cmd_null_guard_steam()
{
   __asm {
      test eax, eax
      jz   skip
      mov  ecx, [eax + 0x148]     // original overwritten instruction
      jmp  [s_cmdContinue]
   skip:
      jmp  [s_cmdSkip]            // no pending args on Steam
   }
}

static void install_command_guard(uintptr_t exe_base)
{
   uintptr_t siteVA, skipVA;
   const uint8_t* kSite;
   const uint8_t* kPrev;
   void*     guard;

   // Crash-site signature = `MOV reg,[reg+0x148]`; preceding-load signature =
   // `MOV reg,[reg+0xCC]` (the null pilot link).  Registers differ per build.
   static const uint8_t kSiteModtools[] = {0x8B, 0xBF, 0x48, 0x01, 0x00, 0x00}; // MOV EDI,[EDI+0x148]
   static const uint8_t kPrevModtools[] = {0x8B, 0xBF, 0xCC, 0x00, 0x00, 0x00}; // MOV EDI,[EDI+0xCC]
   static const uint8_t kSiteSteam[]    = {0x8B, 0x88, 0x48, 0x01, 0x00, 0x00}; // MOV ECX,[EAX+0x148]
   static const uint8_t kPrevSteam[]    = {0x8B, 0x80, 0xCC, 0x00, 0x00, 0x00}; // MOV EAX,[EAX+0xCC]

   switch (g_build) {
   case GameBuild::Modtools:
      siteVA = game_addrs::modtools::hover_command_crash_site;
      skipVA = game_addrs::modtools::hover_command_skip_target;
      kSite = kSiteModtools; kPrev = kPrevModtools;
      guard = (void*)&hover_cmd_null_guard_modtools;
      break;
   case GameBuild::Steam:
      siteVA = game_addrs::steam::hover_command_crash_site;
      skipVA = game_addrs::steam::hover_command_skip_target;
      kSite = kSiteSteam; kPrev = kPrevSteam;
      guard = (void*)&hover_cmd_null_guard_steam;
      break;
   case GameBuild::GOG:
      siteVA = game_addrs::gog::hover_command_crash_site;
      skipVA = game_addrs::gog::hover_command_skip_target;
      kSite = kSiteSteam; kPrev = kPrevSteam; // identical codegen to Steam
      guard = (void*)&hover_cmd_null_guard_steam;
      break;
   default:
      return; // unknown build
   }
   if (siteVA == 0 || skipVA == 0) return;

   uint8_t* site = (uint8_t*)resolve(exe_base, siteVA);

   // Positive-ID the site and its preceding null-pilot load.  Bail on mismatch.
   if (std::memcmp(site, kSite, 6) != 0) return;
   if (std::memcmp(site - 6, kPrev, 6) != 0) return;

   s_cmdContinue = (void*)(site + 6);
   s_cmdSkip     = resolve(exe_base, skipVA);
   s_cmdSite     = site;
   std::memcpy(s_cmdOrig, site, sizeof(s_cmdOrig));

   // E9 rel32 (JMP to guard) over bytes 0..4, NOP the 6th byte.  .text is RW
   // during install.
   int32_t rel = (int32_t)((uintptr_t)guard - ((uintptr_t)site + 5));
   site[0] = 0xE9;
   *(int32_t*)(site + 1) = rel;
   site[5] = 0x90;
}

// -----------------------------------------------------------------------------
// Guard 3: cargo-activation mPilot deref (release builds only).
//
// Despite the file name this is the same engine bug as the two above — an
// unguarded Controllable::mPilot deref — just reached from a different place,
// so it lives with the other two rather than in its own module.
//
// EntityCarrier cargo activation calls cargo vtable[5] (0x500410), which walks
// the object's children and virtual-calls each one via `CALL [EAX+0x7c]`
// (0x500592).  That callee (0x5005c0) takes the "has a pilot" branch whenever
// [ESI+0xd4] >= 0 and then dereferences mPilot with no null check:
//     00500634  MOV  EAX,[ESI+0xd0]      ; mPilot -> NULL for carried cargo
//     0050063a  PUSH [EAX+0xd4]          ; access violation
// Its sibling branch (0x500647) checks mPilot and jumps to 0x5006dd when null,
// so that is exactly where this guard sends the null case — the engine's own
// no-pilot route, reached at the same block-level ESP.
//
// Release-only: the byte pair does not occur anywhere in the modtools .text.
// Without the guard the fault is merely contained by the __try in
// flyer_carrier_fixes.cpp, which means ActivatePhysics aborts partway and the
// cargo is left only partially activated.
static void*    s_actContinue  = nullptr; // crash_site + 12 (non-null path)
static void*    s_actSkip      = nullptr; // engine's own no-pilot convergence
static uint8_t* s_actSite      = nullptr;
static uint8_t  s_actOrig[12]  = {};

__declspec(naked) static void activate_pilot_null_guard()
{
   __asm {
      mov  eax, [esi + 0xd0]        // original overwritten instruction 1
      test eax, eax
      jz   skip
      push dword ptr [eax + 0xd4]   // original overwritten instruction 2
      jmp  [s_actContinue]
   skip:
      jmp  [s_actSkip]              // block-level ESP, same as the engine's path
   }
}

static void install_activate_guard(uintptr_t exe_base)
{
   // Release-only; modtools does not emit this shape at all.
   if (g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;

   const uintptr_t siteVA = g_addr->activate_pilot_crash_site;
   const uintptr_t skipVA = g_addr->activate_pilot_skip_target;
   if (siteVA == 0 || skipVA == 0) return;

   uint8_t* site = (uint8_t*)resolve(exe_base, siteVA);
   uint8_t* skip = (uint8_t*)resolve(exe_base, skipVA);

   // Positive-ID: the 12-byte deref pair, the `LEA ECX,[ESI+0x18]` that precedes
   // it, and `MOV ECX,ESI` at the skip target.  Bail (no-op) on any mismatch.
   static const uint8_t kSite[] = {0x8B, 0x86, 0xD0, 0x00, 0x00, 0x00,
                                   0xFF, 0xB0, 0xD4, 0x00, 0x00, 0x00};
   static const uint8_t kPrev[] = {0x8D, 0x4E, 0x18};   // LEA ECX,[ESI+0x18]
   static const uint8_t kSkip[] = {0x8B, 0xCE};         // MOV ECX,ESI

   if (std::memcmp(site, kSite, sizeof(kSite)) != 0) return;
   if (std::memcmp(site - 3, kPrev, sizeof(kPrev)) != 0) return;
   if (std::memcmp(skip, kSkip, sizeof(kSkip)) != 0) return;

   s_actContinue = (void*)(site + sizeof(kSite));
   s_actSkip     = skip;
   s_actSite     = site;
   std::memcpy(s_actOrig, site, sizeof(s_actOrig));

   // E9 rel32 to the guard over bytes 0..4, NOP the remaining 7.  .text is RW
   // during install.
   const int32_t rel = (int32_t)((uintptr_t)&activate_pilot_null_guard - ((uintptr_t)site + 5));
   site[0] = 0xE9;
   *(int32_t*)(site + 1) = rel;
   std::memset(site + 5, 0x90, sizeof(kSite) - 5);
}

// -----------------------------------------------------------------------------
// Public install / uninstall.
// -----------------------------------------------------------------------------
void hover_pilot_null_fix_install(uintptr_t exe_base)
{
   install_updateindirect_shim(exe_base);
   install_command_guard(exe_base);
   install_activate_guard(exe_base);
}

void hover_pilot_null_fix_uninstall()
{
   if (s_site) {
      DWORD oldProt;
      if (VirtualProtect(s_site + 1, sizeof(int32_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
         *(int32_t*)(s_site + 1) = s_origRel;
         VirtualProtect(s_site + 1, sizeof(int32_t), oldProt, &oldProt);
      }
      s_site = nullptr;
   }
   if (s_cmdSite) {
      DWORD oldProt;
      if (VirtualProtect(s_cmdSite, sizeof(s_cmdOrig), PAGE_EXECUTE_READWRITE, &oldProt)) {
         std::memcpy(s_cmdSite, s_cmdOrig, sizeof(s_cmdOrig));
         VirtualProtect(s_cmdSite, sizeof(s_cmdOrig), oldProt, &oldProt);
      }
      s_cmdSite = nullptr;
   }
   if (s_actSite) {
      DWORD oldProt;
      if (VirtualProtect(s_actSite, sizeof(s_actOrig), PAGE_EXECUTE_READWRITE, &oldProt)) {
         std::memcpy(s_actSite, s_actOrig, sizeof(s_actOrig));
         VirtualProtect(s_actSite, sizeof(s_actOrig), oldProt, &oldProt);
      }
      s_actSite = nullptr;
   }
}
