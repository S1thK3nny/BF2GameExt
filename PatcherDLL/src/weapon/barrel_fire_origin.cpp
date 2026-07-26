#include "pch.h"
#include <math.h>
#include "barrel_fire_origin.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

// =============================================================================
// Barrel fire origin — OverrideAimer vtable hook.
//
// Weapon::OverrideAimer is virtual slot 0x70; the base impl just returns false.
// We patch that slot on the WeaponCannon and WeaponLauncher vtables, and the hook
// relocates the aimer's mFirePos to the barrel hardpoint so bolts (and the muzzle
// flash) originate from the gun in both first and third person.
//
// WeaponLauncher (ClassLabel "launcher") derives from WeaponCannon but carries its
// own vtable, so it needs its own patch.  It overrides seven slots and Fire is not
// one of them — there is no WeaponLauncher::Fire at all — so it reaches
// WeaponCannon::Fire and the same aimer this hook writes, no logic changes needed.
//
// =============================================================================

bool g_useBarrelFireOrigin = true;

// Patched vtable slots + the originals they displaced (file-local; nothing else
// touches them).  One entry per weapon class: WeaponCannon and WeaponLauncher, the
// latter being a WeaponCannon subclass with its own vtable that overrides seven
// slots — Fire not among them, so it reaches WeaponCannon::Fire and the same aimer
// the hook writes.
static const int kMaxPatchedSlots = 2;
static void** s_slot[kMaxPatchedSlots] = {};
static void*  s_orig[kMaxPatchedSlots] = {};
static int    s_slotCount = 0;
static void*  s_hook = nullptr;

// Controllable::mIsAiming offset — differs by build because TargetInfo shifts -4 on
// release (modtools TargetInfo@0x148 -> mIsAiming 0x160; Steam/GOG @0x144 -> 0x15C).
// Set by barrel_fire_origin_install() from g_build.
static unsigned s_misAimingOff = 0x160;

// ---------------------------------------------------------------------------
// Scope texture — ask the engine instead of re-deriving the condition.
//
// ScopeDisplay::Update (modtools 0x683D80) decides visibility from five terms:
//
//   CameraManager::IsChaseMode(cameraId)
//   && (EntityClass+0x1FC >> 3) & 1
//   && Controllable::mIsAiming
//   && (Weapon::ZoomFirstPerson(weapon) || Tracker::IsFirstPersonView(tracker))
//   && RedCamera::_fZoom > 1.0
//
// and stores the answer in a bool on the instance.  Reimplementing that has gone
// wrong twice already in this file (the bit-3 test missed SniperScope weapons; the
// raw Tracker+0x14 read ignores the class camera-mode override), so read the flag
// the engine actually computed.  The instance pointer is a one-element array
// indexed by camera; PC never uses index 0's neighbours.
// ---------------------------------------------------------------------------
static uintptr_t s_scopeDisplay = 0;   // address of the ScopeDisplay* global

static bool scopeTextureVisible()
{
   if (!s_scopeDisplay) return false;
   const char* sd = *(const char* const*)s_scopeDisplay;
   if (!sd) return false;
   return *(const bool*)(sd + 0x4C9);
}

// ---------------------------------------------------------------------------
// CollisionManager::RayHit — used to find what the vanilla shot would hit, so the
// barrel ray can be aimed at the same point (see convergeDirection below).
//
//   float RayHit(PblVector3* start, PblVector3* dir, float maxDist,
//                CollisionObject** outHit, PblVector3* outNormal,
//                GameObject** exclude, int excludeCount, int flags, bool)
//
// Returns the hit fraction of maxDist; 1.0 means nothing was hit.  outHit is
// written unconditionally on entry, so it must never be null.
//
// The two builds disagree on everything but the argument list:
//   modtools (0x42E230) — plain __cdecl, every argument on the stack, result ST(0)
//   Steam    (0x45E3A0) — LTCG: ECX = start, EDX = dir, XMM2 = maxDist, the other
//                         six pushed, caller-cleans, result XMM0
// Calling the release build through the debug signature is the exact mistake that
// produced the old aim-assist crash (every stack argument shifts one slot), hence
// the naked thunk.
// ---------------------------------------------------------------------------
typedef float(__cdecl* RayHit_t)(const void* start, const void* dir, float maxDist,
                                 void** outHit, void* outNormal, void** exclude,
                                 int excludeCount, int flags, int lastArg);

static uintptr_t s_rayHitFn = 0;      // resolved engine address
static RayHit_t  s_rayHit   = nullptr; // what convergeDirection() calls

