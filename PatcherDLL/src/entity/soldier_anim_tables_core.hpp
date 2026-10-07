#pragma once

#include <cstdio>
#include <stdint.h>

// The parts of soldier_anim_tables' lookup that need no game: the names
// AnimationFinder tries for one half of an action animation, and the parent
// chains it walks. Tested by tests/soldier_anim_tables_tests.cpp; the lookup
// itself is soldier_anim_tables::find_named.

namespace soldier_anim_tables {

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

} // namespace soldier_anim_tables
