#include "pch.h"
#include "cloth_collision_fix.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <cstring>
#include <cmath>
#include <cstdio>
#include <climits>
#include <new>
#include <detours.h>

// =============================================================================
// Cloth simulation fixes
//
// Pipeline (see docs / memory "cloth-system-full-pipeline"):
//
//   EntityCloth::Render(mat, pose, alpha, color, flags, ITERATIONS)
//     SetWorldMatrix(worldMat, dt)
//     InternalUpdate(mat, pose, ITERATIONS, dt)          <- frame-gated on +0x80
//        AccumulateForces(dt) -> Verlet(dt)
//        -> SatisfyConstraints(mat, pose, ITERATIONS) -> ComputeNormals()
//
//   SatisfyConstraints:
//     RestoreFixedPoints(pose)
//     for i in 0..ITERATIONS:
//        cross / bend / stretch constraints
//        EnforceCollisions(mat, pose)      <- already LAST in every iteration
//
// FIXED HERE
//
//   1. Cylinder half-height.  EnforceCylinderCollision tests
//      |axisProj| < halfHeight*2, i.e. it treats the authored `height` as a
//      half-extent AND doubles it.  The engine's own debug draw
//      (render_cloth_connections) draws the cylinder as an OBB with Y
//      half-extent height*0.5, and EnforceBoxCollision tests `height - |proj|`
//      on real half-extents.  So `height` is the FULL height and the vanilla
//      cylinder is 4x too tall.
//
//   2. Cylinder axis push direction.  Vanilla always pushes along +axis, so a
//      particle that entered from the negative cap gets shoved through the
//      whole cylinder.  EnforceBoxCollision does the same push with
//      sign(projection) and is the correct reference.
//
//   3. Verlet velocity after collision.  Collision moves pos but not old_pos,
//      so the implicit velocity (pos - old_pos) drives the particle straight
//      back in next step.  Corrected around EnforceCollisions - which is where
//      the displacement actually happens.  (The previous version of this file
//      hooked SatisfyConstraints and ran an extra collision pass afterwards;
//      that could never do anything, because collision is already the last
//      step of each iteration and every call site passes ITERATIONS = 1, so
//      the extra pass was idempotent and the snapshot diff was always zero.)
//
//   4. NaN recovery.  EnforceConstraint divides by the current segment length
//      and the cylinder divides by the radial distance, both unguarded.  Two
//      coincident particles produce a NaN that spreads through the whole
//      particle array in one frame; after that every collision comparison is
//      false and the cloth is permanently broken.  Rather than detour the
//      per-constraint hot path, non-finite particles are reset to their rest
//      position once per collision pass.
//
//   5. Fixed simulation timestep.  Retail/modtools integrate with the raw
//      frame delta, unclamped.  Verlet is only stable at a constant dt, and
//      AccumulateForces/SetWorldMatrix additionally scale by 1/dt - so high
//      framerates inflate the drag term and any hitch makes acc*dt^2 teleport
//      particles clean past the collision volumes (the test is a static
//      point-in-volume projection, no swept test, so a particle that lands
//      beyond the far side reports no penetration at all).  The later Phantom
//      dev build substeps at exactly 1/30 s with the accumulator clamped to
//      0.13333 s; that loop is reproduced here.
//
//   6. Solver iterations.  Every shipped call site passes 1, which barely
//      converges.  Raised via the InternalUpdate hook.
// =============================================================================

// ---------------------------------------------------------------------------
// EntityCloth / ClothData offsets (identical on all three builds)
// ---------------------------------------------------------------------------

static constexpr int kPosBuffer_offset        = 0x20;  // PblVector3* m_pPos
static constexpr int kOldPosBuffer_offset     = 0x24;  // PblVector3* m_pOldPos
static constexpr int kLastFrameUpdated_offset = 0x80;  // int
static constexpr int kClothData_offset        = 0x114; // ClothData*

static constexpr int kData_numParticles_offset   = 0x00;
static constexpr int kData_numFixedPoints_offset = 0x04;
static constexpr int kData_restPos_offset        = 0x24; // PblVector3* (rest pose)
// NOTE on ClothCollision.depth (+0x50) and cylinders:
//   EnforceCollisions dispatches cylinders as
//       EnforceCylinderCollision(this, M, vol->height, vol->width)
//   and never passes `depth`, so the cross-section is a circle of radius
//   `width`.  That looks like a bug next to the engine's own debug draw, which
//   uses +/-width on X and +/-depth on Z - but every cylinder in every munged
//   asset checked (praetorian .model, firstperson.lvl, side lvls) has
//   depth == 0.0: the munger simply never writes the field for cylinders.
//   So the circle IS correct, and it is the DEBUG DRAW that is degenerate -
//   a zero-thickness OBB, which is why render_cloth_connections shows
//   cylinders as flat rectangles.  An elliptical cross-section was implemented
//   and reverted (2026-08-05) as provably dead code.

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