// Marshals the __cdecl signature above onto the release build's register layout.
static __declspec(naked) float __cdecl rayhit_release_thunk(
   const void* /*start*/, const void* /*dir*/, float /*maxDist*/,
   void** /*outHit*/, void* /*outNormal*/, void** /*exclude*/,
   int /*excludeCount*/, int /*flags*/, int /*lastArg*/)
{
   __asm {
      push  ebp
      mov   ebp, esp
      // Stack arguments, deepest last: outHit .. lastArg (six dwords).
      push  dword ptr [ebp + 0x28]   // lastArg
      push  dword ptr [ebp + 0x24]   // flags
      push  dword ptr [ebp + 0x20]   // excludeCount
      push  dword ptr [ebp + 0x1C]   // exclude
      push  dword ptr [ebp + 0x18]   // outNormal
      push  dword ptr [ebp + 0x14]   // outHit
      mov   ecx, dword ptr [ebp + 0x08]         // start
      mov   edx, dword ptr [ebp + 0x0C]         // dir
      movss xmm2, dword ptr [ebp + 0x10]        // maxDist
      mov   eax, dword ptr [s_rayHitFn]
      call  eax
      add   esp, 24                             // caller-cleans
      // XMM0 -> ST(0), which is where a __cdecl float return belongs.
      sub   esp, 4
      movss dword ptr [esp], xmm0
      fld   dword ptr [esp]
      add   esp, 4
      mov   esp, ebp
      pop   ebp
      ret
   }
}

// How far to look for the vanilla shot's impact point.  Beyond this the ray is
// treated as "nothing hit" and convergence falls back to the far end of the ray,
// where the remaining error is a fraction of the barrel offset.
static const float kConvergeMaxDist = 500.0f;

// Pushed forward along the aim direction so the ray starts outside the shooter's
// own collision volume — cheaper and less fragile than building an exclude list,
// which would need the GameObject* behind the Controllable.
static const float kConvergeStartOffset = 1.0f;

// Re-aims the barrel ray at whatever the vanilla ray would have hit.
//
// Without this, moving the origin from the engine's eye-point fire position to the
// barrel leaves the direction untouched, so the shot is a ray parallel to the
// vanilla one, displaced by the whole barrel-to-eyepoint vector — a fixed
// world-space miss that scope magnification puts right in your face.
//
//   O = vanilla origin (aimer mRootPos — SetSoldierInfo wrote it alongside
//       mFirePos and we never touch it, so it survives our own mFirePos write)
//   D = vanilla direction (aimer mDirection)
//   P = O + D * hitDistance          <- what the vanilla shot hits
//   B = barrel hardpoint
//   new direction = normalize(P - B)
//
// Returns false and leaves the direction alone whenever the result would be
// untrustworthy, so a bad raycast degrades to the previous behaviour instead of
// throwing shots somewhere arbitrary.
static bool convergeDirection(float* dir, const float* origin, const float* barrel)
{
   if (!s_rayHit) return false;

   const float D[3] = { dir[0], dir[1], dir[2] };

   float start[3] = { origin[0] + D[0] * kConvergeStartOffset,
                      origin[1] + D[1] * kConvergeStartOffset,
                      origin[2] + D[2] * kConvergeStartOffset };

   // Flags 0x9A are the engine's own aim-ray mask: EntitySoldier::UpdateWeaponAndAimer
   // uses exactly these to resolve TargetInfo::mAimPoint, i.e. to answer "what is this
   // player aiming at".  Same question, so same mask — we only drop its 60-unit cap.
   // (0x80 = terrain, 0x100 = water; Ordnance::Update sweeps the world with 0x90, a
   // subset, and RayHit's own default when flags == 0 is 0xBE, a superset whose two
   // extra categories are unidentified.)  Anything the mask does not see — a soldier
   // may be one — converges on the geometry behind it instead, which leaves a
   // fraction of the barrel offset rather than all of it.
   void* hitObj = nullptr;   // written on entry by RayHit; must be a real pointer
   const float frac = s_rayHit(start, D, kConvergeMaxDist, &hitObj, nullptr,
                               nullptr, 0, 0x9A, 1);
   if (!(frac >= 0.0f) || frac > 1.0f) return false;   // also rejects NaN

   // Distance from the ORIGIN, not from the pushed-forward ray start.
   const float dist = kConvergeStartOffset + frac * kConvergeMaxDist;
   if (dist < 2.0f) return false;   // muzzle-contact range; nothing to correct

   const float P[3] = { origin[0] + D[0] * dist,
                        origin[1] + D[1] * dist,
                        origin[2] + D[2] * dist };

   float N[3] = { P[0] - barrel[0], P[1] - barrel[1], P[2] - barrel[2] };
   const float lenSq = N[0] * N[0] + N[1] * N[1] + N[2] * N[2];
   if (lenSq < 0.01f) return false;

   const float inv = 1.0f / sqrtf(lenSq);
   N[0] *= inv; N[1] *= inv; N[2] *= inv;

   // Sanity clamp: the correction is an arcminutes-scale nudge in every sane case.
   // Anything past ~25 degrees means one of the inputs was garbage.
   if (N[0] * D[0] + N[1] * D[1] + N[2] * D[2] < 0.9f) return false;

   dir[0] = N[0]; dir[1] = N[1]; dir[2] = N[2];
   return true;
}

