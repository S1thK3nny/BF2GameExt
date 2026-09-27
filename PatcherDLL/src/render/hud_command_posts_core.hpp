#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

// The engine-independent half of the command post strip, tested by
// tests/hud_command_posts_tests.cpp: which post fills which slot, and how much
// of a post its side holds. Nothing here touches the game.
namespace hud_command_posts {

constexpr int kSlots = 16;

// A post's place in the strip: numbered posts (HUDIndex set) first, by number,
// then the rest in map order.
struct Entry {
   int hudIndex;  // 0 when HUDIndex is unset
   int mapIndex;  // position in CommandPost::sPostArray
};

inline bool before(const Entry& a, const Entry& b)
{
   const bool an = a.hudIndex > 0, bn = b.hudIndex > 0;
   if (an != bn) return an;
   if (an && a.hudIndex != b.hudIndex) return a.hudIndex < b.hudIndex;
   return a.mapIndex < b.mapIndex;
}

// A few dozen posts at most: insertion sort, stable.
template <typename T>
void order(T* items, int count, const Entry T::*key)
{
   for (int i = 1; i < count; ++i) {
      T item = items[i];
      int j = i - 1;
      while (j >= 0 && before(item.*key, items[j].*key)) {
         items[j + 1] = items[j];
         --j;
      }
      items[j + 1] = item;
   }
}

// One post's capture state as the engine holds it.
struct Capture {
   int   team;            // owner, 0 when neutral
   int   biasTeam;        // the team the capture timer is counting for
   float neutralize, neutralizeTime;
   float capture, captureTime;
   bool  timersValid;     // false on a client far from the post: it does not simulate them
};

inline float ratio(float timer, float total)
{
   if (!(total > 0) || !std::isfinite(timer) || !std::isfinite(total)) return 0;
   const float r = timer / total;
   return r < 0 ? 0 : r > 1 ? 1 : r;
}

// How much of the post its side holds, 0..1: an owned post drains as it is
// neutralised, a neutral post fills as it is captured.
inline float control(const Capture& c)
{
   if (!c.timersValid) return c.team != 0 ? 1.0f : 0.0f;
   return c.team != 0 ? 1.0f - ratio(c.neutralize, c.neutralizeTime)
                      : ratio(c.capture, c.captureTime);
}

// The team whose colour that fill shows: the owner, else the capturing team
// once its capture has started, else neutral.
inline int control_team(const Capture& c)
{
   if (c.team != 0) return c.team;
   if (c.timersValid && c.biasTeam > 0 && ratio(c.capture, c.captureTime) > 0) return c.biasTeam;
   return 0;
}

// A post's team is a 4-bit signed field; anything outside the colour table is
// treated as neutral.
inline int valid_team(int team, int teamCount) { return team >= 0 && team < teamCount ? team : 0; }

// Change detection: true when v must be sent.
struct Sent {
   uint32_t value = 0;
   bool     valid = false;

   void invalidate() { valid = false; }

   bool set(uint32_t v)
   {
      if (valid && value == v) return false;
      value = v;
      valid = true;
      return true;
   }

   bool set_float(float v)
   {
      uint32_t bits;
      std::memcpy(&bits, &v, sizeof(bits));
      return set(bits);
   }
};

} // namespace hud_command_posts
