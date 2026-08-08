#include "pch.h"
#include "util/ray_hit.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

// =============================================================================
// CollisionManager::RayHit - build-specific marshalling.
//
// Extracted from weapon/barrel_fire_origin.cpp, which was the first caller.  The
// naked thunk below is the only place the release calling convention is encoded;
// duplicating it per feature is how the two copies eventually drift apart.
// =============================================================================

static uintptr_t s_rayHitFn = 0;      // resolved engine address
static RayHit_t  s_rayHit   = nullptr; // what callers invoke

// Marshals the __cdecl signature onto the release build's register layout.
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

bool ray_hit_init(uintptr_t exe_base)
{
   if (s_rayHit) return true;   // already resolved by an earlier installer

   uintptr_t rayHitVA;
   bool      isRelease;

   switch (g_build) {
   case GameBuild::Modtools:
      rayHitVA  = game_addrs::modtools::collision_manager_ray_hit;
      isRelease = false;   // plain __cdecl, result in ST(0)
      break;
   case GameBuild::Steam:
      rayHitVA  = game_addrs::steam::collision_manager_ray_hit;
      isRelease = true;    // ECX/EDX/XMM2 + six stack args, result in XMM0
      break;
   case GameBuild::GOG:
      rayHitVA  = game_addrs::gog::collision_manager_ray_hit;
      isRelease = true;    // same LTCG convention as Steam
      break;
   default:
      return false;        // unknown build
   }

   if (rayHitVA == 0) return false;

   s_rayHitFn = (uintptr_t)resolve(exe_base, rayHitVA);
   s_rayHit   = isRelease ? &rayhit_release_thunk : (RayHit_t)s_rayHitFn;
   return true;
}

RayHit_t ray_hit_get()
{
   return s_rayHit;
}
