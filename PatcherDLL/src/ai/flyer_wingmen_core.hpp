#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>

// =============================================================================
// Wingmen for a player's flyer, on their own: where each wingman's slot is, the
// point it is steered at, the speed it is asked for, and who may fly as one. No
// engine reads, so tests/flyer_wingmen_tests.cpp can exercise all of it. The
// reads and calls around it are in flyer_wingmen.hpp.
//
// Formation space: x to the leader's right, y up, z ahead, in metres, the
// leader at the origin and its roll left out, so a banking leader does not tip
// its wingmen into the ground.
// =============================================================================

namespace flyer_wingmen {

struct V3 { float x = 0.0f, y = 0.0f, z = 0.0f; };

inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }
inline bool finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

inline bool normalise(V3& v)
{
   const float len = length(v);
   if (!(len > 1e-4f) || !std::isfinite(len)) return false;
   v = v * (1.0f / len);
   return true;
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- The formation ------------------------------------------------------------
constexpr int   kMaxWingmen     = 8;
constexpr int   kDefaultWingmen = 3;
constexpr float kDefaultSpacing = 25.0f;   // m between neighbours
constexpr float kMinSpacing     = 8.0f;
constexpr float kMaxSpacing     = 60.0f;

inline float clamp_spacing(float s) { return std::isfinite(s) ? clampf(s, kMinSpacing, kMaxSpacing) : kDefaultSpacing; }
inline int clamp_count(int n) { return n < 1 ? 1 : n > kMaxWingmen ? kMaxWingmen : n; }

// A vee: slot 0 back on the right, slot 1 back on the left, then the next pair
// a row further back, each a full spacing from its neighbours.
inline V3 slot_offset(int slot, float spacing)
{
   if (slot < 0) slot = 0;
   const float s = clamp_spacing(spacing);
   constexpr float kDiagonal = 0.70710678f;   // 45 degrees back
   const float row = (float)(slot / 2 + 1);
   const float side = slot % 2 == 0 ? 1.0f : -1.0f;
   return {side * row * s * kDiagonal, 0.0f, -row * s * kDiagonal};
}

// The leader's frame without its roll: ahead the way it points, right level
// with the ground (left-handed, y up: the right of +z is +x), up square to
// both. Pointing straight up or down there is no level right, so its own right
// (`fallbackRight`, from its matrix) stands in.
struct Frame {
   V3 right, up, fwd;
   bool valid = false;
};

inline Frame level_frame(V3 forward, V3 fallbackRight)
{
   Frame f;
   f.fwd = forward;
   if (!normalise(f.fwd)) return f;
   f.right = {f.fwd.z, 0.0f, -f.fwd.x};
   if (!normalise(f.right)) {
      f.right = fallbackRight - f.fwd * dot(fallbackRight, f.fwd);
      if (!normalise(f.right)) return f;
   }
   f.up = cross(f.fwd, f.right);
   f.valid = normalise(f.up);
   return f;
}

inline V3 to_world(const Frame& f, V3 origin, V3 offset)
{
   return origin + f.right * offset.x + f.up * offset.y + f.fwd * offset.z;
}

// Where a point lies from `from` in the leader's frame: x right, y up, z ahead.
inline V3 in_frame(const Frame& f, V3 from, V3 to)
{
   const V3 d = to - from;
   return {dot(d, f.right), dot(d, f.up), dot(d, f.fwd)};
}

// ---- Steering -------------------------------------------------------------------
// The flight code turns a flyer toward a point and slows it once the point is
// off its nose. A wingman is steered across and up to its slot, at a point
// always ahead of itself along the formation's heading: as far ahead as the
// slot is, plus a lead. One that overruns its slot so flies on and lets its
// speed bring it back, rather than turning round for a point behind it (the
// first test's U-turns).
constexpr float kLeadSeconds = 0.4f;    // the lead: this much of the leader's flight ...
constexpr float kMinLead     = 25.0f;   // ... and never less than this, m

inline float lead_distance(float leaderSpeed)
{
   const float lead = std::isfinite(leaderSpeed) && leaderSpeed > 0.0f ? leaderSpeed * kLeadSeconds : 0.0f;
   return lead > kMinLead ? lead : kMinLead;
}

// `slot` is where the slot lies from the wingman in the leader's frame;
// `climb` the trim that holds its height (Trim). The result is in the same
// frame, from the wingman.
inline V3 steer_offset(V3 slot, float lead, float climb)
{
   return {slot.x, slot.y + climb, (slot.z > 0.0f ? slot.z : 0.0f) + lead};
}

// Clear of the leader: inside kClearance of it the point is pushed away from
// it, across and up or down, the harder the closer.
constexpr float kClearance = 20.0f;   // m

inline V3 keep_clear(V3 steer, V3 leader)
{
   const float d = length(leader);
   if (!(d < kClearance)) return steer;
   const float push = (kClearance - d) * 2.0f;
   V3 out = steer;
   out.x += leader.x > 0.0f ? -push : push;
   out.y += leader.y > 0.0f ? -push * 0.5f : push * 0.5f;
   return out;
}

// The speed it is asked for, m/s: the leader's, plus kCatchUp for every metre
// its slot is ahead of it (less for every metre behind), within what it can fly.
constexpr float kCatchUp = 0.5f;   // m/s per metre

inline float formation_speed(float leaderSpeed, float slotAhead, float minSpeed, float maxSpeed)
{
   if (!std::isfinite(leaderSpeed) || !std::isfinite(slotAhead) || !(maxSpeed > 0.0f)) return maxSpeed > 0.0f ? maxSpeed : 0.0f;
   const float lo = minSpeed > 0.0f && minSpeed < maxSpeed ? minSpeed : 0.0f;
   return clampf(leaderSpeed + kCatchUp * slotAhead, lo, maxSpeed);
}

// Steady errors the flight code leaves (the first test's wingmen sat ~6 m
// under their slots and ~20 m ahead) are trimmed out slowly, from errors near
// the slot only, so a turn or a join does not wind them up: the height by
// aiming above or below, the place along the line by speed.
constexpr float kTrimRate     = 0.5f;    // per second, of the error added to the trim
constexpr float kTrimNear     = 40.0f;   // m: further off its slot than this, no trimming
constexpr float kMaxClimbTrim = 15.0f;   // m
constexpr float kMaxSpeedTrim = 10.0f;   // m/s

struct Trim {
   float climb = 0.0f;   // m added to the height it aims at
   float speed = 0.0f;   // m/s added to the speed it is asked for
};

inline void update_trim(Trim& t, V3 slot, float dt)
{
   if (!(dt > 0.0f) || dt > 0.5f || !finite(slot) || !(length(slot) < kTrimNear)) return;
   t.climb = clampf(t.climb + slot.y * kTrimRate * dt, -kMaxClimbTrim, kMaxClimbTrim);
   t.speed = clampf(t.speed + slot.z * kCatchUp * kTrimRate * dt, -kMaxSpeedTrim, kMaxSpeedTrim);
}

// A wingman alone may fly outside its class's speeds to match its leader's:
// up to kStretchUp above the leader to catch up, down to kStretchDown below it
// to wait (never under kSlowest). Its own speeds stand where they reach further.
constexpr float kStretchUp   = 30.0f;   // m/s
constexpr float kStretchDown = 20.0f;   // m/s
constexpr float kSlowest     = 10.0f;   // m/s

inline float stretched_max(float classMax, float leaderSpeed)
{
   if (!std::isfinite(leaderSpeed) || !std::isfinite(classMax)) return classMax;
   const float need = leaderSpeed + kStretchUp;
   return classMax > need ? classMax : need;
}

inline float stretched_min(float classMin, float leaderSpeed)
{
   if (!std::isfinite(leaderSpeed) || !std::isfinite(classMin)) return classMin;
   const float need = leaderSpeed - kStretchDown > kSlowest ? leaderSpeed - kStretchDown : kSlowest;
   return classMin < need ? classMin : need;
}

// What the AI asks for is a target (0 to 1) that becomes a throttle, and the
// throttle a speed, each the game's own way:
//   - throttle = AIUtil::GetThrustForTargetSpeed(target, min, mid, max): the
//     target 0 gives zero_thrust (-1 for a flyer whose slowest speed is above
//     0), and the target 1 full throttle, in a straight line between;
//   - speed = EntityFlyer::Update (modtools 0x004FED19): from the slowest speed
//     at -1 to the mid speed at 0, and from mid to the top speed at +1,
//     then kept within the slowest and the top.
// The min, mid and max are the flyer's getters', a wingman's stretched.
inline float zero_thrust(float minSpeed, float midSpeed, float maxSpeed)
{
   if (minSpeed > 0.0f) return -1.0f;
   if (midSpeed > 0.0f) return minSpeed / (midSpeed - minSpeed);
   return maxSpeed > 0.0f ? (0.0f - midSpeed) / (maxSpeed - midSpeed) : 1.0f;
}

// The target that flies `speed`. Outside what it can fly: the nearest end.
inline float target_for_speed(float speed, float minSpeed, float midSpeed, float maxSpeed)
{
   if (!std::isfinite(speed) || !std::isfinite(minSpeed) || !std::isfinite(midSpeed) || !std::isfinite(maxSpeed))
      return 1.0f;
   float thrust;
   if (speed < midSpeed) thrust = midSpeed > minSpeed ? (speed - midSpeed) / (midSpeed - minSpeed) : -1.0f;
   else thrust = maxSpeed > midSpeed ? (speed - midSpeed) / (maxSpeed - midSpeed) : 1.0f;
   const float zero = zero_thrust(minSpeed, midSpeed, maxSpeed);   // past either end, the clamp below holds it
   if (!(zero < 1.0f) || !std::isfinite(zero)) return 1.0f;
   return clampf((thrust - zero) / (1.0f - zero), 0.0f, 1.0f);
}

// The flight code all but stops a flyer whose point is more than ~26 degrees
// off its nose. A wingman in formation flies its formation speed until the
// point is kKeepSpeedCos off its nose; past that it slows to turn, as the
// stock code would have it.
constexpr float kKeepSpeedCos = 0.5f;    // 60 degrees

inline float keep_speed(float stockTarget, float formationTarget, float offNoseCos)
{
   if (!std::isfinite(offNoseCos) || offNoseCos < kKeepSpeedCos || !std::isfinite(formationTarget)) return stockTarget;
   return formationTarget;
}

// ---- Who flies as one -------------------------------------------------------------
// Only a flyer that can keep up: its top speed at least kKeepUp of the leader's.
constexpr float kKeepUp       = 0.9f;
constexpr float kRecruitRange = 300.0f;   // m: friendlies this near join
constexpr float kKeepRange    = 800.0f;   // m: a wingman further off than this is let go

inline bool can_keep_up(float wingmanMax, float leaderMax)
{
   return leaderMax > 0.0f && std::isfinite(wingmanMax) && wingmanMax >= kKeepUp * leaderMax;
}

// ---- A leader class's ODF keys over the INI --------------------------------------
// Each key a flyer class sets, itself or through ClassParent, decides that
// setting for leaders of the class, whatever the INI says, and a class that
// sets any of them has wingmen even with the INI's FlyerWingmen off; a key it
// leaves out takes the INI's.
//   WingmanClass   a class that may fly as its wingman, by ODF name; several
//                  lines, or several names on one, add up. A child's own lines
//                  replace the list it inherits.
//   WingmanCount   wingmen it takes, 0 to kMaxWingmen; 0 leads none
//   WingmanAuto    1: friendlies join it at takeoff; 0: only the squad key
//                  recruits them
//   WingmanSlot    "x y z [class ...]": a slot in the leader's frame, roll
//                  left out (x right, y up, z ahead, metres), one line per
//                  slot in the order wingmen take them, as a command vehicle's
//                  SpawnPointLocation lines are written (a further number,
//                  the spawn points' angle, is ignored). Classes named after
//                  it keep the slot for themselves; one not on the leader's
//                  WingmanClass list is ignored, and a slot left with none is
//                  open to any class that may follow. Its lines set how many
//                  wingmen it takes, unless WingmanCount asks for fewer; a
//                  child's own lines replace the slots it inherits. Without
//                  any, the vee (slot_offset).
constexpr int kMaxClasses     = 16;
constexpr int kMaxSlotClasses = 4;

struct ClassKeys {
   uint32_t classes[kMaxClasses] = {};   // PblHash of each ODF name, as EntityClass::mId holds it
   int      classCount = 0;
   bool     hasClasses = false;
   bool     ownClasses = false;   // its own lines have begun replacing the inherited list
   bool     hasCount = false;
   int      count = 0;
   bool     hasAuto = false;
   bool     autoForm = true;
   V3       slots[kMaxWingmen];
   uint32_t slotClasses[kMaxWingmen][kMaxSlotClasses] = {};   // the classes each slot is kept for
   int      slotClassCount[kMaxWingmen] = {};
   int      slotCount = 0;
   bool     hasSlots = false;
   bool     ownSlots = false;     // its own lines have begun replacing the inherited slots
};

// A value as an ODF writes it: spaces and quotes round it ignored.
inline void trim(const char*& b, const char*& e)
{
   while (b < e && (*b == ' ' || *b == '\t' || *b == '"')) ++b;
   while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '"' || e[-1] == '\r' || e[-1] == '\n')) --e;
}

