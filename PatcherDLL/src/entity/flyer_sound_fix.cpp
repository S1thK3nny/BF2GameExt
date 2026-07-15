#include "pch.h"
#include "flyer_sound_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>
#include <unordered_map>
#include <cmath>

// =============================================================================
// Flyer path-following engine-sound stutter fix (ported from RJP1992's
// dinput-hook branch, flyer_sound_patch.cpp; modtools-only there — the
// Steam/GOG port below is new)
// =============================================================================
// VehicleEngine::Update receives speedRatio and acceleration from
// EntityFlyer::Update.  During path following these jitter frame-to-frame:
// the Catmull-Rom spline has parametric speed variation, and the speed
// derivative (speedRatio - prevSpeed) / dt amplifies the noise.  The engine
// audio pitch/volume tracks these values, so AI flyers on paths stutter.
//
// Fix: EMA-smooth both values (tau = 0.05s) before the original runs.  Fast
// enough that free flight feels unchanged; damps the 5-15 Hz path jitter.
//
// Calling convention differs per build:
//   - Modtools (0x7600F0):  __thiscall, 14 stack args, RET 0x38 (verified).
//     (engineClass, dt, position, velocity, speedRatio, acceleration,
//      turbRatio, turbulence, unused1, unused2, trickEngine, isLocalPlayer,
//      param14, lastDistTarget)
//   - Steam 0x66CCB0 / GOG 0x66DD50 (byte-identical, GOG at the usual
//     +0x10A0 shift): LTCG hoisted dt into XMM2 — ECX=this, XMM2=dt,
//     13 stack args, RET 0x34 (callee cleans).  speedRatio is stack arg 4
//     ([esp+0x10] at entry), acceleration arg 5 ([esp+0x14]).
// =============================================================================

// ---------------------------------------------------------------------------
// Per-engine EMA state
// ---------------------------------------------------------------------------
struct SmoothState {
   float smoothSpeed;
   float smoothAccel;
   bool  initialized;
};

// Keyed by VehicleEngine*.  Entries for destroyed engines linger until map
// clear at uninstall — stale keys are only ever re-initialized, never read
// through, so that's harmless.
static std::unordered_map<void*, SmoothState> s_engineStates;

// EMA time constant in seconds.  Lower = heavier smoothing.  0.05s keeps
// free flight responsive while damping the path-following oscillation.
static constexpr float EMA_TAU = 0.05f;

// Shared smoothing core — updates the two values in place.
static void __cdecl smooth_engine_params(void* engine, float dt,
                                         float* speedRatio, float* acceleration)
{
   if (dt <= 0.0f)
      return;

   SmoothState& st = s_engineStates[engine];
   if (!st.initialized) {
      st.smoothSpeed = *speedRatio;
      st.smoothAccel = *acceleration;
      st.initialized = true;
   } else {
      const float alpha = 1.0f - expf(-dt / EMA_TAU);
      st.smoothSpeed += alpha * (*speedRatio   - st.smoothSpeed);
      st.smoothAccel += alpha * (*acceleration - st.smoothAccel);
   }
   *speedRatio   = st.smoothSpeed;
   *acceleration = st.smoothAccel;
}

// ---------------------------------------------------------------------------
// Hook: modtools (__thiscall, all 14 params on stack; __fastcall shim)
// ---------------------------------------------------------------------------
using fn_VEUpdate_thiscall = void(__fastcall*)(
   void* engine, void* edx,
   void* engineClass, float dt, void* position, void* velocity,
   float speedRatio, float acceleration, float turbRatio, float turbulence,
   float unused1, float unused2, float trickEngine, char isLocalPlayer,
   char param14, float lastDistTarget);

static fn_VEUpdate_thiscall original_VEUpdate_thiscall = nullptr;

