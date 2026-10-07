#pragma once

#include <cmath>
#include <stdint.h>

// The parts of the directional rolls that need no game: which way a roll
// goes, the dive it plays and where that dive's root aims. Tested by
// tests/directional_rolls_tests.cpp; the rest is in directional_rolls.cpp,
// the mechanism in directional_rolls.hpp, and the names and parent chains
// the dives are looked up through in soldier_anim_tables_core.hpp.

namespace directional_rolls {

enum class Side : uint8_t { Forward = 0, Left = 1, Right = 2 };

// The dive each side plays, beside the stock "diveforward".
inline const char* dive_name(Side side)
{
   return side == Side::Left ? "diveleft" : side == Side::Right ? "diveright" : "diveforward";
}

// Below this speed squared (m/s) a roll keeps diveforward: SetAction's gate
// for directional jumps, 2 m/s.
constexpr float kMinSpeedSq = 4.0f;

inline float dot3(const float* a, const float* b)
{
   return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The way a roll goes, from the body's move this frame and the body's right
// and forward rows, by SoldierAnimator::SetAction's rule for directional jumps:
// forward when slower than 2 m/s or when forward (or backward, which has no
// dive) is the larger part, else to the side. The body's "right" row points to
// its left: SetAction plays jump_left when the move along it is positive.
inline Side choose_side(const float movement[3], const float right[3], const float forward[3], float dt)
{
   if (!(dt > 0.0f)) return Side::Forward;
   const float f = dot3(movement, forward) / dt;
   const float s = dot3(movement, right) / dt;
   if (!(f * f + s * s >= kMinSpeedSq)) return Side::Forward;   // also NaN
   if (f > std::fabs(s) || -f > std::fabs(s)) return Side::Forward;
   return s > 0.0f ? Side::Left : Side::Right;
}

// The move to hand SoldierAnimator::SetupPose while a side dive plays.
// SetupPose aims a diving body's root at atan2(move . right, move . forward),
// the move's angle from the body, which keeps a forward dive on the soldier's
// real move as the body turns with the aim. A side dive moves a quarter turn
// from where its root faces (+90 degrees for the left, positive along the
// right row; -90 for the right), so it gets the move turned that quarter back
// in the body's plane: its root then aims a quarter turn from the real move,
// and the dive follows the move the same way. A move straight to the side
// comes out along the forward row: the root faces the aim. Forward gives the
// move unchanged.
inline void side_dive_aim(const float movement[3], const float right[3], const float forward[3], Side side,
                          float out[3])
{
   const float f = dot3(movement, forward);
   const float s = dot3(movement, right);
   if (side == Side::Forward) {
      for (int i = 0; i < 3; ++i) out[i] = movement[i];
      return;
   }
   const float f2 = side == Side::Left ? s : -s;
   const float s2 = side == Side::Left ? -f : f;
   for (int i = 0; i < 3; ++i) out[i] = forward[i] * f2 + right[i] * s2;
}

} // namespace directional_rolls
