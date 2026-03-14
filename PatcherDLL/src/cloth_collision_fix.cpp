#include "pch.h"
#include "cloth_collision_fix.hpp"

#include <cstring>
#include <cmath>
#include <cstdio>
#include <detours.h>

// =============================================================================
// Cloth Collision Fixes
//
// BUG 1 (CRITICAL): EnforceCylinderCollision axis push direction.
//   When pushing a particle out through the nearest cap, the vanilla code
//   always pushes in the POSITIVE axis direction.  If the particle entered
//   from the negative side, it gets shoved THROUGH the entire cylinder.
//   Compare with EnforceBoxCollision which correctly uses sign(projection).
//
// BUG 2: EnforceCylinderCollision height doubled.
//   The vanilla code computes heightPen = (halfHeight + halfHeight) - |proj|
//   instead of halfHeight - |proj|, making cylinders twice as tall.
//
// BUG 3: Verlet velocity not corrected after collision.
//   Collision modifies pos but not old_pos, so the implicit Verlet velocity
//   (pos - old_pos) fights the correction every frame.
//
// FIX: Hook EnforceCylinderCollision with a corrected version (bugs 1+2).
//   Hook SatisfyConstraints for a final collision pass + old_pos correction
//   after all constraint iterations (bug 3).
// =============================================================================

static constexpr uintptr_t kUnrelocatedBase = 0x400000u;

static inline void* resolve(uintptr_t exe_base, uintptr_t unrelocated_addr)
{
   return (void*)((unrelocated_addr - kUnrelocatedBase) + exe_base);
}

typedef void (__cdecl* GameLog_t)(const char* fmt, ...);
static GameLog_t get_gamelog()
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   return (GameLog_t)((0x7E3D50 - kUnrelocatedBase) + base);
}

// ---------------------------------------------------------------------------
// Addresses (unrelocated, BF2_modtools)
// ---------------------------------------------------------------------------

static constexpr uintptr_t kSatisfyConstraints_addr       = 0x004cae40;
static constexpr uintptr_t kEnforceCollisions_addr         = 0x004cabd0;
static constexpr uintptr_t kEnforceCylinderCollision_addr  = 0x004c8660;

// ---------------------------------------------------------------------------
// EntityCloth struct offsets (from this pointer)
// ---------------------------------------------------------------------------

static constexpr int kPosBuffer_offset    = 0x20;
static constexpr int kOldPosBuffer_offset = 0x24;
static constexpr int kClothData_offset    = 0x114; // [0]=total, [1]=fixed count

// ---------------------------------------------------------------------------
// Function types
// ---------------------------------------------------------------------------

typedef void (__fastcall* fn_SatisfyConstraints_t)(void* ecx, void* edx,
                                                    void* param_2, int param_3, int param_4);
static fn_SatisfyConstraints_t original_SatisfyConstraints = nullptr;

typedef void (__fastcall* fn_EnforceCollisions_t)(void* ecx, void* edx,
                                                   void* param_2, int param_3);
static fn_EnforceCollisions_t fn_EnforceCollisions = nullptr;

// EnforceCylinderCollision: void __thiscall(this, float* matrix, float halfHeight, float radius)
typedef void (__fastcall* fn_EnforceCylinderCollision_t)(void* ecx, void* edx,
                                                          float* matrix, float halfHeight, float radius);
static fn_EnforceCylinderCollision_t original_EnforceCylinderCollision = nullptr;

static constexpr uint32_t kMaxStackParticles = 256;
static bool g_firstCorrectionLogged = false;

// ---------------------------------------------------------------------------
// Fixed EnforceCylinderCollision
//
// Vanilla bugs fixed:
//   1. Axis push always in positive direction — now uses sign(axisProj)
//   2. Height doubled (halfHeight*2) — now uses halfHeight directly
// ---------------------------------------------------------------------------

static void __fastcall hooked_EnforceCylinderCollision(
   void* ecx, void* /*edx*/, float* mat, float halfHeight, float radius)
{
   uintptr_t self = (uintptr_t)ecx;

   float*    posBuffer = *(float**)(self + kPosBuffer_offset);
   uint32_t* clothData = *(uint32_t**)(self + kClothData_offset);
   if (!posBuffer || !clothData) return;

   uint32_t totalCount = clothData[0];
   uint32_t fixedCount = clothData[1];

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

      // Height check: particle must be within ±halfHeight of center
      float heightPen = halfHeight - std::abs(axisProj);
      if (heightPen <= 0.0f)
         continue;

      // Project onto radial plane (X and Z axes of matrix)
      float radialX = dx * mat[0] + dy * mat[1] + dz * mat[2];
      float radialZ = dx * mat[8] + dy * mat[9] + dz * mat[10];
      float radialDist = std::sqrt(radialX * radialX + radialZ * radialZ);

      // Radial check: particle must be within radius
      float radialPen = radius - radialDist;
      if (radialPen <= 0.0f)
         continue;

      // Inside cylinder — push out along axis of minimum penetration
      if (heightPen <= radialPen) {
         // Push along cylinder axis toward nearest cap
         // FIX: use sign(axisProj) so we push AWAY from center, not always positive
         float sign = (axisProj >= 0.0f) ? 1.0f : -1.0f;
         pos[0] += sign * heightPen * mat[4];
         pos[1] += sign * heightPen * mat[5];
         pos[2] += sign * heightPen * mat[6];
      }
      else {
         // Push radially outward
         if (radialDist > 1e-6f) {
            float scale = radialPen / radialDist;
            pos[0] += (radialX * mat[0] + radialZ * mat[8]) * scale;
            pos[1] += (radialX * mat[1] + radialZ * mat[9]) * scale;
            pos[2] += (radialX * mat[2] + radialZ * mat[10]) * scale;
         }
      }
   }
}

