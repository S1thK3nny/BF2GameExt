#include "pch.h"
#include "walker_footstep_fix.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <detours.h>

// See walker_footstep_fix.hpp.
//
// EntityWalker::DoFootImpactEffects is only called from EntityWalker::UpdateState,
// before the walk anim is advanced. Its 0.1 is a shared .rdata constant, so the
// compare operands are repointed at g_threshold rather than the constant
// rewritten. modtools compares once (x87 FCOMP, both branches test the flags),
// Steam/GOG load it twice (COMISS and MOVSS).

// thiscall(dt, fwd, strafe, float* speed, turn), RET 0x14 on every build.
typedef void (__fastcall* fn_UpdateState_t)(void* ecx, void* edx, float dt, float a, float b,
                                            float* out, float c);

static fn_UpdateState_t g_origUpdateState = nullptr;

// Stock threshold is 0.1 m per frame, taken to mean 30 fps.
static constexpr float kStepSpeed = 3.0f;

static float g_threshold = 0.1f;

static uint32_t* g_refs[2]    = {};
static uint32_t  g_origRef[2] = {};

static void __fastcall hooked_UpdateState(void* ecx, void* edx, float dt, float a, float b,
                                          float* out, float c)
{
   if (dt > 0.0f) g_threshold = kStepSpeed * dt;
   g_origUpdateState(ecx, edx, dt, a, b, out, c);
}

void walker_footstep_fix_install(uintptr_t exe_base)
{
   if (g_addr->walker_update_state == 0 || g_addr->walker_step_threshold == 0 ||
       g_addr->walker_step_threshold_ref0 == 0)
      return;

   const uintptr_t refAddrs[2] = { g_addr->walker_step_threshold_ref0,
                                   g_addr->walker_step_threshold_ref1 };
   const uint32_t  expected    = (uint32_t)resolve(exe_base, g_addr->walker_step_threshold);

   // Each operand must still point at the 0.1 constant.
   for (int i = 0; i < 2; ++i) {
      if (refAddrs[i] == 0) continue;
      const uint32_t* ref = (const uint32_t*)resolve(exe_base, refAddrs[i]);
      if (*ref != expected) {
         install_log("[WalkerFootstep] unexpected operand at %08X, skipping", (unsigned)refAddrs[i]);
         return;
      }
   }

   g_origUpdateState = reinterpret_cast<fn_UpdateState_t>(resolve(exe_base, g_addr->walker_update_state));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG ra = DetourAttach(reinterpret_cast<PVOID*>(&g_origUpdateState), hooked_UpdateState);
   const LONG rc = DetourTransactionCommit();

   if (ra != NO_ERROR || rc != NO_ERROR) {
      g_origUpdateState = nullptr;
      install_log("[WalkerFootstep] detour failed (%ld/%ld)", ra, rc);
      return;
   }

   // .text is RW during install.
   for (int i = 0; i < 2; ++i) {
      if (refAddrs[i] == 0) continue;
      g_refs[i]    = (uint32_t*)resolve(exe_base, refAddrs[i]);
      g_origRef[i] = *g_refs[i];
      *g_refs[i]   = (uint32_t)(uintptr_t)&g_threshold;
   }

   install_log("[WalkerFootstep] installed (UpdateState %08X)", (unsigned)g_addr->walker_update_state);
}

void walker_footstep_fix_uninstall()
{
   for (int i = 0; i < 2; ++i) {
      if (!g_refs[i]) continue;
      protected_write(g_refs[i], &g_origRef[i], sizeof(g_origRef[i]));
      g_refs[i] = nullptr;
   }

   if (!g_origUpdateState) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(reinterpret_cast<PVOID*>(&g_origUpdateState), hooked_UpdateState);
   DetourTransactionCommit();
   g_origUpdateState = nullptr;
}
