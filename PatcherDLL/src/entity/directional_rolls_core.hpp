#pragma once

#include <cmath>
#include <cstdio>
#include <stdint.h>

// The parts of the directional rolls that need no game: which way a roll
// goes, the names its dive is looked up by, and the parent chains the lookup
// walks. Tested by tests/directional_rolls_tests.cpp; the rest is in
// directional_rolls.cpp, and the mechanism in directional_rolls.hpp.

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

// How the name was found, which sets the SoldierAnimation's scope as
// AnimationFinder::AssignAnimation does: the half's own suffix, the plain
// name (the action's own scope), or "_full".
enum class Match : uint8_t { Half, Plain, Full };
constexpr int kNameMax = 128;
constexpr int kNamesPerHalf = 3;

// The names AnimationFinder::AssignAnimation tries for one half of a
// full-body action, in its order: "<bank>_<weapon>_<anim>_upper" (or _lower),
// then "<bank>_<weapon>_<anim>", then "<bank>_<weapon>_<anim>_full". Half 0 is
// the upper body, 1 the lower.
inline void half_names(char out[kNamesPerHalf][kNameMax], const char* bank, const char* weapon,
                       const char* anim, int half)
{
   std::snprintf(out[0], kNameMax, "%s_%s_%s_%s", bank, weapon, anim, half == 0 ? "upper" : "lower");
   std::snprintf(out[1], kNameMax, "%s_%s_%s", bank, weapon, anim);
   std::snprintf(out[2], kNameMax, "%s_%s_%s_full", bank, weapon, anim);
}

constexpr Match kMatchOf[kNamesPerHalf] = { Match::Half, Match::Plain, Match::Full };

// A parent chain from `start`: start, its parent, and so on, ending at a root
// (its own parent), a parent outside [0, count) or kMaxChain entries.
// `parent(i)` reads entry i's parent. Returns the length written to `out`.
constexpr int kMaxChain = 8;

template <class ParentOf>
int parent_chain(int start, int count, ParentOf parent, int out[kMaxChain])
{
   int n = 0;
   int at = start;
   while (n < kMaxChain && at >= 0 && at < count) {
      for (int i = 0; i < n; ++i)
         if (out[i] == at) return n;   // a loop
      out[n++] = at;
      const int up = parent(at);
      if (up == at) break;
      at = up;
   }
   return n;
}

} // namespace directional_rolls