// ---------------------------------------------------------------------------
// SatisfyConstraints hook — final collision pass + old_pos velocity fix
// ---------------------------------------------------------------------------

static void __fastcall hooked_SatisfyConstraints(void* ecx, void* edx,
                                                  void* param_2, int param_3, int param_4)
{
   // Run the full vanilla constraint solver (constraints + collisions)
   original_SatisfyConstraints(ecx, edx, param_2, param_3, param_4);

   // --- Final collision pass: give collision the last word ---
   uintptr_t self = (uintptr_t)ecx;

   float*    posBuffer    = *(float**)(self + kPosBuffer_offset);
   float*    oldPosBuffer = *(float**)(self + kOldPosBuffer_offset);
   uint32_t* clothData    = *(uint32_t**)(self + kClothData_offset);

   if (!posBuffer || !oldPosBuffer || !clothData)
      return;

   uint32_t totalCount = clothData[0];
   uint32_t fixedCount = clothData[1];
   if (fixedCount >= totalCount)
      return;

   uint32_t movableCount = totalCount - fixedCount;
   uint32_t floatCount   = movableCount * 3;

   // Snapshot positions before final collision pass
   float stackBuf[kMaxStackParticles * 3];
   float* snapshot = (movableCount <= kMaxStackParticles) ? stackBuf : new float[floatCount];

   float* movablePos = posBuffer + fixedCount * 3;
   std::memcpy(snapshot, movablePos, floatCount * sizeof(float));

   // Final collision enforcement — after all constraint iterations are done
   fn_EnforceCollisions(ecx, nullptr, param_2, param_3);

   // Fix old_pos for any particle displaced by the final collision pass
   float* movableOldPos = oldPosBuffer + fixedCount * 3;
   uint32_t fixedThisCall = 0;

   for (uint32_t i = 0; i < movableCount; i++) {
      uint32_t b = i * 3;

      float dx = movablePos[b]     - snapshot[b];
      float dy = movablePos[b + 1] - snapshot[b + 1];
      float dz = movablePos[b + 2] - snapshot[b + 2];

      if (dx == 0.0f && dy == 0.0f && dz == 0.0f)
         continue;

      float dLen2 = dx * dx + dy * dy + dz * dz;
      if (dLen2 < 1e-10f)
         continue;

      fixedThisCall++;

      // Original velocity: snapshot_pos - old_pos
      float vx = snapshot[b]     - movableOldPos[b];
      float vy = snapshot[b + 1] - movableOldPos[b + 1];
      float vz = snapshot[b + 2] - movableOldPos[b + 2];

      float proj = (vx * dx + vy * dy + vz * dz) / dLen2;

      if (proj < 0.0f) {
         // Kill penetrating component, preserve tangential sliding
         float vx_tang = vx - proj * dx;
         float vy_tang = vy - proj * dy;
         float vz_tang = vz - proj * dz;

         movableOldPos[b]     = movablePos[b]     - vx_tang;
         movableOldPos[b + 1] = movablePos[b + 1] - vy_tang;
         movableOldPos[b + 2] = movablePos[b + 2] - vz_tang;
      }
      else {
         movableOldPos[b]     += dx;
         movableOldPos[b + 1] += dy;
         movableOldPos[b + 2] += dz;
      }
   }

   if (fixedThisCall > 0 && !g_firstCorrectionLogged) {
      g_firstCorrectionLogged = true;
      auto fn_log = get_gamelog();
      fn_log("[ClothCollisionFix] ACTIVE — final pass corrected %u particles "
             "(total=%u, fixed=%u, movable=%u)\n",
             fixedThisCall, totalCount, fixedCount, movableCount);
   }

   if (snapshot != stackBuf)
      delete[] snapshot;
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------

void cloth_collision_fix_install(uintptr_t exe_base)
{
   original_SatisfyConstraints = (fn_SatisfyConstraints_t)resolve(exe_base, kSatisfyConstraints_addr);
   fn_EnforceCollisions = (fn_EnforceCollisions_t)resolve(exe_base, kEnforceCollisions_addr);
   original_EnforceCylinderCollision = (fn_EnforceCylinderCollision_t)resolve(exe_base, kEnforceCylinderCollision_addr);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SatisfyConstraints, hooked_SatisfyConstraints);
   DetourAttach(&(PVOID&)original_EnforceCylinderCollision, hooked_EnforceCylinderCollision);
   LONG rc = DetourTransactionCommit();

   auto fn_log = get_gamelog();
   fn_log("[ClothCollisionFix] install: commit=%ld\n", rc);
}

void cloth_collision_fix_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_SatisfyConstraints)
      DetourDetach(&(PVOID&)original_SatisfyConstraints, hooked_SatisfyConstraints);
   if (original_EnforceCylinderCollision)
      DetourDetach(&(PVOID&)original_EnforceCylinderCollision, hooked_EnforceCylinderCollision);
   DetourTransactionCommit();
}
