#include "pch.h"
#include <math.h>
#include <string.h>
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
// WeaponGrapplingHook (ClassLabel "grapplinghookweapon") is a third such subclass
// and gets the same treatment on modtools, where the grappling hook is enabled.
// It overrides only CheckFire, SetOrdnance and the net pair, so both slots hold
// the same vanilla thunks — and because OrdnanceGrapplingHook takes its start
// point from the aimer this hook relocates, the hook itself leaves the barrel
// rather than the soldier's head.
//
// =============================================================================

bool g_useBarrelFireOrigin = true;

// Patched vtable slots + the originals they displaced (file-local; nothing else
// touches them).  Two slots per weapon class — OverrideAimer (0x70) and Render
// (0x8C) — on WeaponCannon, WeaponLauncher and (modtools only)
// WeaponGrapplingHook, the latter two being WeaponCannon subclasses with their own
// vtables.  Neither overrides Fire, so both reach WeaponCannon::Fire and the same
// aimer the hook writes, and none overrides Render, so every Render slot holds the
// same function.
static const int kMaxPatchedSlots = 6;
static void** s_slot[kMaxPatchedSlots] = {};
static void*  s_orig[kMaxPatchedSlots] = {};
static int    s_slotCount = 0;

// Weapon::mFirePointMatrix — PblMatrix, 16 floats, at Weapon+0x20 on every build.
static const unsigned kFirePointMatrixOff = 0x20;

// Controllable::mPlayerId, an int.  Build-INVARIANT at 0xD4: WeaponMelee::OverrideAimer
// reads it off exactly this pointer on all three builds (modtools 0x6345D6/0x6345D9;
// Steam 0x688F98 and GOG 0x68A028 are byte-identical 8B 42 6C 83 B8 D4 00 00 00 00).
static const unsigned kPlayerIdOff = 0xD4;

// Controllable, all build-INVARIANT. mEyePoint and mEyeDir are written
// unconditionally at the top of EntitySoldier::UpdateWeaponAndAimer, before any
// branch, so they are always this turn's true eye. mAimStart / mAimPoint are the
// TargetInfo pair at +0x148 / +0x154 -- NOT the +0x144 the PDB implies for release
// builds, which is the same -4 that already shipped one bug in this file.
static const unsigned kEyePointOff = 0xDC;
static const unsigned kEyeDirOff   = 0xE8;
static const unsigned kAimStartOff = 0x148;
static const unsigned kAimPointOff = 0x154;

// NetComm::sLocalPlayerId — int[2].  See game_addrs.hpp for why the table is read
// rather than the accessor called.  Null if the build has no address, in which case
// ownerIsLocalPlayer answers false for everyone and the whole level converges at the
// fixed distance: degraded, not broken.
static const int* s_localPlayerIds = nullptr;

// Is this Controllable a LOCAL human?  This decides who is worth a raycast, and it is
// deliberately FAIL-SAFE: a false negative only downgrades that player to fixed-
// distance convergence, which is still correct, just less exact at range.
//
// It replaces a test on Controllable::mIsAiming, which was never the right question.
// EntitySoldier::CheckForZoom early-returns on mPlayerId < 0, so mIsAiming means
// "not AI" -- never "local" -- and it is a zoom TOGGLE on top of that.  mTracker is
// not a local test either: Controllable::SetupController calls Trackable::Track for
// every human on every machine.
static bool ownerIsLocalPlayer(const void* owner)
{
   if (!s_localPlayerIds) return false;
   const int pid = *(const int*)((const char*)owner + kPlayerIdOff);
   if (pid < 0) return false;                    // AI
   return s_localPlayerIds[0] == pid || s_localPlayerIds[1] == pid;
}

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
// CollisionManager::RayHit — used to find what the vanilla shot would hit, so the
// barrel ray can be aimed at the same point (see aimTargetPoint below).
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
static RayHit_t  s_rayHit   = nullptr; // what aimTargetPoint() calls

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