// Each name in a WingmanClass value: separated by spaces, commas or quotes.
template <class F>
inline void for_each_name(const char* value, F f)
{
   if (!value) return;
   const char* p = value;
   while (*p) {
      while (*p == ' ' || *p == '\t' || *p == ',' || *p == '"' || *p == '\r' || *p == '\n') ++p;
      const char* b = p;
      while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '"' && *p != '\r' && *p != '\n') ++p;
      if (p > b) f(b, (unsigned)(p - b));
   }
}

// One WingmanClass name, by its hash: the first of a class's own lines clears
// what it inherited.
inline void add_class(ClassKeys& k, uint32_t id)
{
   if (!k.ownClasses) {
      k.classCount = 0;
      k.ownClasses = true;
   }
   k.hasClasses = true;
   if (!id) return;
   for (int i = 0; i < k.classCount; ++i)
      if (k.classes[i] == id) return;
   if (k.classCount < kMaxClasses) k.classes[k.classCount++] = id;
}

inline bool read_number(const char* value, double& out)
{
   if (!value) return false;
   const char* b = value;
   const char* e = value;
   while (*e) ++e;
   trim(b, e);
   if (b == e) return false;
   char buf[32];
   const size_t n = (size_t)(e - b) < sizeof buf - 1 ? (size_t)(e - b) : sizeof buf - 1;
   for (size_t i = 0; i < n; ++i) buf[i] = b[i];
   buf[n] = 0;
   char* end = nullptr;
   out = std::strtod(buf, &end);
   return end && end != buf && *end == 0 && std::isfinite(out);
}

