#pragma once

#include <cmath>
#include <cstdlib>
#include <cstring>

// =============================================================================
// Squads, on their own: where members stand relative to their leader, which way
// the squad faces, and how a class's ODF keys and the INI decide each setting.
// No engine reads, so tests/squad_formation_tests.cpp can exercise all of it.
// The reads and calls around it, per build, are in squad_formation.hpp.
//
// Formation space: x to the right of the way the squad faces, z ahead of it, in
// metres, the leader at the origin. Every slot is behind or beside the leader.
// =============================================================================

namespace squad_formation {

enum class Layout {
   Stock,    // the game's own slots
   Auto,     // a wedge where there is room, a column where there is not
   Wedge,
   File,     // two abreast
   Column,   // one behind the other
   Line,     // abreast of the leader
};

// A value as an ODF or the INI writes it: spaces and quotes round it ignored.
inline void trim(const char* text, const char*& begin, const char*& end)
{
   begin = text;
   end = text + std::strlen(text);
   while (begin < end && (*begin == ' ' || *begin == '\t' || *begin == '"')) ++begin;
   while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '"' || end[-1] == '\r' ||
                          end[-1] == '\n')) --end;
}

// "stock", "auto", "wedge", "file", "column" or "line", any case. False for
// anything else.
inline bool try_parse_layout(const char* text, Layout& out)
{
   static const struct { const char* name; Layout layout; } kNames[] = {
      {"stock", Layout::Stock}, {"auto", Layout::Auto},     {"wedge", Layout::Wedge},
      {"file", Layout::File},   {"column", Layout::Column}, {"line", Layout::Line},
   };
   if (!text) return false;
   const char *b, *e;
   trim(text, b, e);
   for (const auto& n : kNames) {
      const size_t len = std::strlen(n.name);
      if ((size_t)(e - b) != len) continue;
      size_t i = 0;
      while (i < len && (b[i] | 0x20) == n.name[i]) ++i;
      if (i == len) {
         out = n.layout;
         return true;
      }
   }
   return false;
}

inline Layout parse_layout(const char* text, Layout fallback)
{
   Layout layout = fallback;
   return try_parse_layout(text, layout) ? layout : fallback;
}

struct Vec2 { float x = 0.0f, z = 0.0f; };
struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };

// Spacing, in metres, between neighbours in the formation.
constexpr float kMinSpacing = 1.5f;
constexpr float kMaxSpacing = 10.0f;
constexpr float kDefaultSpacing = 3.0f;

inline float clamp_spacing(float spacing)
{
   if (!std::isfinite(spacing)) return kDefaultSpacing;
   return spacing < kMinSpacing ? kMinSpacing : spacing > kMaxSpacing ? kMaxSpacing : spacing;
}

// Followers a squad takes.
constexpr int kMaxSquadSize = 8;

// The slot of follower number `slot` (0 first) in formation space. Followers
// pair off right then left, a row at a time; `open` is the game's own test for
// room around the follower (its controller's open radius above 10 m), which
// only Auto reads.
inline Vec2 slot_offset(Layout layout, int slot, float spacing, bool open)
{
   if (slot < 0) slot = 0;
   const float s = clamp_spacing(spacing);
   const float row = (float)(slot / 2 + 1);
   const float side = (slot % 2 == 0) ? 1.0f : -1.0f;
   constexpr float kDiagonal = 0.70710678f;   // 45 degrees back from the leader
   if (layout == Layout::Auto) layout = open ? Layout::Wedge : Layout::Column;
   switch (layout) {
   case Layout::Column: return {0.0f, -(float)(slot + 1) * s};
   case Layout::File:   return {side * 0.5f * s, -row * s};
   case Layout::Line:   return {side * row * s, 0.0f};
   case Layout::Wedge:
   default:             return {side * row * s * kDiagonal, -row * s * kDiagonal};
   }
}

