// Standalone tests (not a DLL build) for when an AI soldier sprints: the
// decision in ai_sprint_core.hpp, fed what ai_sprint.cpp reads from the game.
#include "../PatcherDLL/src/ai/ai_sprint_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>

using namespace ai_sprint;

namespace {

constexpr float kStart = 30.0f;

// A unit standing and running to somewhere 50 m off, with a full bar.
Inputs running()
{
   Inputs in;
   in.height = kHeightStand;
   in.soldierState = kStateStand;
   in.travelling = true;
   in.distance = 50.0f;
   in.energy = 120.0f;
   in.energyMax = 120.0f;
   return in;
}

void starts_on_a_long_run()
{
   assert(should_sprint(running(), kStart));
}

void only_instead_of_standing()
{
   for (int height : {1, 2, kHeightSprint, 4}) {   // crouch, prone, its own sprint, walk
      Inputs in = running();
      in.height = height;
      assert(!should_sprint(in, kStart));
   }
   // Sprint() itself only works for a soldier standing or sprinting.
   for (int state : {1, 2, 4, 5, 6, 7, 9, 14, 17, 18}) {
      Inputs in = running();
      in.soldierState = state;
      assert(!should_sprint(in, kStart));
   }
}

void never_in_combat()
{
   Inputs in = running();
   in.inCombat = true;
   assert(!should_sprint(in, kStart));
   in = running();
   in.hasTarget = true;
   assert(!should_sprint(in, kStart));
   in = running();
   in.travelling = false;   // following a leader, sliding or standing about
   assert(!should_sprint(in, kStart));
}

void distance_with_a_margin()
{
   Inputs in = running();
   in.distance = kStart - 0.1f;
   assert(!should_sprint(in, kStart));          // too close to start
   in.distance = kStart;
   assert(should_sprint(in, kStart));
   in.soldierState = kStateSprint;              // already going: keeps on to half
   in.distance = kStart * kStopShare + 0.1f;
   assert(should_sprint(in, kStart));
   in.distance = kStart * kStopShare - 0.1f;
   assert(!should_sprint(in, kStart));
}

void energy_like_a_player()
{
   Inputs in = running();
   in.energy = 0.5f * in.energyMax - 1.0f;      // not enough to start a run
   assert(!should_sprint(in, kStart));
   in.soldierState = kStateSprint;              // but enough to carry on
   assert(should_sprint(in, kStart));
   in.energy = 1.0f;
   assert(should_sprint(in, kStart));
   in.overheated = true;                        // the latch tripped at empty: stop
   assert(!should_sprint(in, kStart));
   in.soldierState = kStateStand;
   in.energy = in.energyMax;
   assert(!should_sprint(in, kStart));          // and no restart until it clears
   in.overheated = false;
   assert(should_sprint(in, kStart));
}

void followers_travel_nowhere_of_their_own()
{
   // A follower's navigator says yes to its own class and to the graph follower.
   assert(route_of(true, true, false) == Route::None);
   assert(route_of(false, true, false) == Route::Graph);
   assert(route_of(false, false, true) == Route::Path);
   assert(route_of(false, false, false) == Route::None);   // sliding or standing
}

void bad_input_never_sprints()
{
   const float nan = std::numeric_limits<float>::quiet_NaN();
   const float inf = std::numeric_limits<float>::infinity();
   Inputs in = running();
   in.distance = nan;
   assert(!should_sprint(in, kStart));
   in = running();
   in.distance = inf;
   assert(!should_sprint(in, kStart));
   in = running();
   in.energy = nan;
   assert(!should_sprint(in, kStart));
   in = running();
   in.energyMax = 0.0f;
   assert(!should_sprint(in, kStart));
   for (float start : {0.0f, -5.0f, nan}) assert(!should_sprint(running(), start));
}

} // namespace

int main()
{
   starts_on_a_long_run();
   only_instead_of_standing();
   never_in_combat();
   distance_with_a_margin();
   energy_like_a_player();
   followers_travel_nowhere_of_their_own();
   bad_input_never_sprints();
   std::printf("AI sprint tests passed (long runs, standing only, combat, distance margin, energy latch, followers, bad input).\n");
   return 0;
}