static void __fastcall hooked_VEUpdate_thiscall(
   void* engine, void* edx,
   void* engineClass, float dt, void* position, void* velocity,
   float speedRatio, float acceleration, float turbRatio, float turbulence,
   float unused1, float unused2, float trickEngine, char isLocalPlayer,
   char param14, float lastDistTarget)
{
   smooth_engine_params(engine, dt, &speedRatio, &acceleration);
   original_VEUpdate_thiscall(
      engine, edx, engineClass, dt, position, velocity,
      speedRatio, acceleration, turbRatio, turbulence,
      unused1, unused2, trickEngine, isLocalPlayer, param14, lastDistTarget);
}

// ---------------------------------------------------------------------------
// Hook: Steam/GOG (LTCG: ECX=this, XMM2=dt, 13 stack args, callee cleans)
// ---------------------------------------------------------------------------
// Naked stub: smooth the two caller-owned stack slots in place, then jump to
// the original with every register (incl. XMM2 = dt) intact.  The C helper
// may clobber any XMM (all volatile on x86-32), so XMM2 is saved around it;
// no other XMM carries an input (only XMM2 is read by the original before
// being reloaded from memory).
static void* original_VEUpdate_regcall = nullptr;

__declspec(naked) static void hooked_VEUpdate_regcall()
{
   __asm {
      // entry: [esp]=ret, [esp+4]=engineClass, [esp+8]=pos, [esp+0xC]=vel,
      //        [esp+0x10]=speedRatio, [esp+0x14]=acceleration, ...
      push  ecx                     // this (also preserve)
      push  edx
      sub   esp, 16
      movups [esp], xmm2            // preserve dt across the C call

      lea   eax, [esp + 0x2C]       // &acceleration (orig esp+0x14, disp 24)
      push  eax
      lea   eax, [esp + 0x2C]       // &speedRatio   (orig esp+0x10, disp 28)
      push  eax
      sub   esp, 4
      movss [esp], xmm2             // dt
      push  ecx                     // engine
      call  smooth_engine_params
      add   esp, 16

      movups xmm2, [esp]            // restore dt
      add   esp, 16
      pop   edx
      pop   ecx
      jmp   [original_VEUpdate_regcall]
   }
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------
void flyer_sound_install(uintptr_t exe_base)
{
   uintptr_t updateVA;
   switch (g_build) {
      case GameBuild::Modtools: updateVA = game_addrs::modtools::vehicle_engine_update; break;
      case GameBuild::Steam:    updateVA = game_addrs::steam::vehicle_engine_update;    break;
      case GameBuild::GOG:      updateVA = game_addrs::gog::vehicle_engine_update;      break;
      default:                  return; // unsupported build
   }

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());

   if (g_build == GameBuild::Modtools) {
      original_VEUpdate_thiscall = (fn_VEUpdate_thiscall)resolve(exe_base, updateVA);
      DetourAttach(&(PVOID&)original_VEUpdate_thiscall, hooked_VEUpdate_thiscall);
   } else {
      original_VEUpdate_regcall = resolve(exe_base, updateVA);
      DetourAttach(&(PVOID&)original_VEUpdate_regcall, hooked_VEUpdate_regcall);
   }

   if (DetourTransactionCommit() != NO_ERROR) {
      original_VEUpdate_thiscall = nullptr;
      original_VEUpdate_regcall  = nullptr;
   }
}

void flyer_sound_uninstall()
{
   if (!original_VEUpdate_thiscall && !original_VEUpdate_regcall)
      return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_VEUpdate_thiscall)
      DetourDetach(&(PVOID&)original_VEUpdate_thiscall, hooked_VEUpdate_thiscall);
   if (original_VEUpdate_regcall)
      DetourDetach(&(PVOID&)original_VEUpdate_regcall, hooked_VEUpdate_regcall);
   DetourTransactionCommit();

   original_VEUpdate_thiscall = nullptr;
   original_VEUpdate_regcall  = nullptr;
   s_engineStates.clear();
}