// Replacement for WeaponCannon::OverrideAimer (vtable slot 0x70).
// Relocates the fire ORIGIN (Aimer::mFirePos, 0x88) to the barrel hardpoint
// (Weapon::mFirePointMatrix trans).  Falls back to the vanilla aimer position when
// the matrix is stale (first-person zoom) or reflected (water).
//
// WeaponCannon::Fire (Phantom 0x7B5680) builds the OrdnanceDesc from mFirePos
// (origin) and mDirection (0x48) INDEPENDENTLY, so moving the origin on its own
// shifts the whole shot sideways.  While zoomed we therefore also re-aim the
// direction — see convergeDirection().
//
// Two convergence targets were considered and rejected before the raycast:
//   - The reticle is NOT a raycast.  ReticuleDisplay::Update (modtools 0x683270)
//     draws the crosshair at the projection of P = mAimStart + aimDir*1024
//     (mAimStart = Controllable+0x148 TargetInfo base; aimDir = Controllable+0xE8).
//     At 1024 units the angular correction is ~barrelOffset/1024, i.e. nothing.
//     Reading those fields in the FIRE path also produced wildly wrong directions
//     (shots into the ground) — they are not a reliable per-frame eye ray there.
//   - TargetInfo::mAimPoint (TargetInfo+0xC) is the engine's own third-person
//     convergence target, written by UpdateWeaponAndAimer just before this hook
//     runs, but CollisionManager::RayHit caps it at 60 units from the camera.
//     Converging there fixes close range and over-corrects past it, the error
//     growing as barrelOffset * (dist/60 - 1) — worse than doing nothing beyond
//     ~120 units, which is exactly sniper range.
// Only the true hit distance is range-correct, so we cast the ray ourselves.
static bool __fastcall hooked_cannon_OverrideAimer(void* weapon, void* /*edx*/)
{
   if (!g_useBarrelFireOrigin) return false;

   // ---- Zoom state -----------------------------------------------------------
   // mIsAiming = TargetInfo::mIsAiming (TargetInfo+0x18; Controllable+0x148
   // modtools / +0x144 release -> 0x160 / 0x15C).  EntitySoldier::CheckForZoom
   // (modtools 0x528E00) drives it from Weapon::CycleZoom, so it is the zoom
   // TOGGLE, not a per-frame aim state, and it stays 0 for AI (CheckForZoom
   // returns early when mPlayerId < 0) and for weapons with ZoomMax <= 1.
   //
   // Why relocating the origin costs accuracy at all:
   // EntitySoldier::UpdateWeaponAndAimer (modtools 0x52C980, Phantom 0x58AD80)
   // computes the fire position from sEyePointOffset[stance] and then, in the
   // third-person path, derives the aim DIRECTION by converging that position
   // onto TargetInfo::mAimPoint (the camera ray's hit point):
   //     dir = normalize(mAimPoint - firePos)
   // We move only the ORIGIN to the barrel, so unless the direction is re-aimed
   // the shot is a ray parallel to the vanilla one, displaced by the whole
   // barrel-to-eyepoint vector.  That displacement is fixed in world units, so
   // what decides whether it is visible is magnification: unzoomed nobody can see
   // it, zoomed it is the difference between hitting and missing.
   //
   // So: zoomed, pay for a raycast and correct the direction properly.  mIsAiming
   // is only ever set for the local player (CheckForZoom returns early when
   // mPlayerId < 0), which bounds this to one or two rays per frame no matter how
   // many WeaponCannons the level holds.
   void* owner = *(void**)((char*)weapon + 0x6C);
   bool converge = false;
   if (owner && *(bool*)((char*)owner + s_misAimingOff)) {
      // First-person zoom: mFirePointMatrix stops tracking (the third-person model
      // is not being posed), so the barrel position itself goes stale and there is
      // nothing worth converging on.
      void* tracker = *(void**)((char*)owner + 0x34);
      if (tracker && *(bool*)((char*)tracker + 0x14))
         return false;

      converge = true;
   }

   __try {
      // Scope texture up: hand the frame back untouched.  Converging the direction
      // makes the shot land correctly but cannot fix how it LOOKS — the bolt still
      // leaves from wherever the idle animation is holding the muzzle, so instead
      // of a straight line from the gun you get a visible diagonal to the target.
      // Under a scope there is nothing to gain from the barrel origin anyway.
      if (scopeTextureVisible()) return false;

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

      // Re-aim from the new origin so the shot still hits what the vanilla shot
      // would have hit.  mRootPos is the engine's own fire position: SetSoldierInfo
      // writes mFirePos and mRootPos from the same value and we only overwrite
      // mFirePos, so mRootPos still holds the untouched vanilla origin.
      if (converge)
         convergeDirection((float*)((char*)aimer + 0x48), rootPos, aimerFirePos);

      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// ---------------------------------------------------------------------------
// Install — build-aware (modtools + Steam).  Patches the OverrideAimer slot of the
// WeaponCannon and WeaponLauncher vtables to our hook, each only after validating
// that it still points at the vanilla implementation.  Struct offsets used by the
// hook are build-invariant except mIsAiming (see s_misAimingOff).  GOG addresses
// are not yet derived -> no-op.
// ---------------------------------------------------------------------------
void barrel_fire_origin_install(uintptr_t exe_base)
{
   uintptr_t cannonVA, launcherVA, implVA, thunkVA, rayHitVA, scopeVA;
   bool rayHitIsRelease;
   switch (g_build) {
   case GameBuild::Modtools:
      cannonVA   = game_addrs::modtools::weapon_cannon_vftable_override_aimer;
      launcherVA = game_addrs::modtools::weapon_launcher_vftable_override_aimer;
      implVA     = game_addrs::modtools::weapon_override_aimer_impl;
      thunkVA    = game_addrs::modtools::weapon_override_aimer_thunk;
      rayHitVA   = game_addrs::modtools::collision_manager_ray_hit;
      rayHitIsRelease = false;   // plain __cdecl, result in ST(0)
      scopeVA = game_addrs::modtools::scope_display_instance;
      s_misAimingOff = 0x160;
      break;

   case GameBuild::Steam:
      cannonVA   = game_addrs::steam::weapon_cannon_vftable_override_aimer;
      launcherVA = game_addrs::steam::weapon_launcher_vftable_override_aimer;
      implVA     = game_addrs::steam::weapon_override_aimer_impl;
      thunkVA    = game_addrs::steam::weapon_override_aimer_thunk;
      rayHitVA   = game_addrs::steam::collision_manager_ray_hit;
      rayHitIsRelease = true;    // ECX/EDX/XMM2 + six stack args, result in XMM0
      scopeVA = game_addrs::steam::scope_display_instance;
      s_misAimingOff = 0x15C;
      break;

   case GameBuild::GOG:
      cannonVA   = game_addrs::gog::weapon_cannon_vftable_override_aimer;
      launcherVA = game_addrs::gog::weapon_launcher_vftable_override_aimer;
      implVA     = game_addrs::gog::weapon_override_aimer_impl;
      thunkVA    = game_addrs::gog::weapon_override_aimer_thunk;
      rayHitVA   = game_addrs::gog::collision_manager_ray_hit;
      rayHitIsRelease = true;    // same LTCG RayHit convention as Steam
      scopeVA = game_addrs::gog::scope_display_instance;
      s_misAimingOff = 0x15C;    // shared release layout
      break;
   default:
      return; // unknown build
   }

   // Direction convergence is optional: if RayHit is missing the hook still
   // relocates the origin, it just stops correcting for the offset while zoomed.
   s_rayHitFn = (uintptr_t)resolve(exe_base, rayHitVA);
   s_rayHit   = rayHitIsRelease ? &rayhit_release_thunk : (RayHit_t)s_rayHitFn;

   s_scopeDisplay = (uintptr_t)resolve(exe_base, scopeVA);

   void* expected_impl  = resolve(exe_base, implVA);
   void* expected_thunk = resolve(exe_base, thunkVA);
   s_hook = (void*)&hooked_cannon_OverrideAimer;

   const uintptr_t slotVAs[kMaxPatchedSlots] = { cannonVA, launcherVA };
   for (int i = 0; i < kMaxPatchedSlots; i++) {
      void** slot = (void**)resolve(exe_base, slotVAs[i]);
      if (*slot != expected_impl && *slot != expected_thunk)
         continue;   // already hooked, or not the vtable we think it is

      DWORD oldProt;
      if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
         s_slot[s_slotCount] = slot;
         s_orig[s_slotCount] = *slot;
         s_slotCount++;
         *slot = s_hook;
         VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
      }
   }
}

// ---------------------------------------------------------------------------
// Uninstall — restore every original vtable slot.
// ---------------------------------------------------------------------------
void barrel_fire_origin_uninstall()
{
   for (int i = 0; i < s_slotCount; i++) {
      if (!s_slot[i] || !s_orig[i]) continue;
      DWORD oldProt;
      if (VirtualProtect(s_slot[i], sizeof(void*), PAGE_READWRITE, &oldProt)) {
         *s_slot[i] = s_orig[i];
         VirtualProtect(s_slot[i], sizeof(void*), oldProt, &oldProt);
      }
   }
}