// The way the formation faces. While the leader moves at kMinTravelSpeed or
// more it turns towards the leader's direction of travel, easing in over
// kTurnTime seconds so a strafe or a corner does not swing the squad round at
// once; while the leader stands, it keeps the last one, so turning the camera
// does not move the squad. With none yet, it is the way the leader faces.
constexpr float kMinTravelSpeed = 1.5f;   // m/s, a walk
constexpr float kTurnTime = 0.35f;        // s, to cover ~63% of a turn

struct Heading {
   Vec2 dir{0.0f, 1.0f};
   bool valid = false;
};

inline bool normalise(Vec2& v)
{
   const float len = std::sqrt(v.x * v.x + v.z * v.z);
   if (!(len > 1e-4f) || !std::isfinite(len)) return false;
   v.x /= len;
   v.z /= len;
   return true;
}

inline Heading next_heading(Heading prev, Vec2 velocity, Vec2 facing, float dt)
{
   const float speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
   Vec2 target;
   bool travelling = false;
   if (std::isfinite(speed) && speed >= kMinTravelSpeed) {
      target = velocity;
      travelling = normalise(target);
   }
   if (!prev.valid || !normalise(prev.dir)) {
      Heading h;
      if (travelling) {
         h.dir = target;
         h.valid = true;
      } else if (normalise(facing)) {
         h.dir = facing;
         h.valid = true;
      }
      return h;
   }
   if (!travelling) return prev;
   if (!(dt > 0.0f) || !std::isfinite(dt)) return prev;
   const float k = 1.0f - std::exp(-dt / kTurnTime);
   Vec2 blended{prev.dir.x + (target.x - prev.dir.x) * k, prev.dir.z + (target.z - prev.dir.z) * k};
   // Turning right round: the blend passes through nothing, so step aside first.
   if (!normalise(blended)) blended = {prev.dir.z, -prev.dir.x};
   prev.dir = blended;
   return prev;
}

// Formation space to the world: z along `heading`, x to its right (left-handed,
// y up: right of +z is +x).
inline Vec2 to_world(Vec2 slot, Vec2 heading)
{
   return {slot.x * heading.z + slot.z * heading.x, -slot.x * heading.x + slot.z * heading.z};
}

// ---- What the slot hands the game, and what the turn makes of it -----------
// The follow agent multiplies the slot by kAgentScale (2.2 while the leader
// moves under 4 m/s, 1.7 while the follower jet-hovers) and keeps it as the
// navigator's offset, building a new navigator whenever it changes. A slot of
// ours is the fixed formation-space offset, so it changes only with the slot,
// and carries kTag in y, where every stock slot has 0, so the navigator's turn
// can tell it from a stock one after any of those scales. The tag is far too
// small to move anything that reads it as a height.
constexpr float kAgentScale = 2.0f;
constexpr float kTag = 1e-20f;

inline Vec3 tagged_slot(Vec2 slot)
{
   return {slot.x / kAgentScale, kTag, slot.z / kAgentScale};
}

inline bool is_tagged(float y)
{
   const float a = std::fabs(y);
   return a >= kTag * 0.5f && a <= kTag * 4.0f;
}

// Where a tagged offset puts the follower: the leader's position plus the
// offset turned to the squad's heading, level with the leader.
inline Vec3 slot_point(Vec3 offset, Vec2 heading, const float* position)
{
   const Vec2 w = to_world({offset.x, offset.z}, heading);
   return {position[0] + w.x, position[1], position[2] + w.z};
}

// ---- A class's ODF keys over the INI ----------------------------------------
// Each key a class sets, itself or through ClassParent, decides that setting
// for the class, whatever the INI says; a key it leaves out takes the INI's.
enum class Key { Leader, Size, Formation, Spacing, Follower };

struct ClassKeys {
   bool   hasLeader = false;    int    leader = 0;      // SquadLeader: rank, 0 never leads
   bool   hasSize = false;      int    size = 0;        // SquadSize: followers it leads
   bool   hasFormation = false; Layout formation = Layout::Auto;
   bool   hasSpacing = false;   float  spacing = kDefaultSpacing;
   bool   hasFollower = false;  bool   follower = true; // SquadFollower: 0 never joins
};

