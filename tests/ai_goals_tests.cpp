// Standalone tests (not a DLL build) for the AI goals' limit and which goal a
// unit joins: the logic in ai_goals_core.hpp, fed what ai_goals.cpp reads.
#include "../PatcherDLL/src/ai/ai_goals_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace ai_goals;

namespace {

GoalView goal(int team, float weight, int members, bool enabled = true, bool reserved = false)
{
   GoalView g;
   g.present = true;
   g.team = team;
   g.enabled = enabled;
   g.weight = weight;
   g.members = members;
   g.reserved = reserved;
   return g;
}

// The INI's number, and the debug listing's reach.
void limits()
{
   assert(clamp_limit(0) == kStockGoals && clamp_limit(-5) == kStockGoals && clamp_limit(20) == kStockGoals);
   assert(clamp_limit(21) == 21 && clamp_limit(64) == 64 && clamp_limit(65) == kMaxGoals && clamp_limit(500) == kMaxGoals);
   assert(listing_end(20) == 20 && listing_end(21) == 25 && listing_end(60) == 60 && listing_end(64) == 65);
   // The listing reads five slots a turn from 0 while below the limit: never past
   // the manager's last slot, and never short of the limit.
   for (int limit = kStockGoals; limit <= kMaxGoals; ++limit) {
      int last = -1;
      for (int i = 0; i < limit; i += 5) last = i + 4;
      assert(last < kGoalSlots && last >= limit - 1);
      assert(listing_end(limit) % 5 == 0 && listing_end(limit) >= limit && listing_end(limit) < limit + 5);
   }
}

// The game's rule: the team's enabled goals, least full for their weight share.
void picking()
{
   GoalView g[kGoalSlots];
   assert(pick_goal(g, kGoalSlots, 1, 10) == -1);   // none at all
   assert(pick_goal(nullptr, 5, 1, 10) == -1);
   g[0] = goal(1, 1.0f, 2);
   g[1] = goal(1, 1.0f, 0);
   assert(pick_goal(g, 2, 1, 10) == 1);             // the emptier
   g[1] = goal(1, 1.0f, 2);
   assert(pick_goal(g, 2, 1, 10) == 0);             // a tie: the first
   // By the share of the weight: 2 / (0.75 x 8) against 1 / (0.25 x 8).
   g[0] = goal(1, 3.0f, 2);
   g[1] = goal(1, 1.0f, 1);
   assert(pick_goal(g, 2, 1, 8) == 0);
   g[0].members = 5;                                // 5 / 6 against 0.5
   assert(pick_goal(g, 2, 1, 8) == 1);
   // Other teams', disabled and absent goals are passed over.
   g[0] = goal(2, 1.0f, 0);
   g[1] = goal(1, 1.0f, 0, false);
   g[2] = goal(1, 1.0f, 7);
   assert(pick_goal(g, 3, 1, 10) == 2);
   g[2].present = false;
   assert(pick_goal(g, 3, 1, 10) == -1);
   // Weights adding up to nothing: none, even where they cancel out.
   g[0] = goal(1, 0.0f, 0);
   g[1] = goal(1, 0.0f, 0);
   assert(pick_goal(g, 2, 1, 10) == -1);
   g[0] = goal(1, 1.0f, 0);
   g[1] = goal(1, -1.0f, 0);
   assert(pick_goal(g, 2, 1, 10) == -1);
   // A total below 0 counts as 1, as the game has it: shares keep their signs,
   // 2 / (-1 x 10) against 1 / (-3 x 10).
   g[0] = goal(1, -1.0f, 0);
   g[1] = GoalView{};
   assert(pick_goal(g, 2, 1, 10) == 0);
   g[0] = goal(1, -1.0f, 2);
   g[1] = goal(1, -3.0f, 1);
   assert(pick_goal(g, 2, 1, 10) == 0);
   // A share is the weight over the total: a lone goal however light is all of it.
   g[0] = goal(1, 1e-12f, 1);
   g[1] = GoalView{};
   assert(pick_goal(g, 2, 1, 1) == 0);
   // A disabled goal's weight is no part of the total either: with it, the
   // enabled one's share would be ~1e-12 and its 1 member past the game's limit.
   g[0] = goal(1, 1.0f, 1);
   g[1] = goal(1, 1e12f, 0, false);
   assert(pick_goal(g, 2, 1, 1) == 0);
   // The game's starting "least full" (modtools 0x00A4A3C0, which
   // tests/ai_goals_abi_tests.py reads from the exe).
   assert(kNoRatio == 1e11f);
   // No one on the team: no share to fill.
   g[0] = goal(1, 1.0f, 1);
   assert(pick_goal(g, 1, 1, 0) == -1);
   // Past the stock 20.
   GoalView many[kGoalSlots];
   for (int i = 0; i < kMaxGoals; ++i) many[i] = goal(1, 1.0f, 3);
   many[50].members = 0;
   assert(pick_goal(many, kMaxGoals, 1, 64) == 50);
   assert(pick_goal(many, kStockGoals, 1, 64) == 0);   // the stock limit sees the first 20 only
}

// A goal kept for a script's own units takes no one by weight, and its weight is
// left out of the others' shares.
void kept_goals()
{
   GoalView g[3];
   g[0] = goal(1, 1.0f, 0, true, true);   // empty, but kept
   g[1] = goal(1, 1.0f, 4);
   assert(pick_goal(g, 2, 1, 10) == 1);
   g[1].reserved = true;
   assert(pick_goal(g, 2, 1, 10) == -1);   // all kept: none
   // Shares from the unkept only: 3 / (0.5 x 10) against 1 / (0.5 x 10), the
   // kept one's 100 left out (with it, both shares would be 1/102 and the ratios
   // past 100).
   g[0] = goal(1, 100.0f, 0, true, true);
   g[1] = goal(1, 1.0f, 3);
   g[2] = goal(1, 1.0f, 1);
   assert(pick_goal(g, 3, 1, 10) == 2);
   g[1].members = 0;
   assert(pick_goal(g, 3, 1, 10) == 1);
   // Where it shows: a kept goal's huge weight would leave the other a share of
   // ~1e-12, its 1 member past the game's limit, and no one placed.
   g[0] = goal(1, 1e12f, 0, true, true);
   g[1] = goal(1, 1.0f, 1);
   assert(pick_goal(g, 2, 1, 1) == 1);
}

} // namespace

int main()
{
   limits();
   picking();
   kept_goals();
   std::printf("AI goals tests passed (limits, picking, kept goals).\n");
   return 0;
}
