#include "pch.h"
#include "barrel_fire_origin.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

// =============================================================================
// Barrel fire origin — WeaponCannon OverrideAimer vtable hook.
//
// Weapon::OverrideAimer is virtual slot 0x70; the base impl just returns false.
// We patch the WeaponCannon vtable slot to our hook, which relocates the aimer's
// mFirePos to the barrel hardpoint so bolts (and the muzzle flash) originate from
// the gun in both first and third person.
//
// =============================================================================

bool g_useBarrelFireOrigin = true;

// Vtable slot + original/hook pointers (file-local; nothing else touches them).
static void** s_slot = nullptr;
static void*  s_orig = nullptr;
static void*  s_hook = nullptr;

// Controllable::mIsAiming offset — differs by build because TargetInfo shifts -4 on
// release (modtools TargetInfo@0x148 -> mIsAiming 0x160; Steam/GOG @0x144 -> 0x15C).
// Set by barrel_fire_origin_install() from g_build.
static unsigned s_misAimingOff = 0x160;

// Replacement for WeaponCannon::OverrideAimer (vtable slot 0x70).
// Relocates the fire ORIGIN (Aimer::mFirePos, 0x88) to the barrel hardpoint
// (Weapon::mFirePointMatrix trans).  Falls back to the vanilla aimer position when
// the matrix is stale (first-person zoom) or reflected (water).
//
// TODO(reticle parallax): WeaponCannon::Fire (Phantom 0x7B5680) builds the
// OrdnanceDesc from mFirePos (origin) and mDirection (0x48) INDEPENDENTLY.  We move
// only the origin to the barrel, so the bolt keeps the eye-ray direction and flies
// parallel-offset from the crosshair (worst at close range).  ReticleCorrection
// (hud_widescreen.cpp) exposes this once the crosshair is drawn honestly.
//
// A direction-convergence attempt was tried and REVERTED (made it far worse):
//   - The reticle is NOT a raycast.  ReticuleDisplay::Update (modtools 0x683270)
//     draws the crosshair at the projection of P = mAimStart + aimDir*1024
//     (mAimStart = Controllable+0x148 TargetInfo base; aimDir = Controllable+0xE8).
//   - Aiming the barrel at that P is useless anyway: at 1024 units the angular
//     correction is ~barrelOffset/1024 ≈ negligible, so it can't fix close range.
//   - Worse, reading owner+0x148/+0xE8 in the FIRE path produced wildly wrong
//     directions (shots into the ground) — those TargetInfo fields are not a
//     reliable per-frame eye ray for the firing weapon here.
// A real fix needs EITHER a fixed moderate zero-distance D (~20-40u) converging
//   mDirection = normalize((rootPos[0x70] + origDir[0x48]*D) - barrelPos)
// using the AIMER's own fresh fields (not TargetInfo), OR a camera-through-
// crosshair raycast to hit exactly what's under the reticle at all ranges.
static bool __fastcall hooked_cannon_OverrideAimer(void* weapon, void* /*edx*/)
{
   if (!g_useBarrelFireOrigin) return false;

   // Zoom detection: revert to vanilla aimer when zoomed with a scope weapon.
   // mIsAiming (owner+s_misAimingOff; 0x160 modtools / 0x15C release): zoom state.
   // mIsFirstPersonView: Controllable+0x34 (mTracker ptr) → Tracker+0x14.
   //
   // Two zoom modes exist:
   //   1. "Closer in" — just FOV tightening, barrel fire origin is fine.
   //   2. Scope texture — high magnification, barrel-to-camera parallax
   //      makes shots miss the crosshair badly in third person.
   //
   // Scope weapons are identified by WeaponClass+0x2B0 bit 3 (mZoomFirstPerson).
   // In FP zoom, always bail (original behavior).
   // In TP zoom, bail only for scope weapons.
   void* owner = *(void**)((char*)weapon + 0x6C);
   if (owner) {
      bool isZoomed = *(bool*)((char*)owner + s_misAimingOff);
      if (isZoomed) {
         void* tracker = *(void**)((char*)owner + 0x34);
         if (tracker) {
            bool isFirstPerson = *(bool*)((char*)tracker + 0x14);
            if (isFirstPerson) return false;
         }
         // TP zoom: bail for scope weapons (high magnification = large parallax)
         void* weaponClass = *(void**)((char*)weapon + 0x64); // Weapon::mClass
         if (weaponClass && (*(uint8_t*)((char*)weaponClass + 0x2B0) & 0x08))
            return false;
      }
   }

   __try {
      void* aimer = *(void**)((char*)weapon + 0x70);   // Weapon::mAimer
      if (!aimer) return false;

      // Weapon::mFirePointMatrix at weapon+0x20 (PblMatrix, 0x40 bytes).
      // PblMatrix::trans row is at offset 0x30 — the world-space fire position.
      float* trans = (float*)((char*)weapon + 0x20 + 0x30);

      // Validate: check for uninitialized (0xCDCDCDCD) or zero
      const uint32_t raw = *(uint32_t*)&trans[0];
      if (raw == 0xCDCDCDCD ||
          (trans[0] == 0.0f && trans[1] == 0.0f && trans[2] == 0.0f))
         return false;

      float* aimerFirePos = (float*)((char*)aimer + 0x88);  // Aimer::mFirePos
      float* rootPos      = (float*)((char*)aimer + 0x70);  // Aimer::mRootPos

      // Reflection guard: the engine's reflection render pass
      // (FLRenderer::RenderReflections at 0x0081DCE0, region test at
      // FLRenderer::IsReflected 0x0081CE10) mirrors mFirePointMatrix across
      // the reflective surface.  Both water and reeflection regions
      // floors produce a horizontal-plane Y-flip.
      //
      // A mirror flips the matrix's handedness — the 3×3 determinant goes
      // from +1 (proper rotation) to -1 (improper rotation).  This catches
      // every horizontal-plane mirror regardless of distance to the surface
      // (a position-delta heuristic misses the case where the unit stands
      // on the surface — Y delta drops to ~3 units).
      //
      // For a horizontal-plane reflection, only Y is mirrored: trans.x and
      // trans.z are still the correct hp_fire world position.  We
      // reconstruct Y by mirroring back across the soldier's feet plane,
      // approximated as (rootPos.y − soldier_height).  Aimer::mRootPos was
      // set by SetSoldierInfo to the un-reflected aim origin (eye height),
      // so it's a clean reference.
      const float* m0 = (float*)((char*)weapon + 0x20);
      const float* m1 = m0 + 4;
      const float* m2 = m0 + 8;
      const float det =
         m0[0] * (m1[1] * m2[2] - m1[2] * m2[1]) -
         m0[1] * (m1[0] * m2[2] - m1[2] * m2[0]) +
         m0[2] * (m1[0] * m2[1] - m1[1] * m2[0]);

      float fireY = trans[1];
      if (det < 0.0f) {
         // Reconstruct un-mirrored Y using the soldier's authoritative
         // world position (struct_base + 0x124).  owner is the Controllable
         // base == entity == struct_base + 0x240, so world.y is at
         // owner - 0x240 + 0x124 = owner - 0x11C.  This is the engine's
         // own ground/origin reference for the unit — no soldier-height
         // assumption needed, and it works regardless of stance.
         // (Entity->Controllable == 0x240 assumed build-invariant; unverified
         //  on Steam — only affects muzzle-flash Y inside water reflections.)
         //
         // Reflected trans.y = 2*Yw - true_y  →  true_y = 2*Yw - trans.y.
         if (!owner) return false;
         const float Yw = *(const float*)((const char*)owner - 0x11C);
         fireY = 2.0f * Yw - trans[1];
      }

      // Position sanity: reject grossly out-of-body X/Z (corrupt matrix).
      const float dx = trans[0] - rootPos[0];
      const float dz = trans[2] - rootPos[2];
      if (dx < -5.0f || dx > 5.0f || dz < -5.0f || dz > 5.0f)
         return false;

      aimerFirePos[0] = trans[0];
      aimerFirePos[1] = fireY;
      aimerFirePos[2] = trans[2];
      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// ---------------------------------------------------------------------------
// Install — build-aware (modtools + Steam).  Patches the WeaponCannon vtable
// OverrideAimer slot to our hook after validating it still points at the vanilla
// implementation.  Struct offsets used by the hook are build-invariant except
// mIsAiming (see s_misAimingOff).  GOG addresses are not yet derived -> no-op.
// ---------------------------------------------------------------------------
void barrel_fire_origin_install(uintptr_t exe_base)
{
   uintptr_t slotVA, implVA, thunkVA;
   switch (g_build) {
   case GameBuild::Modtools:
      slotVA  = game_addrs::modtools::weapon_cannon_vftable_override_aimer;
      implVA  = game_addrs::modtools::weapon_override_aimer_impl;
      thunkVA = game_addrs::modtools::weapon_override_aimer_thunk;
      s_misAimingOff = 0x160;
      break;
      
   case GameBuild::Steam:
      slotVA  = game_addrs::steam::weapon_cannon_vftable_override_aimer;
      implVA  = game_addrs::steam::weapon_override_aimer_impl;
      thunkVA = game_addrs::steam::weapon_override_aimer_thunk;
      s_misAimingOff = 0x15C;
      break;
   default:
      return; // GOG / unknown: addresses not derived yet
   }

   s_slot = (void**)resolve(exe_base, slotVA);
   void* expected_impl  = resolve(exe_base, implVA);
   void* expected_thunk = resolve(exe_base, thunkVA);
   s_hook = (void*)&hooked_cannon_OverrideAimer;

   if (*s_slot == expected_impl || *s_slot == expected_thunk) {
      DWORD oldProt;
      if (VirtualProtect(s_slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
         s_orig  = *s_slot;
         *s_slot = s_hook;
         VirtualProtect(s_slot, sizeof(void*), oldProt, &oldProt);
      }
   }
}

// ---------------------------------------------------------------------------
// Uninstall — restore the original vtable slot.
// ---------------------------------------------------------------------------
void barrel_fire_origin_uninstall()
{
   if (s_slot && s_orig) {
      DWORD oldProt;
      if (VirtualProtect(s_slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
         *s_slot = s_orig;
         VirtualProtect(s_slot, sizeof(void*), oldProt, &oldProt);
      }
   }
}
