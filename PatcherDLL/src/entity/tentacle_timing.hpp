#pragma once

#include <stdint.h>

// Native DoTentacles timing, shared by the debug and retail hooks. UpdateTimer
// remains in the engine and maintains these same three simulator fields.
namespace tentacle_timing {

using get_turn_ratio_t = float(__cdecl*)();

struct network_state {
   const volatile uint8_t* selector;
   const volatile uint8_t* modeWhenClear;
   const volatile uint8_t* modeWhenSet;
   const volatile uint8_t* frameLock;
   get_turn_ratio_t getTurnRatio;

   bool offline() const
   {
      return (*selector ? *modeWhenSet : *modeWhenClear) == 0 || *frameLock != 0;
   }
};

// Both shipped x86 implementations keep GetTurnRatio's x87 result through the
// multiply/add and round only at the final float store. Keep that behavior even
// when this DLL is compiled with SSE floating-point arithmetic.
inline float extrapolated_time(get_turn_ratio_t getTurnRatio, const float* internalTimer,
                               const float* timerOffset)
{
   float result;
   __asm {
      call getTurnRatio
      mov eax, internalTimer
      fmul dword ptr [eax]
      mov eax, timerOffset
      fadd dword ptr [eax]
      fstp result
   }
   return result;
}

inline float fractional_time(get_turn_ratio_t getTurnRatio, const float* internalTimer)
{
   float result;
   __asm {
      call getTurnRatio
      mov eax, internalTimer
      fmul dword ptr [eax]
      fstp result
   }
   return result;
}

inline float select_elapsed_time(float& internalTimer, float& timeSinceLastUpdate, float& timerOffset,
                                 float suppliedDt, bool hasPose, const network_state& network)
{
   float dt = suppliedDt;
   if (network.offline()) {
      dt = internalTimer;
      if (dt > 0.039f) dt = 0.039f;
   }
   else if (!(dt >= 0.0f)) {
      // The original comparison also takes this branch for unordered input.
      dt = extrapolated_time(network.getTurnRatio, &internalTimer, &timerOffset);
      if (timeSinceLastUpdate < dt) dt -= timeSinceLastUpdate;
      const float fractional = fractional_time(network.getTurnRatio, &internalTimer);
      timerOffset = 0.0f;
      timeSinceLastUpdate = fractional;
   }

   // Timing selection precedes the native null-pose check. A null pose still
   // advances network history above, but never consumes the offline timer.
   // Recheck the mode, as the native function does after GetTurnRatio calls.
   if (hasPose && network.offline()) internalTimer = 0.0f;
   return dt;
}

} // namespace tentacle_timing