// ---------------------------------------------------------------------------
// Reflection regions — Weapon::Render hook.
//
// Weapon::Render is the engine's ONLY writer of Weapon::mFirePointMatrix.  It
// bakes the matrix it is handed:
//
//   Weapon::Render(PblMatrix* world, RedPose* pose, RedColor* color,
//                  uint flags, bool highRes)
//       node = pose->Find(hp_fire)            // hash 0x2B960099 / 0xB7EC1D31
//       if (!node) return;                    // no hardpoint -> matrix left alone
//       world = node * world
//       mRenderClass->mModel->Render(world, 0, color, flags, 0)
//       if (color->a == 0) return;            // invisible -> matrix left alone
//       mFirePointMatrix       = world
//       mFirePointMatrix.trans = TransformCoord(mRenderClass->mFirePointOffset, world)
//
// which means the fire point is whatever the LAST draw of the frame used.
//
// Inside a reflection region that is not the real draw.  Dynamic entities get
// their planar reflection by being drawn a SECOND time in the main pass with a
// mirrored world matrix — EntityProp::Render and EntitySoldier::Render both do
//
//   if (!(flags & 0x200000) && FLRenderer::IsReflected(pos, radius, &R, false))
//       Render(world * R, ..., flags & ~0x10000 | 0x10000100)
//
// where R is ReflectionRegion::m_reflectionMat, so the weapon is re-rendered
// mirrored and mFirePointMatrix keeps the reflected position.  The aimer hook
// then reads it a frame later and the bolt leaves from the mirror image: an
// error of twice the shooter's height above the reflective plane, which is a
// couple of units on the floor of dea1's falcon hangar and tens of units from
// the walkways above it.
//
// Restoring the matrix around the mirrored draw fixes it at the source.  The
// engine still gets the mirrored matrix for the duration of the call, so the
// reflected muzzle flash and charge-up effect are unaffected, and afterwards
// the field holds what the real draw put there.  This needs no idea of where
// the mirror plane is, survives any number of overlapping reflection regions
// (each mirrored draw is bracketed independently), and works for a mirror of
// any orientation rather than just a horizontal floor.
//
// The predicate is the handedness of the matrix about to be baked: a mirror is
// an improper transform, so its 3x3 determinant is negative.  Reflections are
// the only source of one — a negative scale anywhere else would render the model
// inside out — and a false positive would only mean this draw leaves the fire
// point at its previous value, which is the safe direction to be wrong in.
//
// Weapon::Render stays plain __thiscall on all three builds (ECX = this, five
// stack args, RET 0x14): it is virtual, so LTCG left the convention alone.
// Verified off the Steam (0x679350) and GOG (0x67A3F0) prologue/epilogue.
// ---------------------------------------------------------------------------
typedef void(__fastcall* WeaponRender_t)(void* self, void* edx, const void* world,
                                         void* pose, const void* color, unsigned flags,
                                         int highRes);

static WeaponRender_t s_origWeaponRender = nullptr;

// 3x3 determinant of a PblMatrix's rotation part (rows are 4 floats each).
static bool matrixIsMirrored(const void* matrix)
{
   const float* m = (const float*)matrix;
   const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) -
                     m[1] * (m[4] * m[10] - m[6] * m[8]) +
                     m[2] * (m[4] * m[9]  - m[5] * m[8]);
   return det < 0.0f;
}

static void __fastcall hooked_weapon_Render(void* weapon, void* /*edx*/, const void* world,
                                            void* pose, const void* color, unsigned flags,
                                            int highRes)
{
   if (!s_origWeaponRender) return;   // never installed without this being set

   const bool mirrored = g_useBarrelFireOrigin && weapon && world && matrixIsMirrored(world);

   float saved[16];
   if (mirrored)
      memcpy(saved, (char*)weapon + kFirePointMatrixOff, sizeof(saved));

   s_origWeaponRender(weapon, nullptr, world, pose, color, flags, highRes);

   if (mirrored)
      memcpy((char*)weapon + kFirePointMatrixOff, saved, sizeof(saved));
}

// How far to look for the impact point on the crosshair line.
static const float kConvergeMaxDist = 500.0f;

// The ray starts this far ahead of the muzzle's own projection onto the aim line,
// which clears the shooter without needing an exclude list.
static const float kRayClear = 0.35f;

// Below this the hit is muzzle contact or the shooter himself; there is nothing to
// correct and a correction computed from it would be enormous.
static const float kMinTargetDist = 1.5f;

