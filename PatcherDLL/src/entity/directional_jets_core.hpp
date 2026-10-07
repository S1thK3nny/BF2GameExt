#pragma once

#include <cmath>
#include <stdint.h>

// The parts of the directional jets that need no game: the four animations,
// the unit's top jet speeds, how far the move leans the legs toward each way
// and how much of each animation that shows. Tested by
// tests/directional_jets_tests.cpp; the rest is in directional_jets.cpp and
// the mechanism in directional_jets.hpp.

namespace directional_jets {

enum Dir : int { Forward = 0, Backward = 1, Left = 2, Right = 3, kDirs = 4 };

// Each way's animation, beside the stock "jetpack_hover", named for the way
// the soldier moves, as jump_left is: jetpack_hover_left is the pose while
// moving left, legs trailing to the right.
inline const char* anim_name(int dir)
{
   switch (dir) {
   case Forward:  return "jetpack_hover_forward";
   case Backward: return "jetpack_hover_backward";
   case Left:     return "jetpack_hover_left";
   default:       return "jetpack_hover_right";
   }
}

// Below this a speed (m/s) counts as none.
constexpr float kMinSpeed = 0.1f;

// A unit's top jet speed each way. EntitySoldier::MoveJetHover pushes a
// hovering soldier toward the stick times its class's speeds: forward
// MaxSpeed x ControlSpeed jet thrust, backward MaxStrafeSpeed x jet thrust,
// sideways MaxStrafeSpeed x jet strafe. A way whose jet ControlSpeed leaves
// no push there still moves off a jet jump, which pushes by JetAcceleration,
// so its run speed stands in.
struct TopSpeeds {
   float forward = 0.0f, backward = 0.0f, side = 0.0f;
};

inline TopSpeeds top_speeds(float maxSpeed, float maxStrafeSpeed, float jetThrust, float jetStrafe)
{
   TopSpeeds t;
   t.forward  = maxSpeed * jetThrust;
   t.backward = maxStrafeSpeed * jetThrust;
   t.side     = maxStrafeSpeed * jetStrafe;
   if (!(t.forward >= kMinSpeed))  t.forward = maxSpeed;
   if (!(t.backward >= kMinSpeed)) t.backward = maxStrafeSpeed;
   if (!(t.side >= kMinSpeed))     t.side = maxStrafeSpeed;
   return t;
}

// How far the legs lean, a point in the unit disc: f toward forward (+) or
// backward (-), s toward left (+) or right (-).
struct Lean {
   float f = 0.0f, s = 0.0f;
};

inline float over(float speed, float top)
{
   return top >= kMinSpeed ? speed / top : 0.0f;
}

// The lean a move asks for, from the body's speed along its forward row (f)
// and its right row (s, which points to the body's left, so + is moving
// left): each over the unit's top speed that way, kept inside the unit disc.
// Full at the top speed, and in between by speed.
inline Lean lean_of(float f, float s, const TopSpeeds& top)
{
   if (!std::isfinite(f) || !std::isfinite(s)) return {};
   Lean l;
   l.f = f >= 0.0f ? over(f, top.forward) : over(f, top.backward);
   l.s = over(s, top.side);
   const float m = std::sqrt(l.f * l.f + l.s * l.s);
   if (!std::isfinite(m)) return {};
   if (m > 1.0f) {
      l.f /= m;
      l.s /= m;
   }
   return l;
}

// The lean the legs show moves toward the move's at the rate
// SoldierAnimator::UpdateActionAnimation turns the legs toward theirs
// (min(dt x 7.5, 1) of the way each frame), so a quick turn or a change of
// direction swings the legs over rather than snapping them.
constexpr float kFollowRate = 7.5f;

inline Lean follow(const Lean& shown, const Lean& target, float dt)
{
   float k = dt * kFollowRate;
   if (!(k > 0.0f)) return shown;   // paused, or NaN
   if (k > 1.0f) k = 1.0f;
   Lean l;
   l.f = shown.f + (target.f - shown.f) * k;
   l.s = shown.s + (target.s - shown.s) * k;
   return l;
}

// How much of each way's animation the legs show; the rest is the hover. The
// lean's length is the total, shared between its forward-or-backward way and
// its side in proportion to the two parts: straight ahead at the top speed
// is all jetpack_hover_forward, diagonally half each of two ways.
struct Weights {
   float w[kDirs] = {};
};

constexpr float kMinWeight = 0.002f;   // less shows nothing

inline Weights weights_of(const Lean& lean)
{
   Weights out;
   const float af = std::fabs(lean.f), as = std::fabs(lean.s);
   float m = std::sqrt(af * af + as * as);
   if (!(m > kMinWeight)) return out;   // also NaN
   if (m > 1.0f) m = 1.0f;
   const float share = af / (af + as);
   out.w[lean.f >= 0.0f ? Forward : Backward] = m * share;
   out.w[lean.s > 0.0f ? Left : Right] = m * (1.0f - share);
   return out;
}

// The blends that lay those weights on the hover one after another: each
// step moves the pose `t` of the way to its animation, t being its weight
// over all the weight laid so far, the hover's included, so each animation
// ends with its own share. A way without an animation is left out and its
// share stays the hover's. Returns the number of steps.
struct Step {
   int   dir;
   float t;
};

inline int blend_steps(const Weights& w, const bool have[kDirs], Step out[kDirs])
{
   float laid = 1.0f;   // the hover's weight, then the running total
   for (int d = 0; d < kDirs; ++d)
      if (have[d] && w.w[d] > kMinWeight) laid -= w.w[d];
   if (laid < 0.0f) laid = 0.0f;
   int n = 0;
   for (int d = 0; d < kDirs; ++d) {
      if (!have[d] || !(w.w[d] > kMinWeight)) continue;
      laid += w.w[d];
      out[n].dir = d;
      out[n].t = laid > 0.0f ? w.w[d] / laid : 1.0f;
      if (out[n].t > 1.0f) out[n].t = 1.0f;
      ++n;
   }
   return n;
}

} // namespace directional_jets
