#include "pch.h"
#include "jetpack_fp_sound_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstring>

// =============================================================================
// Jetpack sound lost on switching to first person.
//
// Traced in the Phantom build, where the whole path is symbolized.
//
// The jetpack's continuous sound is a VehicleEngine embedded in EntitySoldier
// (+0x3D4 on every build), configured from the VehicleEngineClass inside
// EntitySoldierClass (EngineSound / TurnOnSound / TurnOffSound in the ODF).
// It is started by EntitySoldier::JetJump:
//
//     if (!VehicleEngine::IsOn(&mJetEngine))
//         VehicleEngine::TurnOn(&mJetEngine, mClass->mJetEngine, pos, vel, fuel);
//
// and stopped from EntitySoldier::Update, which calls VehicleEngine::TurnOff
// every frame the soldier is not in a jet state.  That repetition matters:
// TurnOff is staged.  The first call runs StartTurningOff, which plays the
// turn-off sound and sets the "turning off" flag but leaves the engine on; only
// once mTimer has passed mTurnOffTime does a later call actually stop the loop
// and clear the on flag.  VehicleEngine::Update advances mTimer and re-calls
// TurnOff for as long as the engine reads as on.
//
// EntitySoldier::SetFirstPersonView breaks into the middle of that:
//
//     if (enteringFirstPerson) {
//         TurnOffJetEffect(soldier);        // <- kills the engine AND the effect
//         TurnOffJetIdleEffect(soldier);    // <- effect only, fine
//         ...
//     }
//
// TurnOffJetEffect is two separate teardowns welded together:
//
//     if (VehicleEngine::IsOn(&mJetEngine))                      // the SOUND
//         VehicleEngine::TurnOff(&mJetEngine, mClass->mJetEngine, pos, vel);
//     if (mJetEffectHandle.mObject) {                            // the EXHAUST
//         if (handle still valid) mJetEffectHandle->Destroy();
//         mJetEffectHandle = {};
//     }
//
// Only the exhaust needs to go when the view changes - it is attached to the
// third person model, which is not drawn in first person.  Killing the engine
// with it is what the player hears: the turn-off sound fires immediately, and
// because JetJump only starts the engine from a standing/landed state, nothing
// starts it again for the rest of that jet.
//
// The fix redirects the one call site in SetFirstPersonView to a shim that does
// the effect half only.  The engine keeps running and is still shut down
// normally by EntitySoldier::Update once the jet actually stops, so the sound
// lifecycle is otherwise untouched - as is every other TurnOffJetEffect caller
// (Render, EnterControllable, TurnOffEffects, the net read paths), which do want
// both halves.
//
// Call sites (`E8 rel32`, ECX = the EntitySoldier base):
//
//   modtools 0x53CC74  CALL 0x40DE63 -> EntitySoldier::TurnOffJetEffect 0x535C90
//                      preceded by LEA EDI,[ESI-0x258] ; MOV ECX,EDI
//   Steam/GOG 0x4F36F3 CALL 0x4E2300 (release calls it directly, no ILT thunk)
//                      preceded by LEA ECX,[EDI-0x258]
//
// mJetEffectHandle sits at a different offset on the release layout, in line
// with the rest of the EntitySoldier block (see core/entity_layout.hpp):
//
//   modtools  mObject +0xA68  mSavedHandleId +0xA6C
//   Steam/GOG mObject +0xA4C  mSavedHandleId +0xA50
// =============================================================================

// Byte offsets of mJetEffectHandle for the active build; filled by install.
static uint32_t g_offHandleObject = 0;
static uint32_t g_offHandleId     = 0;

// Patched call site and its original rel32, for uninstall.
static uint8_t* g_site    = nullptr;
static int32_t  g_origRel = 0;

// __thiscall shim, ECX = EntitySoldier*.  The effect half of TurnOffJetEffect:
// destroy the jet exhaust if its handle is still live, then clear the handle.
// vtable slot 0x20 is the FLEffectObject teardown the engine itself calls, and
// the +0x1C compare is the engine's own handle-still-valid test.
__declspec(naked) static void jet_effect_teardown_only()
{
   __asm {
      push esi
      push ebx
      mov  esi, ecx                  // EntitySoldier*
      mov  ebx, [g_offHandleObject]
      mov  ecx, [esi + ebx]          // mJetEffectHandle.mObject
      test ecx, ecx
      jz   clear
      mov  eax, [ecx + 0x1c]
      mov  edx, [g_offHandleId]
      cmp  eax, [esi + edx]          // still the object we stored?
      jnz  clear
      mov  eax, [ecx]
      call dword ptr [eax + 0x20]    // FLEffectObject teardown (thiscall, ECX = obj)
   clear:
      mov  ebx, [g_offHandleId]
      mov  dword ptr [esi + ebx], 0
      mov  ebx, [g_offHandleObject]
      mov  dword ptr [esi + ebx], 0
      pop  ebx
      pop  esi
      ret
   }
}

void jetpack_fp_sound_fix_install(uintptr_t exe_base)
{
   const uint8_t* tail;      // what must follow the call, to positively ID it
   size_t         tailLen;

   // The second teardown call in SetFirstPersonView (the idle effect), which
   // always follows the one we patch.
   static const uint8_t kTailModtools[] = {0x8B, 0xCF, 0xE8};                    // MOV ECX,EDI ; CALL
   static const uint8_t kTailRetail[]   = {0x8D, 0x8F, 0xA8, 0xFD, 0xFF, 0xFF,   // LEA ECX,[EDI-0x258]
                                           0xE8};                                // CALL

   switch (g_build) {
   case GameBuild::Modtools:
      g_offHandleObject = 0xA68; g_offHandleId = 0xA6C;
      tail = kTailModtools; tailLen = sizeof(kTailModtools);
      break;
   case GameBuild::Steam:
   case GameBuild::GOG:
      g_offHandleObject = 0xA4C; g_offHandleId = 0xA50;
      tail = kTailRetail;   tailLen = sizeof(kTailRetail);
      break;
   default:
      return; // unknown build
   }

   if (g_addr->soldier_setfp_jet_effect_call == 0 ||
       g_addr->soldier_turnoff_jet_effect == 0)
      return;

   uint8_t* site = (uint8_t*)resolve(exe_base, g_addr->soldier_setfp_jet_effect_call);

   // Positive-ID: a CALL, landing exactly on TurnOffJetEffect, followed by the
   // idle-effect call.  Bail (no-op) on any mismatch.
   const uint8_t* expected = (const uint8_t*)resolve(exe_base, g_addr->soldier_turnoff_jet_effect);
   if (site[0] != 0xE8 ||
       site + 5 + *(int32_t*)(site + 1) != expected ||
       std::memcmp(site + 5, tail, tailLen) != 0) {
      get_gamelog()("[JetpackFPSoundFix] unexpected bytes at SetFirstPersonView call, skipping\n");
      return;
   }

   // Redirect the call to the shim.  rel32 is relative to the next instruction.
   // .text is RW during install (dllmain re-protects afterwards).
   g_site    = site;
   g_origRel = *(int32_t*)(site + 1);
   *(int32_t*)(site + 1) =
      (int32_t)((uintptr_t)&jet_effect_teardown_only - ((uintptr_t)site + 5));
}

void jetpack_fp_sound_fix_uninstall()
{
   // Sections are re-protected by the time this runs, so the restore cannot be
   // a plain write.
   if (g_site) {
      protected_write(g_site + 1, &g_origRel, sizeof(g_origRel));
      g_site = nullptr;
   }
}