// An mAimPoint nearer than this is one of the engine's CAPPED aim points (60 units
// for a local player in third person, 20 for a remote soldier on a net client)
// rather than the genuine 200-unit one it writes for AI. Converging on a capped
// point is worse than doing nothing past twice the cap, so the AI path refuses it.
static const float kMinAiAimDist = 100.0f;

// Below this barrel-to-origin displacement there is nothing worth moving.
static const float kMinBarrelOffsetSq = 0.0025f;   // (5 cm)^2

// Hard cap on the correction applied to an AI aimer. AIUtil::DumbDown (modtools
// 0x591DC0) reads Aimer::mDirection back as the previous state of a discrete PD
// controller, so a bias we inject becomes its steady-state error; push past what
// the loop can hold and the aim slides degrees per second until it snaps. The same
// bias erodes the AI fire gate, which needs dot(aimDir, eyeDir) > cos(pi/8).
//
// Estimated from the AIDifficulty ranges, NOT measured. If AI aim is ever seen
// sliding rather than jittering, halve it.
static const float kAiMaxCorrectionRad = 0.004f;

// ---------------------------------------------------------------------------
// Where the shot should actually land.
//
// The previous design converged on `mRootPos + mDirection * T` -- a point on the
// VANILLA AIMER'S OWN LINE -- and spent its effort choosing T. That was wrong twice
// over, and both showed up in play:
//
//   * In third person the vanilla aimer line is NOT the crosshair line.
//     UpdateWeaponAndAimer converges the fire point onto the camera axis at only
//     TrackOffset.z + 2 + 60*frac, i.e. about 5..65 units. Past that the vanilla
//     shot itself drifts, and reproducing it faithfully reproduces the drift.
//   * A clean RayHit MISS returns frac == 1.0 exactly, which the old code accepted,
//     so every miss converged at 501 units. Mask 0x9A does not include water
//     (0x100), so firing across water was a guaranteed miss.
//
// Both are the same error class: a convergence point further away than the thing
// being shot at, which leaves residual error on the BARREL side. That is the
// "distant shots land to the right" report.
//
// So: stop computing T. Resolve the actual impact point on the CROSSHAIR line, and
// treat a miss as "no correction" rather than as a distance. A fallback at 500 is
// worse than vanilla inside about 100 units.
// ---------------------------------------------------------------------------
static bool aimTargetPoint(const void* owner, const float* rootPos, float P[3])
{
   const float* E        = (const float*)((const char*)owner + kEyeDirOff);
   const float* eyePos   = (const float*)((const char*)owner + kEyePointOff);
   const float* aimStart = (const float*)((const char*)owner + kAimStartOff);
   const float* aimPoint = (const float*)((const char*)owner + kAimPointOff);

   if (!ownerIsLocalPlayer(owner) || !s_rayHit) {
      // AI and server-side remotes: the engine already authored a genuine target.
      // UpdateWeaponAndAimer writes mAimPoint = origin + dir*200 on both pid < 0
      // sub-branches and on the tracker-less human branch, with no raycast at all.
      // Converging there reproduces the vanilla shot from the barrel with ZERO
      // error and costs us nothing. A local player (60-unit cap) or a remote on a
      // client (20) can never pass the distance gate, so they never take this path.
      const float d[3] = { aimPoint[0] - rootPos[0],
                           aimPoint[1] - rootPos[1],
                           aimPoint[2] - rootPos[2] };
      if (!(d[0]*d[0] + d[1]*d[1] + d[2]*d[2] > kMinAiAimDist * kMinAiAimDist))
         return false;
      P[0] = aimPoint[0]; P[1] = aimPoint[1]; P[2] = aimPoint[2];
      return true;
   }

   // Which line is the crosshair on? MEASURE it rather than guessing at camera mode.
   //
   // UpdateWeaponAndAimer writes mEyePoint unconditionally at the top, then:
   //   third person      mAimStart = camera point, pushed along E until
   //                     dot(mAimStart - mEyePoint, E) == 0 IDENTICALLY
   //   first person, hip mAimStart = mEyePoint + E*t.z + right*t.x + cross(E,right)*t.y
   //                     and right, cross(E,right) are both perpendicular to E, so
   //                     dot(mAimStart - mEyePoint, E) == t.z EXACTLY
   //                     (0.25 stand, 0.25 crouch, 0.15 prone)
   // So that one scalar says which branch the engine took, build-invariantly, with
   // no Tracker read, no ScopeDisplay read and no camera predicate.
   const float v[3] = { aimStart[0] - eyePos[0],
                        aimStart[1] - eyePos[1],
                        aimStart[2] - eyePos[2] };
   const float along = v[0]*E[0] + v[1]*E[1] + v[2]*E[2];
   const float* A = (along > 0.05f && along < 0.40f) ? eyePos : aimStart;

   // Start the ray at the muzzle's own projection onto the aim line, pushed a
   // little further forward. That clears the shooter in every stance without an
   // exclude list, which would need the GameObject* behind the Controllable.
   const float w[3] = { rootPos[0] - A[0], rootPos[1] - A[1], rootPos[2] - A[2] };
   const float d0 = (w[0]*E[0] + w[1]*E[1] + w[2]*E[2]) + kRayClear;
   const float start[3] = { A[0] + E[0]*d0, A[1] + E[1]*d0, A[2] + E[2]*d0 };

   void* hitObj = nullptr;   // RayHit zeroes this on entry; it must never be null
   const float frac = s_rayHit(start, E, kConvergeMaxDist, &hitObj, nullptr,
                               nullptr, 0, 0x9A, 1);

   // No usable hit -- open sky, across water (0x100 is not in the mask), or a hit
   // so close it can only be contact or the shooter himself. Converge on the FAR
   // END of the crosshair ray rather than giving up.
   //
   // That would have been wrong under the previous design and is right under this
   // one, and the difference is which line the ray follows. The old ray ran along
   // the AIMER's line, which diverges from the crosshair past about 65 units, so it
   // could miss a target the player was plainly aiming at -- and converging at 501
   // then left residual error on the barrel side, which is exactly the "distant
   // shots land right" report. This ray follows the CROSSHAIR line, and soldiers,
   // vehicles, terrain and statics are all in the 0x9A mask, so a miss means the
   // line really is empty and there is nothing within 500 units to be inaccurate
   // against. Bailing there would only cost the barrel origin for no accuracy gain.
   //
   // `hitObj` is the engine's own sentinel: RayHit zeroes it on entry and writes it
   // only on an accepted hit, so a clean miss returns frac == 1.0 with hitObj null
   // and cannot be told from a hit at maximum range by frac alone.
   float t = kConvergeMaxDist;
   if (hitObj && frac > 0.0f && frac <= 1.0f) {   // the frac tests also reject NaN
      const float hit = frac * kConvergeMaxDist;
      if (hit > kMinTargetDist) t = hit;
   }

   P[0] = start[0] + E[0]*t; P[1] = start[1] + E[1]*t; P[2] = start[2] + E[2]*t;
   return true;
}