inline bool set_count(ClassKeys& k, const char* value)
{
   double v;
   if (!read_number(value, v)) return false;
   k.hasCount = true;
   k.count = v < 0 ? 0 : v > kMaxWingmen ? kMaxWingmen : (int)v;
   return true;
}

// One WingmanSlot line, "x y z [class ...]": false, leaving the slots as they
// were, for a value without three numbers first. `hash` gives a class name's
// id as EntityClass::mId holds it (PblHash). The first of a class's own lines
// clears what it inherited.
template <class Hash>
inline bool add_slot(ClassKeys& k, const char* value, Hash hash)
{
   if (!value) return false;
   const char* b = value;
   const char* e = value;
   while (*e) ++e;
   trim(b, e);
   char buf[256];
   const size_t n = (size_t)(e - b) < sizeof buf - 1 ? (size_t)(e - b) : sizeof buf - 1;
   for (size_t i = 0; i < n; ++i) buf[i] = b[i] == ',' ? ' ' : b[i];
   buf[n] = 0;
   float xyz[3];
   char* p = buf;
   for (float& c : xyz) {
      char* end = nullptr;
      const double v = std::strtod(p, &end);
      if (end == p || !std::isfinite(v)) return false;
      c = (float)v;
      p = end;
   }
   if (!k.ownSlots) {
      k.slotCount = 0;
      k.ownSlots = true;
   }
   k.hasSlots = true;
   if (k.slotCount >= kMaxWingmen) return true;
   const int slot = k.slotCount++;
   k.slots[slot] = {xyz[0], xyz[1], xyz[2]};
   k.slotClassCount[slot] = 0;
   // The rest: names of the classes it is kept for; a number (the spawn
   // points' angle) is passed over.
   for_each_name(p, [&](const char* b, unsigned len) {
      char word[64];
      const unsigned n = len < sizeof word - 1 ? len : (unsigned)(sizeof word - 1);
      for (unsigned i = 0; i < n; ++i) word[i] = b[i];
      word[n] = 0;
      char* end = nullptr;
      std::strtod(word, &end);
      if (end && end != word && *end == 0) return;   // a number
      const uint32_t id = hash(b, len);
      if (!id) return;
      for (int i = 0; i < k.slotClassCount[slot]; ++i)
         if (k.slotClasses[slot][i] == id) return;
      if (k.slotClassCount[slot] < kMaxSlotClasses) k.slotClasses[slot][k.slotClassCount[slot]++] = id;
   });
   return true;
}