// Fixed simulation timestep.  Phantom uses 1/30 with a 4-step ceiling, but that
// HALVES the sim rate against vanilla at 60 FPS (vanilla steps once per rendered
// frame), which reads as visibly steppy cloth.  1/60 keeps the cadence identical
// to vanilla at 60 FPS - one substep per frame - while still giving the solver a
// constant dt, which is the whole point: Verlet is only stable at a fixed step,
// and AccumulateForces/SetWorldMatrix scale by 1/dt.
// Above 60 FPS the sim runs at 60 Hz; below it, the accumulator catches up until
// the ceiling, so a hitch costs at most 4 steps instead of one giant one.
static constexpr float kSimStep     = 1.0f / 60.0f; // 0.016666668
static constexpr float kSimMaxAccum = 4.0f / 60.0f; // 0.06666667 -> max 4 substeps

// Gauss-Seidel passes per substep.  Vanilla ships 1 at every call site; raising
// this multiplies the per-substep constraint AND collision cost, so it stays at
// the stock value until the fixed timestep alone has been play-tested.
static constexpr uint32_t kMinIterations = 1;

// ---------------------------------------------------------------------------
// Function types
// ---------------------------------------------------------------------------

// EntityCloth::InternalUpdate - thiscall(PblMatrix*, RedPose*, uint, float), RET 0x10
typedef char (__fastcall* fn_InternalUpdate_t)(void* ecx, void* edx,
                                                void* mat, void* pose,
                                                uint32_t iterations, float dt);
static fn_InternalUpdate_t original_InternalUpdate = nullptr;

// EntityCloth::EnforceCollisions - thiscall(PblMatrix*, RedPose*), RET 8
typedef void (__fastcall* fn_EnforceCollisions_t)(void* ecx, void* edx,
                                                   void* mat, void* pose);
static fn_EnforceCollisions_t original_EnforceCollisions = nullptr;

// EntityCloth::EnforceCylinderCollision - thiscall(float* mat, float height, float radius)
typedef void (__fastcall* fn_EnforceCylinderCollision_t)(void* ecx, void* edx,
                                                          float* mat, float height, float radius);
static fn_EnforceCylinderCollision_t original_EnforceCylinderCollision = nullptr;

// ---------------------------------------------------------------------------
// Fixed EnforceCylinderCollision
// ---------------------------------------------------------------------------

static void __cdecl enforce_cylinder_impl(void* ecx, float* mat, float height, float radius)
{
   uintptr_t self = (uintptr_t)ecx;

   float*    posBuffer = *(float**)(self + kPosBuffer_offset);
   uint32_t* clothData = *(uint32_t**)(self + kClothData_offset);
   if (!posBuffer || !clothData) return;

   uint32_t totalCount = clothData[kData_numParticles_offset / 4];
   uint32_t fixedCount = clothData[kData_numFixedPoints_offset / 4];

   // `height` is the FULL cylinder height - vanilla used height*2 as the HALF
   // height, making the volume 4x too tall.  Confirmed against real assets:
   // praetorian cape cylinders are bone_l_thigh w=0.125 h=0.75 and
   // bone_r_calf w=0.125 h=0.60, i.e. a 75 cm thigh / 60 cm calf at a 12.5 cm
   // radius.  Vanilla turned each of those into a 3 m tall cylinder.
   const float halfHeight = height * 0.5f;

   // Matrix layout (4x4, row-major as float[16]):
   //   Axis X: mat[0..2]     Axis Y (cylinder axis): mat[4..6]
   //   Axis Z: mat[8..10]    Translation (center):   mat[12..14]

   for (uint32_t i = fixedCount; i < totalCount; i++) {
      float* pos = posBuffer + i * 3;

      // Vector from cylinder center to particle
      float dx = pos[0] - mat[12];
      float dy = pos[1] - mat[13];
      float dz = pos[2] - mat[14];

      // Project onto cylinder axis (Y axis of matrix)
      float axisProj = dx * mat[4] + dy * mat[5] + dz * mat[6];

      float heightPen = halfHeight - std::abs(axisProj);
      if (heightPen <= 0.0f)
         continue;

      // Project onto radial plane (X and Z axes of matrix)
      float radialX = dx * mat[0] + dy * mat[1] + dz * mat[2];
      float radialZ = dx * mat[8] + dy * mat[9] + dz * mat[10];
      float radialDist = std::sqrt(radialX * radialX + radialZ * radialZ);

      float radialPen = radius - radialDist;
      if (radialPen <= 0.0f)
         continue;

      // Inside the cylinder - push out along the axis of least penetration
      if (heightPen <= radialPen) {
         // Vanilla always pushed along +axis; use sign(axisProj) so the
         // particle leaves through the cap it entered.
         float sign = (axisProj >= 0.0f) ? 1.0f : -1.0f;
         pos[0] += sign * heightPen * mat[4];
         pos[1] += sign * heightPen * mat[5];
         pos[2] += sign * heightPen * mat[6];
      }
      else if (radialDist > 1e-6f) {
         // Vanilla divides by radialDist unguarded; a particle exactly on the
         // axis produces a NaN that spreads through the whole cloth.
         float scale = radialPen / radialDist;
         pos[0] += (radialX * mat[0] + radialZ * mat[8]) * scale;
         pos[1] += (radialX * mat[1] + radialZ * mat[9]) * scale;
         pos[2] += (radialX * mat[2] + radialZ * mat[10]) * scale;
      }
      else {
         // Degenerate: on the axis, no radial direction.  Push along +X.
         pos[0] += radialPen * mat[0];
         pos[1] += radialPen * mat[1];
         pos[2] += radialPen * mat[2];
      }
   }
}