// Aim from `from` at `P`, rejecting anything that cannot be trusted. The coarse
// 60-degree net catches garbage inputs; the correction itself is geometry and at
// contact range is legitimately large, so a tight angular clamp would reject
// exactly the corrections that matter most.
static bool aimAt(const float* from, const float* P, const float* refDir, float out[3])
{
   float n[3] = { P[0] - from[0], P[1] - from[1], P[2] - from[2] };
   const float lenSq = n[0]*n[0] + n[1]*n[1] + n[2]*n[2];
   if (!(lenSq > 1.0f)) return false;   // also rejects NaN
   const float inv = 1.0f / sqrtf(lenSq);
   n[0] *= inv; n[1] *= inv; n[2] *= inv;
   if (!(n[0]*refDir[0] + n[1]*refDir[1] + n[2]*refDir[2] > 0.5f)) return false;
   out[0] = n[0]; out[1] = n[1]; out[2] = n[2];
   return true;
}

// Is this weapon's baked barrel position usable this turn?
static const float* trustedBarrelPoint(void* weapon, const void* owner,
                                       const float* rootPos, const float* P)
{
   const float* m = (const float*)((char*)weapon + kFirePointMatrixOff);

   // trans.w is the LAST write of Weapon::Render's bake (modtools 0x0061E0AB,
   // Steam 0x0067948C, GOG 0x0067A52C -- C7 47 xx 00 00 80 3F), so a matrix that
   // has never been baked does not carry it. Weapon::Weapon never initialises the
   // field, so on retail it is otherwise arbitrary heap contents.
   if (m[15] != 1.0f) return nullptr;
   if (matrixIsMirrored(m)) return nullptr;   // reflection-region backstop

   const float* trans = m + 12;
   const float d[3] = { trans[0] - rootPos[0], trans[1] - rootPos[1], trans[2] - rootPos[2] };
   for (int i = 0; i < 3; ++i)
      if (d[i] < -5.0f || d[i] > 5.0f) return nullptr;   // grossly out of body

   const float lenSq = d[0]*d[0] + d[1]*d[1] + d[2]*d[2];
   if (!(lenSq > kMinBarrelOffsetSq)) return nullptr;    // nothing worth moving

   // NOTE: no AI test here, deliberately. An earlier revision refused the origin
   // move for AI whose barrel sat further than kAiMaxCorrectionRad * range from
   // the chest -- 0.8 units at their 200-unit aim point, which is less than most
   // weapons and ALL long ones, so AI silently stopped firing from the barrel at
   // all. That was wrong on its own terms: AIUtil::DumbDown reads back
   // mDirection, not mFirePos, so relocating the muzzle costs the AI aim loop
   // nothing. Only the DIRECTION correction is a bias it has to absorb, and that
   // is capped at the call site instead.
   return trans;
}

