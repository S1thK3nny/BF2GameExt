#pragma once

#include <cmath>

// =============================================================================
// When an AI soldier sprints to where it is going - the decision on its own,
// with no engine reads, so tests/ai_sprint_tests.cpp can exercise it. The reads
// that fill Inputs, and why each one means what it says here, are in
// ai_sprint.hpp.
// =============================================================================

namespace ai_sprint {

// ControllableHeight (AILowLevel::mHeight) and SoldierState values, the same on
// every build.
constexpr int kHeightStand  = 0;
constexpr int kHeightSprint = 3;
constexpr int kStateStand   = 0;
constexpr int kStateSprint  = 3;

// A sprint starts with at least this share of the energy bar, so the AI gets a
// run out of it rather than a stutter; once going it lasts until the bar's
// overheat latch trips, as a player's does.
constexpr float kStartEnergyShare = 0.5f;

// A sprint stops this much short of where it started from the destination, so
// a unit near the line does not flick in and out of it.
constexpr float kStopShare = 0.5f;

struct Inputs {
   int   height       = kHeightStand;  // what the AI asked for this update
   int   soldierState = kStateStand;   // what the soldier is doing
   bool  inCombat     = false;         // the AI controller's state
   bool  hasTarget    = false;         // the AI has someone to shoot at
   bool  travelling   = false;         // on its way somewhere along a path
   float distance     = 0.0f;          // metres left to there, on the ground
   float energy       = 0.0f;
   float energyMax    = 0.0f;
   bool  overheated   = false;         // the energy bar's latch
};

// Which destination a navigator travels to, from what its IsRtti says yes to.
// A follower's navigator derives from the graph follower and says yes to it
// too, but it walks a leader's trail and the game sprints it itself, so it
// travels nowhere of its own here.
enum class Route { None, Graph, Path };

inline Route route_of(bool followTarget, bool graphFollower, bool pathFollower)
{
   if (followTarget) return Route::None;
   if (graphFollower) return Route::Graph;
   if (pathFollower) return Route::Path;
   return Route::None;
}

// Sprint on a long run out of combat: only where the AI would simply stand and
// run, never instead of a crouch, prone, walk or a sprint of its own, and only
// a soldier standing or already sprinting can (EntitySoldier::Sprint's own test).
inline bool should_sprint(const Inputs& in, float startDistance)
{
   if (in.height != kHeightStand) return false;
   if (in.soldierState != kStateStand && in.soldierState != kStateSprint) return false;
   if (in.inCombat || in.hasTarget || !in.travelling || in.overheated) return false;
   if (!std::isfinite(in.distance) || !std::isfinite(in.energy) || !(in.energyMax > 0.0f)) return false;
   if (!(startDistance > 0.0f)) return false;
   const bool sprinting = in.soldierState == kStateSprint;
   if (in.distance < (sprinting ? startDistance * kStopShare : startDistance)) return false;
   return sprinting || in.energy >= kStartEnergyShare * in.energyMax;
}

} // namespace ai_sprint