inline bool set_auto(ClassKeys& k, const char* value)
{
   double v;
   if (!read_number(value, v)) return false;
   k.hasAuto = true;
   k.autoForm = v != 0.0;
   return true;
}

// A child class starts as its parent; its own WingmanClass and WingmanSlot
// lines replace the lists.
inline ClassKeys inherited(const ClassKeys& parent)
{
   ClassKeys k = parent;
   k.ownClasses = false;
   k.ownSlots = false;
   return k;
}

inline bool sets_any(const ClassKeys* k) { return k && (k->hasClasses || k->hasCount || k->hasAuto || k->hasSlots); }

// Whether a flyer of class `followerId`, one that may follow this leader, may
// take slot `slot`: the classes its WingmanSlot line names, those of them on
// the leader's WingmanClass list where it has one; with none, any.
inline bool slot_accepts(const ClassKeys* leader, int slot, uint32_t followerId)
{
   if (!leader || !leader->hasSlots || slot < 0 || slot >= leader->slotCount) return true;
   int named = 0;
   for (int i = 0; i < leader->slotClassCount[slot]; ++i) {
      const uint32_t id = leader->slotClasses[slot][i];
      if (leader->hasClasses) {
         bool follows = false;
         for (int j = 0; j < leader->classCount; ++j) follows |= leader->classes[j] == id;
         if (!follows) continue;   // not a class that may follow: ignored
      }
      ++named;
      if (id == followerId) return true;
   }
   return named == 0;
}