// ---------------------------------------------------------------------------
// Replacement for WeaponCannon::OverrideAimer (vtable slot 0x70).
//
// WeaponCannon::Fire builds the OrdnanceDesc from Aimer::mFirePos and
// mDirection INDEPENDENTLY, so moving the origin on its own shifts the whole shot
// sideways. Origin and direction are therefore committed TOGETHER or not at all --
// there is no path here that relocates the muzzle without re-aiming from it.
//
// Two stages, in order of how much they need:
//   (1) DIRECTION. Needs no barrel data at all, so it works in first person too,
//       where it removes vanilla's own hipfire offset (the stance weapon-offset
//       table puts the shot on a ray PARALLEL to the crosshair, displaced about
//       0.12 right and 0.10 down -- 31 mrad at 5 units). It is an algebraic no-op
//       while aiming down sights, where the vanilla origin already sits on the aim
//       line, and a no-op for AI, whose target point is exactly rootPos + dir*200.
//   (2) ORIGIN. Only when the baked barrel matrix is trustworthy.
//
// Anything untrustworthy returns false and hands the frame back to the vanilla
// aimer, rather than correcting from a number we do not believe.
// ---------------------------------------------------------------------------
static bool __fastcall hooked_cannon_OverrideAimer(void* weapon, void* /*edx*/)
{
   if (!g_useBarrelFireOrigin) return false;

   __try {
      void* owner = *(void**)((char*)weapon + 0x6C);   // Weapon::mOwner
      void* aimer = *(void**)((char*)weapon + 0x70);   // Weapon::mAimer
      if (!owner || !aimer) return false;

      // Aimer::bDirect. Aimer::SetSoldierInfo is its only writer and sets it
      // unconditionally, so it means "UpdateWeaponAndAimer authored mRootPos and
      // mDirection as a matched pair this turn". A turret or vehicle aimer never
      // carries it, which is what keeps those out of scope.
      if (!*(const unsigned char*)((const char*)aimer + 0x29)) return false;

      float*       dir     = (float*)((char*)aimer + 0x48);        // mDirection
      float*       firePos = (float*)((char*)aimer + 0x88);        // mFirePos
      const float* rootPos = (const float*)((char*)aimer + 0x70);  // mRootPos

      float P[3];
      if (!aimTargetPoint(owner, rootPos, P)) return false;

      float newDir[3];
      if (!aimAt(rootPos, P, dir, newDir)) return false;

      const float* B = trustedBarrelPoint(weapon, owner, rootPos, P);
      if (B) {
         float d2[3];
         if (aimAt(B, P, dir, d2)) {
            // The origin move is unconditional once B is trusted -- it is purely
            // visual and nothing downstream reads mFirePos as aim state.
            firePos[0] = B[0]; firePos[1] = B[1]; firePos[2] = B[2];

            // The DIRECTION is what AIUtil::DumbDown reads back as the previous
            // state of its PD loop, so a bias larger than the loop can hold has
            // no fixed point and the aim slides. For AI, take the correction only
            // while it stays inside that budget; otherwise keep the vanilla
            // direction and accept the barrel offset, which is exactly what
            // shipped before convergence existed.
            bool takeDir = true;
            if (*(const int*)((const char*)owner + kPlayerIdOff) < 0) {
               const float dot = d2[0]*dir[0] + d2[1]*dir[1] + d2[2]*dir[2];
               // cos(x) ~ 1 - x^2/2, so compare against the small-angle bound
               // rather than paying an acosf here.
               takeDir = (dot > 1.0f - 0.5f * kAiMaxCorrectionRad * kAiMaxCorrectionRad);
            }
            if (takeDir) { newDir[0] = d2[0]; newDir[1] = d2[1]; newDir[2] = d2[2]; }
         }
      }

      dir[0] = newDir[0]; dir[1] = newDir[1]; dir[2] = newDir[2];
      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// Swaps one vtable slot to `hook`, but only if it still holds one of the two
// vanilla values (impl or its ILT thunk) — otherwise the vtable is not the one we
// think it is, or somebody else got there first.  Remembers what it displaced so
// uninstall can put it back.  Returns the displaced entry, or null on refusal.
static void* patch_vtable_slot(uintptr_t exe_base, uintptr_t slotVA,
                               void* expected_impl, void* expected_thunk, void* hook)
{
   if (s_slotCount >= kMaxPatchedSlots) return nullptr;

   void** slot = (void**)resolve(exe_base, slotVA);
   if (*slot != expected_impl && *slot != expected_thunk)
      return nullptr;

   DWORD oldProt;
   if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt))
      return nullptr;

   void* orig = *slot;
   s_slot[s_slotCount] = slot;
   s_orig[s_slotCount] = orig;
   s_slotCount++;
   *slot = hook;
   VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
   return orig;
}

