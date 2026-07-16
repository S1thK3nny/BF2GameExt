#include "pch.h"
#include "hover_pilot_null_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstring>

// =============================================================================
// EntityHover self-piloted crash fix.
//
// Crash (confirmed on modtools BattlefrontII.Debug build):
//   C0000005 READ addr 0x18, EAX=0 at EIP 0x00515E3E, inside
//   EntityHover::UpdateIndirect (0x515cc0).
//
// The AI obstacle-avoidance block builds a 2-entry ignore list for a collision
// raycast: { the hover itself, the hover's pilot }.  It gets the pilot via
// FUN_004d49f0 (returns Controllable::mPilot, or null when PilotType is self /
// vehicleself-on-self) and then dereferences it with no guard:
//
//   00515E39  E8 rel32       CALL   get_active_pilot        ; EAX = pilot | 0
//   00515E3E  8B 50 18       MOV    EDX,[EAX+0x18]          ; <-- AV when EAX=0
//   00515E41  8D 48 18       LEA    ECX,[EAX+0x18]
//   00515E44  FF 52 20       CALL   [EDX+0x20]              ; pilot->GetGameObject()
//
// Stock hovers are all soldier-entered (PilotType=vehicle) so mPilot is always
// valid; a self-piloted hover has none, get_active_pilot returns null, and the
// deref faults.  It's the AI-drive path, so it happens in SP and MP.
//
// Fix: point the CALL at a tiny shim that runs the real getter and, when it
// returns null, substitutes the hover's own Controllable (the getter's ECX arg).
// GetGameObject() then resolves to the hover itself, which is merely added to
// the raycast ignore list a second time — harmless.  The hover's Controllable
// has the same layout as a pilot Controllable, so [self+0x18]->vtable[0x20] is
// the identical virtual call.
//
// Always on (no INI toggle): a crash-only-on-a-specific-ODF-value fix nobody
// would want disabled.
//
// TODO(self-piloted-hover): a *user command* issued to a self-piloted hover
// (e.g. "stop" / "halt") also crashes the game. A separate unguarded null
// pilot deref on a different code path than UpdateIndirect.  Not yet located;
// this fix does NOT cover it.  Find and guard that site too.
// =============================================================================

// Resolved address of the game's pilot getter (FUN_004d49f0): __fastcall, takes
// the hover Controllable in ECX, returns mPilot or null.  Set by install.
static uintptr_t s_getActivePilot = 0;

// Patched call site + its original rel32 operand (for uninstall).
static uint8_t* s_site    = nullptr;
static int32_t  s_origRel = 0;

// The unguarded pilot->GetGameObject() idiom that must immediately follow the
// CALL — used to positively identify the site before patching (so a wrong
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

void hover_pilot_null_fix_install(uintptr_t exe_base)
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
   default:
      return; // GOG / unknown: addresses not derived yet
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

void hover_pilot_null_fix_uninstall()
{
   if (!s_site) return;
   DWORD oldProt;
   if (VirtualProtect(s_site + 1, sizeof(int32_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
      *(int32_t*)(s_site + 1) = s_origRel;
      VirtualProtect(s_site + 1, sizeof(int32_t), oldProt, &oldProt);
   }
   s_site = nullptr;
}
