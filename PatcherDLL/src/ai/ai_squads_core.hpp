#pragma once

#include <algorithm>
#include <cmath>

// =============================================================================
// Who leads and who joins an AI squad, on its own: one tick's plan from what
// ai_squads.cpp reads of every AI soldier, with no engine reads, so
// tests/ai_squads_tests.cpp can exercise it.
// =============================================================================

namespace ai_squads {

constexpr int kMaxUnits = 256;

struct Unit {
   int    team      = 0;
   float  x         = 0.0f;   // on the ground: x and z
   float  z         = 0.0f;
   int    rank      = 0;      // leads at this rank; 0 never
   bool   joins     = true;   // may follow a leader
   int    size      = 0;      // followers it takes when it leads
   double since     = 0.0;    // alive since; the longer alive leads first at a rank
   bool   free      = true;   // following nobody
   bool   combat    = false;  // its controller is fighting
   int    followers = 0;      // in its squad already
};

struct Recruit {
   int follower;
   int leader;
};

// A recruit's follow order waits in its controller's queue until the
// controller's next decision, so for a while after joining it still answers
// that it follows nobody. It counts as a member, taken, for kJoinGrace seconds
// from joining; after that only while it answers that it follows its leader.
constexpr double kJoinGrace = 6.0;

inline bool keep_member(bool follows, double joinedAt, double now)
{
   return follows || (now - joinedAt) < kJoinGrace;
}

// The order leaders are served in: squads that already have followers first,
// then the highest rank, the longest alive, the lowest index.
inline bool serves_before(const Unit* units, int a, int b)
{
   const Unit& ua = units[a];
   const Unit& ub = units[b];
   const bool la = ua.followers > 0, lb = ub.followers > 0;
   if (la != lb) return la;
   if (ua.rank != ub.rank) return ua.rank > ub.rank;
   if (ua.since != ub.since) return ua.since < ub.since;
   return a < b;
}

// This tick's recruits, into `out` (up to `maxOut`); returns how many. A
// leader keeps its squad and fills it up to its size; a free unit with a rank
// starts one only out of combat, and only when someone will join it. A unit
// joins only a leader of its own team, within `range` metres, ranked at least
// as high as itself, nearest first; it is free, out of combat, and leads no
// squad of its own.
inline int plan(const Unit* units, int n, float range, Recruit* out, int maxOut)
{
   if (n > kMaxUnits) n = kMaxUnits;
   if (n <= 0 || !(range > 0.0f) || !std::isfinite(range)) return 0;
   const float range2 = range * range;

   int order[kMaxUnits];
   bool claimed[kMaxUnits];
   for (int i = 0; i < n; ++i) {
      order[i] = i;
      claimed[i] = !units[i].free || units[i].followers > 0;
   }
   std::sort(order, order + n, [&](int a, int b) { return serves_before(units, a, b); });

   int made = 0;
   int picks[kMaxUnits];
   for (int o = 0; o < n && made < maxOut; ++o) {
      const int l = order[o];
      const Unit& leader = units[l];
      const bool leads = leader.followers > 0;
      if (!leads && claimed[l]) continue;            // following someone, or recruited this tick
      if (!leader.free || leader.rank <= 0) continue;
      if (!leads && leader.combat) continue;
      const int want = leader.size - leader.followers;
      if (want <= 0) continue;

      int count = 0;
      for (int j = 0; j < n; ++j) {
         if (j == l || claimed[j]) continue;
         const Unit& u = units[j];
         if (u.team != leader.team || !u.joins || u.combat || u.rank > leader.rank) continue;
         const float dx = u.x - leader.x, dz = u.z - leader.z;
         const float d2 = dx * dx + dz * dz;
         if (!(d2 <= range2)) continue;
         picks[count++] = j;
      }
      if (count == 0) continue;
      std::sort(picks, picks + count, [&](int a, int b) {
         const float ax = units[a].x - leader.x, az = units[a].z - leader.z;
         const float bx = units[b].x - leader.x, bz = units[b].z - leader.z;
         const float da = ax * ax + az * az, db = bx * bx + bz * bz;
         return da != db ? da < db : a < b;
      });
      claimed[l] = true;
      for (int k = 0; k < count && k < want && made < maxOut; ++k) {
         claimed[picks[k]] = true;
         out[made++] = {picks[k], l};
      }
   }
   return made;
}

} // namespace ai_squads