// Modtools (debug) entry: plain __thiscall - matrix, height, radius all on the
// stack.
static void __fastcall hooked_EnforceCylinderCollision(
   void* ecx, void* /*edx*/, float* mat, float height, float radius)
{
   enforce_cylinder_impl(ecx, mat, height, radius);
}

// Steam/GOG (release/LTCG) entry: ECX=this, one stack arg (float* matrix,
// RET 4), height in XMM2, radius in XMM3.  Naked thunk re-marshals to the
// shared __cdecl impl (which preserves EBX/ESI/EDI for the LTCG caller).
__declspec(naked) static void hooked_EnforceCylinderCollision_steam()
{
   __asm {
      push ebp
      mov  ebp, esp
      sub  esp, 8
      movss dword ptr [esp], xmm2      // height
      movss dword ptr [esp + 4], xmm3  // radius
      push dword ptr [ebp + 8]         // float* matrix
      push ecx                         // this
      call enforce_cylinder_impl
      add  esp, 16
      pop  ebp
      ret  4
   }
}

// ---------------------------------------------------------------------------
// EnforceCollisions hook - Verlet velocity correction + NaN recovery
//
// This is where collision actually displaces particles, so it is the only
// correct place to fix up old_pos.
// ---------------------------------------------------------------------------

static constexpr uint32_t kMaxStackParticles = 512;