// ---------------------------------------------------------------------------
// Install — build-aware (modtools, Steam and GOG).  Patches two slots on each of
// the WeaponCannon and WeaponLauncher vtables:
//
//   +0x70  OverrideAimer — relocates the fire origin to the barrel
//   +0x8C  Render        — keeps the reflected duplicate draw from leaving a
//                          mirrored matrix behind in mFirePointMatrix
//
// each only after validating that the slot still points at the vanilla entry.
// EVERY struct offset used by the hooks is now build-invariant, verified on all
// three targets' own bytes.  The one per-build address is NetComm::sLocalPlayerId,
// and a build without it degrades cleanly rather than reading the wrong field.
// ---------------------------------------------------------------------------
void barrel_fire_origin_install(uintptr_t exe_base)
{
   uintptr_t cannonVA, launcherVA, implVA, thunkVA, rayHitVA, localIdsVA;
   uintptr_t cannonRenderVA, launcherRenderVA, renderImplVA, renderThunkVA;
   uintptr_t grappleVA = 0, grappleRenderVA = 0;
   bool rayHitIsRelease;
   switch (g_build) {
   case GameBuild::Modtools:
      cannonVA   = game_addrs::modtools::weapon_cannon_vftable_override_aimer;
      launcherVA = game_addrs::modtools::weapon_launcher_vftable_override_aimer;
      implVA     = game_addrs::modtools::weapon_override_aimer_impl;
      thunkVA    = game_addrs::modtools::weapon_override_aimer_thunk;
      cannonRenderVA   = game_addrs::modtools::weapon_cannon_vftable_render;
      launcherRenderVA = game_addrs::modtools::weapon_launcher_vftable_render;
      renderImplVA     = game_addrs::modtools::weapon_render_impl;
      renderThunkVA    = game_addrs::modtools::weapon_render_thunk;
      rayHitVA   = game_addrs::modtools::collision_manager_ray_hit;
      rayHitIsRelease = false;   // plain __cdecl, result in ST(0)
      // Grappling hook is modtools-only; the other builds have no address for it.
      grappleVA       = game_addrs::modtools::weapon_grapple_vftable_override_aimer;
      grappleRenderVA = game_addrs::modtools::weapon_grapple_vftable_render;
      localIdsVA = game_addrs::modtools::net_comm_local_player_id;
      break;

   case GameBuild::Steam:
      cannonVA   = game_addrs::steam::weapon_cannon_vftable_override_aimer;
      launcherVA = game_addrs::steam::weapon_launcher_vftable_override_aimer;
      implVA     = game_addrs::steam::weapon_override_aimer_impl;
      thunkVA    = game_addrs::steam::weapon_override_aimer_thunk;
      cannonRenderVA   = game_addrs::steam::weapon_cannon_vftable_render;
      launcherRenderVA = game_addrs::steam::weapon_launcher_vftable_render;
      renderImplVA     = game_addrs::steam::weapon_render_impl;
      renderThunkVA    = game_addrs::steam::weapon_render_thunk;
      rayHitVA   = game_addrs::steam::collision_manager_ray_hit;
      rayHitIsRelease = true;    // ECX/EDX/XMM2 + six stack args, result in XMM0
      localIdsVA = game_addrs::steam::net_comm_local_player_id;
      break;

   case GameBuild::GOG:
      cannonVA   = game_addrs::gog::weapon_cannon_vftable_override_aimer;
      launcherVA = game_addrs::gog::weapon_launcher_vftable_override_aimer;
      implVA     = game_addrs::gog::weapon_override_aimer_impl;
      thunkVA    = game_addrs::gog::weapon_override_aimer_thunk;
      cannonRenderVA   = game_addrs::gog::weapon_cannon_vftable_render;
      launcherRenderVA = game_addrs::gog::weapon_launcher_vftable_render;
      renderImplVA     = game_addrs::gog::weapon_render_impl;
      renderThunkVA    = game_addrs::gog::weapon_render_thunk;
      rayHitVA   = game_addrs::gog::collision_manager_ray_hit;
      rayHitIsRelease = true;    // same LTCG RayHit convention as Steam
      localIdsVA = game_addrs::gog::net_comm_local_player_id;
      break;
   default:
      return; // unknown build
   }

   // The raycast is optional: without it the local player simply converges at the
   // same fixed distance everyone else does, which is still far better than
   // leaving the direction alone.
   s_rayHitFn = (uintptr_t)resolve(exe_base, rayHitVA);
   s_rayHit   = rayHitIsRelease ? &rayhit_release_thunk : (RayHit_t)s_rayHitFn;

   s_localPlayerIds = localIdsVA ? (const int*)resolve(exe_base, localIdsVA) : nullptr;

   void* aimer_impl  = resolve(exe_base, implVA);
   void* aimer_thunk = resolve(exe_base, thunkVA);
   patch_vtable_slot(exe_base, cannonVA,   aimer_impl, aimer_thunk,
                     (void*)&hooked_cannon_OverrideAimer);
   patch_vtable_slot(exe_base, launcherVA, aimer_impl, aimer_thunk,
                     (void*)&hooked_cannon_OverrideAimer);
   if (grappleVA)
      patch_vtable_slot(exe_base, grappleVA, aimer_impl, aimer_thunk,
                        (void*)&hooked_cannon_OverrideAimer);

   // Render: the hook forwards to whatever it displaced, so the forward pointer
   // has to be published before either slot is claimed — a render can land on us
   // the moment the write does.  Both classes inherit the same Weapon::Render, so
   // one pointer serves both; take it from whichever slot still holds a vanilla
   // entry, so a slot somebody else already hooked can never become the target.
   void* render_impl  = resolve(exe_base, renderImplVA);
   void* render_thunk = resolve(exe_base, renderThunkVA);
   void* const* cannonRender   = (void* const*)resolve(exe_base, cannonRenderVA);
   void* const* launcherRender = (void* const*)resolve(exe_base, launcherRenderVA);

   void* vanillaRender = nullptr;
   if (*cannonRender == render_impl || *cannonRender == render_thunk)
      vanillaRender = *cannonRender;
   else if (*launcherRender == render_impl || *launcherRender == render_thunk)
      vanillaRender = *launcherRender;

   if (vanillaRender) {
      s_origWeaponRender = (WeaponRender_t)vanillaRender;
      patch_vtable_slot(exe_base, cannonRenderVA, render_impl, render_thunk,
                        (void*)&hooked_weapon_Render);
      patch_vtable_slot(exe_base, launcherRenderVA, render_impl, render_thunk,
                        (void*)&hooked_weapon_Render);
      if (grappleRenderVA)
         patch_vtable_slot(exe_base, grappleRenderVA, render_impl, render_thunk,
                           (void*)&hooked_weapon_Render);
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
