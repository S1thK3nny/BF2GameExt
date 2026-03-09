#include "pch.h"

#include "flyer_sound_patch.hpp"
#include "cfile.hpp"
#include <detours.h>
#include <unordered_map>
#include <cmath>

// ============================================================================
// Flyer path-following engine sound stutter fix
//
// VehicleEngine::Update receives speedRatio and acceleration from
// EntityFlyer::Update.  During path following these values jitter due
// to Catmull-Rom parametric speed variation and the speed derivative
// (speedRatio - prevSpeed) / dt / accel amplifying frame-to-frame noise.
//
// We EMA-smooth both values with a fast time constant (tau ~0.05s) so
// free flight is effectively unaffected but path-following jitter is
// damped out.
// ============================================================================

// ---------------------------------------------------------------------------
// Per-build addresses
// ---------------------------------------------------------------------------
struct flyer_sound_addrs {
   uintptr_t id_rva;
   uint64_t  id_expected;
   uintptr_t ve_update_offset;   // VehicleEngine::Update
};

static const flyer_sound_addrs MODTOOLS = {
   .id_rva           = 0x62b59c,
   .id_expected      = 0x746163696c707041,   // "Applicat"
   .ve_update_offset = 0x3600f0,             // VA 0x007600f0
};

// Steam/GOG TBD — search for hash constant 0x43f6a333 or RET 0x38

// ---------------------------------------------------------------------------
// VehicleEngine::Update hook
//
// Calling convention: __thiscall (ECX = VehicleEngine*, 14 stack params)
// Epilogue: RET 0x38  (14 * 4 = 56 bytes cleaned)
//
// We model this as __fastcall with void* edx placeholder + 14 stack params.
// ---------------------------------------------------------------------------

// Original function pointer — Detours will overwrite this
typedef void (__fastcall* fn_VehicleEngineUpdate)(
   void*    thisEngine,   // ECX
   void*    edx,          // EDX (unused, __thiscall shim)
   void*    engineClass,  // arg1  VehicleEngineClass*
   float    dt,           // arg2
   void*    position,     // arg3  float[3]*
   void*    velocity,     // arg4  float[3]*
   float    speedRatio,   // arg5  0-1, JITTERY
   float    acceleration, // arg6  speed derivative, JITTERY
   float    turbRatio,    // arg7
   float    turbulence,   // arg8
   float    unused1,      // arg9  always 0.0
   float    unused2,      // arg10 always 0.0
   float    trickEngine,  // arg11
   char     isLocalPlayer,// arg12
   char     param14,      // arg13
   float    lastDistTarget// arg14
);

static fn_VehicleEngineUpdate original_VEUpdate = nullptr;

// ---------------------------------------------------------------------------
// Per-engine EMA state
// ---------------------------------------------------------------------------
struct SmoothState {
   float smoothSpeed;
   float smoothAccel;
   bool  initialized;
};

static std::unordered_map<void*, SmoothState>* s_engineStates = nullptr;

// Time constant for EMA in seconds.  Lower = heavier smoothing.
// 0.05s is fast enough that free-flight feels responsive but damps
// the path-following oscillation which runs at ~5-15 Hz.
static constexpr float EMA_TAU = 0.05f;

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------
static void __fastcall hooked_VEUpdate(
   void*    thisEngine,
   void*    edx,
   void*    engineClass,
   float    dt,
   void*    position,
   void*    velocity,
   float    speedRatio,
   float    acceleration,
   float    turbRatio,
   float    turbulence,
   float    unused1,
   float    unused2,
   float    trickEngine,
   char     isLocalPlayer,
   char     param14,
   float    lastDistTarget)
{
   if (s_engineStates && dt > 0.0f) {
      SmoothState& st = (*s_engineStates)[thisEngine];
      if (!st.initialized) {
         st.smoothSpeed = speedRatio;
         st.smoothAccel = acceleration;
         st.initialized = true;
      } else {
         float alpha = 1.0f - expf(-dt / EMA_TAU);
         st.smoothSpeed += alpha * (speedRatio   - st.smoothSpeed);
         st.smoothAccel += alpha * (acceleration - st.smoothAccel);
      }
      speedRatio   = st.smoothSpeed;
      acceleration = st.smoothAccel;
   }

   original_VEUpdate(
      thisEngine, edx, engineClass, dt, position, velocity,
      speedRatio, acceleration, turbRatio, turbulence,
      unused1, unused2, trickEngine, isLocalPlayer, param14,
      lastDistTarget);
}

// ---------------------------------------------------------------------------
// Init / install / uninstall
// ---------------------------------------------------------------------------
void identify_flyer_sound(uintptr_t exe_base)
{
   cfile log("BF2GameExt.log", "a");

   auto check_id = [&](const flyer_sound_addrs& a) -> bool {
      uint64_t val = *(uint64_t*)(exe_base + a.id_rva);
      return val == a.id_expected;
   };

   const flyer_sound_addrs* addrs = nullptr;
   const char* build_name = nullptr;

   if (check_id(MODTOOLS)) {
      addrs = &MODTOOLS;
      build_name = "modtools";
   } else {
      log.printf("[FlyerSound] Build not recognized (Steam/GOG TBD), skipping\n");
      return;
   }

   log.printf("[FlyerSound] Identified %s build\n", build_name);

   original_VEUpdate = (fn_VehicleEngineUpdate)(exe_base + addrs->ve_update_offset);
   log.printf("[FlyerSound] VehicleEngine::Update at %p\n", (void*)original_VEUpdate);
}

void flyer_sound_install()
{
   if (!original_VEUpdate) return;

   s_engineStates = new std::unordered_map<void*, SmoothState>();

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_VEUpdate, hooked_VEUpdate);
   LONG result = DetourTransactionCommit();

   cfile log("BF2GameExt.log", "a");
   log.printf("[FlyerSound] VehicleEngine::Update hook %s (result=%ld)\n",
              result == NO_ERROR ? "installed" : "FAILED", result);
}

void flyer_sound_uninstall()
{
   if (!original_VEUpdate) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_VEUpdate, hooked_VEUpdate);
   DetourTransactionCommit();

   delete s_engineStates;
   s_engineStates = nullptr;
}
