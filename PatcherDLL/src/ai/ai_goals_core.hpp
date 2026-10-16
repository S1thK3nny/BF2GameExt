#pragma once

// =============================================================================
// AI goals on their own: how many there may be, and which goal a unit with
// none joins. No engine reads, so tests/ai_goals_tests.cpp can exercise it; the
// reads and patches are in ai_goals.hpp.
// =============================================================================

namespace ai_goals {

constexpr int kStockGoals = 20;   // AIGoalManager::mGoals[20], and AIGoal::sMemoryPool's 20
constexpr int kMaxGoals   = 64;

// The manager's array: one slot more than the most goals, always empty. Its
// debug listing reads the goals five at a time (the loop unrolled), so it reads
// up to four slots past its last; with the limit no higher than 64, slot 64 at
// most.
constexpr int kGoalSlots = kMaxGoals + 1;

// [LimitIncreases] AIGoalLimit: 20 or less (0 included) is the game's own;
// above that, up to 64.
inline int clamp_limit(int n) { return n <= kStockGoals ? kStockGoals : n > kMaxGoals ? kMaxGoals : n; }

// Where the debug listing's unrolled loop ends when it finds no goal of a team:
// the first multiple of five at or past the limit (its "none" test).
inline int listing_end(int limit) { return (limit + 4) / 5 * 5; }

// A goal as AssignUnit reads it.
struct GoalView {
   bool  present = false;
   int   team = -1;
   bool  enabled = false;
   float weight = 0.0f;
   int   members = 0;
   bool  reserved = false;   // a mission script's: units go in it by name only
};

// The game's starting "least full" (modtools 0x00A4A3C0): a goal must be under
// it to be taken.
constexpr float kNoRatio = 1e11f;

// The goal a unit of `team` with none joins: the game's rule
// (AIGoalManager::AssignUnit) over `n` goals. Of the team's enabled goals, the
// one least full for its share of the weight, members / (weight / total weight
// x team size), the first on a tie; none if the weights add up to 0 (a total
// below 0 counts as 1, as the game has it). A goal a script keeps for units of
// its own is passed over, its weight left out of the total. -1 for none.
inline int pick_goal(const GoalView* goals, int n, int team, int teamSize)
{
   float total = 0.0f;
   int counted = 0;
   for (int i = 0; goals && i < n; ++i) {
      const GoalView& g = goals[i];
      if (!g.present || g.team != team || !g.enabled || g.reserved) continue;
      total += g.weight;
      ++counted;
   }
   if (!counted || total == 0.0f) return -1;
   if (total <= 0.0f) total = 1.0f;
   int best = -1;
   float least = kNoRatio;
   for (int i = 0; i < n; ++i) {
      const GoalView& g = goals[i];
      if (!g.present || g.team != team || !g.enabled || g.reserved) continue;
      const float ratio = (float)g.members / ((g.weight / total) * (float)teamSize);
      if (ratio < least) {
         best = i;
         least = ratio;
      }
   }
   return best;
}

} // namespace ai_goals