constexpr int kMaxRank = 100;

// Reads one key's value into `keys`. False, leaving the key unset, for a value
// that is not a number (or a layout name for SquadFormation).
inline bool set_key(ClassKeys& keys, Key key, const char* value)
{
   if (!value) return false;
   const char *b, *e;
   trim(value, b, e);
   if (b == e) return false;
   char buf[32];
   const size_t n = (size_t)(e - b) < sizeof buf - 1 ? (size_t)(e - b) : sizeof buf - 1;
   std::memcpy(buf, b, n);
   buf[n] = '\0';
   char* end = nullptr;
   switch (key) {
   case Key::Formation:
      if (!try_parse_layout(buf, keys.formation)) return false;
      keys.hasFormation = true;
      return true;
   case Key::Spacing: {
      const float v = std::strtof(buf, &end);
      if (end == buf || *end || !std::isfinite(v)) return false;
      keys.spacing = clamp_spacing(v);
      keys.hasSpacing = true;
      return true;
   }
   default: {
      const long v = std::strtol(buf, &end, 10);
      if (end == buf || *end) return false;
      if (key == Key::Leader) {
         keys.leader = v < 0 ? 0 : v > kMaxRank ? kMaxRank : (int)v;
         keys.hasLeader = true;
      } else if (key == Key::Size) {
         keys.size = v < 0 ? 0 : v > kMaxSquadSize ? kMaxSquadSize : (int)v;
         keys.hasSize = true;
      } else {
         keys.follower = v != 0;
         keys.hasFollower = true;
      }
      return true;
   }
   }
}

// The INI's values, for the keys a class leaves out.
struct Defaults {
   Layout formation  = Layout::Auto;
   float  spacing    = kDefaultSpacing;
   int    size       = 4;
   bool   autoFormUp = true;   // friendlies who tag along join the player's squad
   bool   aiSquads   = true;   // AI lead squads of their own
};

// The leader's class decides the squad's layout, spacing and size.
inline Layout formation_for(const ClassKeys* leader, const Defaults& d)
{
   return leader && leader->hasFormation ? leader->formation : d.formation;
}

inline float spacing_for(const ClassKeys* leader, const Defaults& d)
{
   return clamp_spacing(leader && leader->hasSpacing ? leader->spacing : d.spacing);
}

inline int size_for(const ClassKeys* leader, const Defaults& d)
{
   const int size = leader && leader->hasSize ? leader->size : d.size;
   return size < 0 ? 0 : size > kMaxSquadSize ? kMaxSquadSize : size;
}

// Whether friendlies who tag along join the player's squad in formation: a
// SquadSize on the player's class says so (above 0) whatever the INI says.
inline bool forms_up_for(const ClassKeys* leader, const Defaults& d)
{
   if (leader && leader->hasSize) return leader->size > 0;
   return d.autoFormUp && size_for(leader, d) > 0;
}

// How readily a class leads an AI squad: 0 never. Unset, any class but a hero
// leads at rank 1 while the INI has AI squads on.
inline int leader_rank_for(const ClassKeys* cls, const Defaults& d, bool hero)
{
   if (cls && cls->hasLeader) return cls->leader;
   return d.aiSquads && !hero ? 1 : 0;
}

// Whether a class joins a squad. Unset, any class but a hero does.
inline bool joins_for(const ClassKeys* cls, bool hero)
{
   if (cls && cls->hasFollower) return cls->follower;
   return !hero;
}

// What PlayerController::CheckForAutoFollow's own stop (CMP EAX,2 / JGE) is
// handed for the player's formation followers: the count itself while
// tag-alongs trail loosely, as stock, otherwise 2 once the squad is full and 0
// until then.
inline int capped_count(int count, bool formsUp, int size)
{
   if (!formsUp) return count;
   return count >= size ? 2 : 0;
}

} // namespace squad_formation