static inline bool is_finite3(const float* v)
{
   return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

static void __fastcall hooked_EnforceCollisions(void* ecx, void* /*edx*/,
                                                 void* mat, void* pose)
{
   uintptr_t self = (uintptr_t)ecx;

   float*    posBuffer    = *(float**)(self + kPosBuffer_offset);
   float*    oldPosBuffer = *(float**)(self + kOldPosBuffer_offset);
   uint32_t* clothData    = *(uint32_t**)(self + kClothData_offset);

   if (!posBuffer || !oldPosBuffer || !clothData) {
      original_EnforceCollisions(ecx, nullptr, mat, pose);
      return;
   }

   uint32_t totalCount = clothData[kData_numParticles_offset / 4];
   uint32_t fixedCount = clothData[kData_numFixedPoints_offset / 4];
   if (fixedCount >= totalCount) {
      original_EnforceCollisions(ecx, nullptr, mat, pose);
      return;
   }

   float* restPos = *(float**)((uintptr_t)clothData + kData_restPos_offset);

   uint32_t movableCount = totalCount - fixedCount;
   uint32_t floatCount   = movableCount * 3;

   float* movablePos    = posBuffer + fixedCount * 3;
   float* movableOldPos = oldPosBuffer + fixedCount * 3;

   // --- NaN recovery -------------------------------------------------------
   // Once a single particle goes non-finite the constraint solver spreads it
   // through the whole array within a frame, and every collision comparison
   // then evaluates false.  Reset offenders to their rest position with zero
   // velocity so the cloth recovers instead of staying broken forever.
   for (uint32_t i = 0; i < movableCount; i++) {
      uint32_t b = i * 3;
      if (is_finite3(movablePos + b) && is_finite3(movableOldPos + b))
         continue;

      const float* rest = restPos ? (restPos + (fixedCount + i) * 3) : nullptr;
      for (int c = 0; c < 3; c++) {
         float v = rest ? rest[c] : 0.0f;
         if (!std::isfinite(v)) v = 0.0f;
         movablePos[b + c]    = v;
         movableOldPos[b + c] = v;
      }
   }

   // --- Snapshot, collide, then fix up the implicit Verlet velocity --------
   float  stackBuf[kMaxStackParticles * 3];
   float* snapshot = (movableCount <= kMaxStackParticles)
                        ? stackBuf
                        : new (std::nothrow) float[floatCount];

   if (!snapshot) {
      original_EnforceCollisions(ecx, nullptr, mat, pose);
      return;
   }

   std::memcpy(snapshot, movablePos, floatCount * sizeof(float));

   original_EnforceCollisions(ecx, nullptr, mat, pose);

   for (uint32_t i = 0; i < movableCount; i++) {
      uint32_t b = i * 3;

      float dx = movablePos[b]     - snapshot[b];
      float dy = movablePos[b + 1] - snapshot[b + 1];
      float dz = movablePos[b + 2] - snapshot[b + 2];

      float dLen2 = dx * dx + dy * dy + dz * dz;
      if (dLen2 < 1e-10f)
         continue; // not displaced by this pass

      // Velocity going into the pass, in Verlet's implicit form.
      float vx = snapshot[b]     - movableOldPos[b];
      float vy = snapshot[b + 1] - movableOldPos[b + 1];
      float vz = snapshot[b + 2] - movableOldPos[b + 2];

      float proj = (vx * dx + vy * dy + vz * dz) / dLen2;

      if (proj < 0.0f) {
         // Moving into the surface: kill the penetrating component, keep the
         // tangential part so the cloth still slides.
         float vx_tang = vx - proj * dx;
         float vy_tang = vy - proj * dy;
         float vz_tang = vz - proj * dz;

         movableOldPos[b]     = movablePos[b]     - vx_tang;
         movableOldPos[b + 1] = movablePos[b + 1] - vy_tang;
         movableOldPos[b + 2] = movablePos[b + 2] - vz_tang;
      }
      else {
         // Already moving out: carry old_pos along so velocity is preserved.
         movableOldPos[b]     += dx;
         movableOldPos[b + 1] += dy;
         movableOldPos[b + 2] += dz;
      }
   }

   if (snapshot != stackBuf)
      delete[] snapshot;
}

// ---------------------------------------------------------------------------
// InternalUpdate hook - fixed timestep + iteration count
//
// The per-instance accumulator lives DLL-side rather than in a spare
// EntityCloth slot, so nothing depends on unused struct space being free.
// Open addressing, 8-slot probe window, oldest-stamp eviction: an entry for a
// destroyed cloth stops being stamped and gets reclaimed on its own.  If the
// window is somehow full of live entries we fall back to vanilla behaviour for
// that cloth rather than freezing it.
// ---------------------------------------------------------------------------

namespace {

struct AccumEntry {
   const void* key;
   float       accum;
   uint32_t    stamp;
};

constexpr uint32_t kAccumSlots = 512;
constexpr uint32_t kAccumProbe = 8;

AccumEntry g_accum[kAccumSlots];
uint32_t   g_accumStamp = 0;

// Returns the entry for `key`, claiming or evicting a slot if needed.
// Never returns null.
AccumEntry* accum_find(const void* key)
{
   uint32_t h = (uint32_t)(((uintptr_t)key >> 4) * 2654435761u) % kAccumSlots;

   AccumEntry* oldest = nullptr;
   for (uint32_t i = 0; i < kAccumProbe; i++) {
      AccumEntry* e = &g_accum[(h + i) % kAccumSlots];
      if (e->key == key)
         return e;
      if (e->key == nullptr) {
         e->key   = key;
         e->accum = 0.0f;
         return e;
      }
      if (!oldest || (int32_t)(e->stamp - oldest->stamp) < 0)
         oldest = e;
   }

   // Window full of other live cloths - take the least recently stamped one.
   oldest->key   = key;
   oldest->accum = 0.0f;
   return oldest;
}

void accum_reset()
{
   std::memset(g_accum, 0, sizeof(g_accum));
   g_accumStamp = 0;
}

} // namespace

static char __fastcall hooked_InternalUpdate(void* ecx, void* /*edx*/,
                                              void* mat, void* pose,
                                              uint32_t iterations, float dt)
{
   if (iterations < kMinIterations)
      iterations = kMinIterations;

   // Reject a dt we cannot integrate sanely (NaN, negative, load-time spike).
   // Fall through to vanilla so behaviour degrades rather than breaking.
   if (!(dt > 0.0f) || !(dt < 10.0f))
      return original_InternalUpdate(ecx, nullptr, mat, pose, iterations, dt);

   AccumEntry* e = accum_find(ecx);
   e->stamp = ++g_accumStamp;

   float prev = e->accum;
   if (!(prev >= 0.0f) || !(prev <= kSimMaxAccum))
      prev = 0.0f; // freshly claimed / evicted slot, or garbage

   float accum = prev + dt;
   if (accum > kSimMaxAccum)
      accum = kSimMaxAccum;

   if (accum < kSimStep) {
      e->accum = accum;
      return 1; // not enough accumulated time for a step yet
   }

   // The vanilla body is gated on m_lastFrameUpdated < GetFrameNumber() and
   // stamps that field when it runs.  Run the first substep untouched so that
   // gate still decides; if it blocked, leave the accumulator alone.
   int32_t* lastFrame = (int32_t*)((uintptr_t)ecx + kLastFrameUpdated_offset);
   int32_t  before    = *lastFrame;

   char rc = original_InternalUpdate(ecx, nullptr, mat, pose, iterations, kSimStep);

   if (*lastFrame == before) {
      e->accum = prev; // gate blocked (already updated this frame, or disabled)
      return rc;
   }

   accum -= kSimStep;

   // Remaining substeps: defeat the once-per-frame gate.  The final original
   // call restamps it to the current frame, exactly as vanilla leaves it.
   while (accum >= kSimStep) {
      *lastFrame = INT_MIN;
      rc = original_InternalUpdate(ecx, nullptr, mat, pose, iterations, kSimStep);
      accum -= kSimStep;
   }

   e->accum = accum;
   return rc;
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------

static PVOID g_cylinderHook = nullptr; // build-specific entry attached to the detour

void cloth_collision_fix_install(uintptr_t exe_base)
{
   uintptr_t internal_update = 0, enforce = 0, cylinder = 0;

   switch (g_build) {
   case GameBuild::Modtools: {
      using namespace game_addrs::modtools;
      internal_update = cloth_internal_update;
      enforce         = cloth_enforce_collisions;
      cylinder        = cloth_enforce_cylinder_coll;
      g_cylinderHook  = (PVOID)hooked_EnforceCylinderCollision;
   } break;
   case GameBuild::Steam: {
      using namespace game_addrs::steam;
      internal_update = cloth_internal_update;
      enforce         = cloth_enforce_collisions;
      cylinder        = cloth_enforce_cylinder_coll;
      g_cylinderHook  = (PVOID)hooked_EnforceCylinderCollision_steam;
   } break;
   case GameBuild::GOG: {
      using namespace game_addrs::gog;
      internal_update = cloth_internal_update;
      enforce         = cloth_enforce_collisions;
      cylinder        = cloth_enforce_cylinder_coll;
      // Same release/LTCG codegen as Steam, so the same naked thunk applies.
      g_cylinderHook  = (PVOID)hooked_EnforceCylinderCollision_steam;
   } break;
   default:
      return; // unknown build
   }

   accum_reset();

   original_InternalUpdate = (fn_InternalUpdate_t)resolve(exe_base, internal_update);
   original_EnforceCollisions = (fn_EnforceCollisions_t)resolve(exe_base, enforce);
   original_EnforceCylinderCollision = (fn_EnforceCylinderCollision_t)resolve(exe_base, cylinder);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_InternalUpdate, hooked_InternalUpdate);
   DetourAttach(&(PVOID&)original_EnforceCollisions, hooked_EnforceCollisions);
   DetourAttach(&(PVOID&)original_EnforceCylinderCollision, g_cylinderHook);
   LONG rc = DetourTransactionCommit();

   (void)rc;
}

void cloth_collision_fix_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_InternalUpdate)
      DetourDetach(&(PVOID&)original_InternalUpdate, hooked_InternalUpdate);
   if (original_EnforceCollisions)
      DetourDetach(&(PVOID&)original_EnforceCollisions, hooked_EnforceCollisions);
   if (original_EnforceCylinderCollision && g_cylinderHook)
      DetourDetach(&(PVOID&)original_EnforceCylinderCollision, g_cylinderHook);
   DetourTransactionCommit();
}