// Slot `slot` of this leader's formation: its class's WingmanSlot where it has
// one, else the vee at `spacing`.
inline V3 slot_for(const ClassKeys* leader, int slot, float spacing)
{
   if (leader && leader->hasSlots && slot >= 0 && slot < leader->slotCount) return leader->slots[slot];
   return slot_offset(slot, spacing);
}

// Whether a flyer of class `followerId` may fly as this leader's wingman: the
// leader's list where it has one, else the INI's rule, keeping up.
inline bool accepts(const ClassKeys* leader, uint32_t followerId, bool keepsUp)
{
   if (leader && leader->hasClasses) {
      for (int i = 0; i < leader->classCount; ++i)
         if (leader->classes[i] == followerId) return true;
      return false;
   }
   return keepsUp;
}

// Wingmen it takes: WingmanCount, else its slots, else the INI; never more than
// it has slots for.
inline int count_for(const ClassKeys* leader, int ini)
{
   int n = leader && leader->hasCount ? leader->count : leader && leader->hasSlots ? leader->slotCount : ini;
   if (leader && leader->hasSlots && n > leader->slotCount) n = leader->slotCount;
   return n < 0 ? 0 : n > kMaxWingmen ? kMaxWingmen : n;
}

inline bool auto_for(const ClassKeys* leader, bool ini) { return leader && leader->hasAuto ? leader->autoForm : ini; }

// ---- How well it holds its slot, for the log ----------------------------------------
struct Stats {
   int    samples = 0;
   double distance = 0.0, ahead = 0.0, right = 0.0, up = 0.0, speed = 0.0, leaderSpeed = 0.0;
   float  worst = 0.0f;

   // `slot` is where the slot lies from the wingman, in the leader's frame.
   void add(V3 slot, float wingmanSpeed, float leader)
   {
      if (!finite(slot) || !std::isfinite(wingmanSpeed) || !std::isfinite(leader)) return;
      const float d = length(slot);
      ++samples;
      distance += d;
      ahead += slot.z;
      right += slot.x;
      up += slot.y;
      speed += wingmanSpeed;
      leaderSpeed += leader;
      if (d > worst) worst = d;
   }
};

} // namespace flyer_wingmen
