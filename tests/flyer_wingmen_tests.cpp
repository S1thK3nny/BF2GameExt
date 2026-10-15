// Standalone tests (not a DLL build) for flyer wingmen: where each wingman's
// slot is, the point it is steered at, the speed it is asked for and who may
// fly as one - the logic in flyer_wingmen_core.hpp, fed what flyer_wingmen.cpp
// reads.
#include "../PatcherDLL/src/ai/flyer_orders_core.hpp"
#include "../PatcherDLL/src/ai/flyer_trail_core.hpp"
#include "../PatcherDLL/src/ai/flyer_wingmen_core.hpp"
#include "../PatcherDLL/src/ai/landing_region_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>

using namespace flyer_wingmen;

namespace {

const float kNan = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// A class name's id, as the game's PblHash makes it (FNV-1a, each byte OR 0x20).
uint32_t test_hash(const char* s, unsigned n)
{
   uint32_t h = 0x811C9DC5u;
   unsigned i = 0;
   for (; i < n && s[i]; ++i) h = (h ^ ((uint32_t)(int32_t)(signed char)s[i] | 0x20u)) * 0x01000193u;
   return i ? h : 0;
}
bool near(V3 a, V3 b, float eps = 1e-4f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }

// A vee: right then left, a row further back each pair, every slot a full
// spacing from its neighbours and behind the leader.
void the_vee()
{
   const float s = 20.0f;
   const V3 a = slot_offset(0, s), b = slot_offset(1, s), c = slot_offset(2, s), d = slot_offset(3, s);
   assert(a.x > 0.0f && b.x < 0.0f && c.x > 0.0f && d.x < 0.0f);
   assert(near(a.x, -b.x) && near(a.z, b.z) && near(c.z, d.z));
   assert(a.z < 0.0f && c.z < a.z);
   assert(near(length(a), s) && near(length(c - a), s, 1e-3f) && near(length(d - b), s, 1e-3f));
   for (int i = 0; i < kMaxWingmen; ++i) assert(slot_offset(i, s).y == 0.0f);
   // Spacing is kept within its range.
   assert(near(length(slot_offset(0, 1.0f)), kMinSpacing));
   assert(near(length(slot_offset(0, 500.0f)), kMaxSpacing));
   assert(near(length(slot_offset(0, kNan)), kDefaultSpacing));
   assert(near(slot_offset(-3, s), slot_offset(0, s)));
   assert(clamp_count(0) == 1 && clamp_count(9) == kMaxWingmen && clamp_count(2) == 2);
}

// The leader's frame keeps its heading and pitch but never its roll.
void level_frames()
{
   // Flying along +z: right is +x, up is +y.
   Frame f = level_frame({0, 0, 50}, {0, 0, 0});
   assert(f.valid && near(f.right, {1, 0, 0}) && near(f.up, {0, 1, 0}) && near(f.fwd, {0, 0, 1}));
   // Along +x: right is -z (left-handed, y up).
   f = level_frame({3, 0, 0}, {0, 0, 0});
   assert(f.valid && near(f.right, {0, 0, -1}) && near(f.up, {0, 1, 0}));
   // Climbing: right stays level, up tips back, all square.
   f = level_frame({0, 0.6f, 0.8f}, {0, 0, 0});
   assert(f.valid && near(f.right.y, 0.0f) && near(f.right, {1, 0, 0}));
   assert(near(dot(f.up, f.fwd), 0.0f) && near(dot(f.up, f.right), 0.0f) && f.up.y > 0.0f && f.up.z < 0.0f);
   // Straight up: its own right stands in.
   f = level_frame({0, 1, 0}, {0, 0, -1});
   assert(f.valid && near(f.right, {0, 0, -1}) && near(dot(f.up, f.fwd), 0.0f));
   // Nothing to go on.
   assert(!level_frame({0, 0, 0}, {1, 0, 0}).valid);
   assert(!level_frame({0, 1, 0}, {0, 1, 0}).valid);
   assert(!level_frame({kNan, 0, 1}, {1, 0, 0}).valid);
}

// A slot goes out into the world and comes back the same.
void slots_in_the_world()
{
   const Frame f = level_frame({1, 0.2f, 1}, {0, 0, 0});
   const V3 leader{100, 50, -20};
   for (int i = 0; i < kMaxWingmen; ++i) {
      const V3 o = slot_offset(i, 25.0f);
      const V3 w = to_world(f, leader, o);
      assert(near(length(w - leader), length(o), 1e-3f));
      assert(near(in_frame(f, leader, w), o, 1e-3f));
   }
   // A wingman 10 m behind and 2 m right of its slot sees the slot ahead and to its left.
   const V3 slot = to_world(f, leader, slot_offset(0, 25.0f));
   const V3 wing = to_world(f, slot, {2, 0, -10});
   const V3 rel = in_frame(f, wing, slot);
   assert(near(rel.z, 10.0f, 1e-3f) && near(rel.x, -2.0f, 1e-3f) && near(rel.y, 0.0f, 1e-3f));
}

// It is steered across and up to its slot, at a point always ahead of itself:
// one that overruns its slot flies on rather than turning back for it.
void steering()
{
   assert(near(lead_distance(80.0f), 80.0f * kLeadSeconds));
   assert(near(lead_distance(10.0f), kMinLead) && near(lead_distance(kNan), kMinLead) && near(lead_distance(-5.0f), kMinLead));
   const float lead = 30.0f;
   // Behind its slot: ahead as far as the slot is, plus the lead.
   assert(near(steer_offset({4, -2, 50}, lead, 0.0f), {4, -2, 50 + lead}));
   // Past its slot: still ahead of itself, by the lead alone.
   assert(near(steer_offset({4, -2, -60}, lead, 0.0f), {4, -2, lead}));
   for (float z = -300.0f; z <= 300.0f; z += 25.0f) assert(steer_offset({0, 0, z}, lead, 0.0f).z >= lead);
   // The climb trim lifts or lowers it.
   assert(near(steer_offset({0, 3, 0}, lead, 6.0f).y, 9.0f));
   // Too near the leader: pushed away from it, across and up or down.
   const V3 s{0, 0, lead};
   assert(near(keep_clear(s, {30, 0, 0}), s));                    // clear: untouched
   const V3 right = keep_clear(s, {5, 2, 0});                     // leader right and above
   assert(right.x < 0.0f && right.y < 0.0f && near(right.z, lead));
   const V3 left = keep_clear(s, {-5, -2, 0});                    // leader left and below
   assert(left.x > 0.0f && left.y > 0.0f);
   assert(std::fabs(keep_clear(s, {2, 0, 0}).x) > std::fabs(keep_clear(s, {15, 0, 0}).x));   // harder the closer
}

// Steady errors are trimmed out slowly, near the slot only.
void trimming()
{
   Trim t;
   for (int i = 0; i < 60; ++i) update_trim(t, {0, 6, -20}, 1.0f / 60.0f);   // 6 m under it, 20 m past it
   assert(t.climb > 2.5f && t.climb < 3.5f);                                // ~0.5/s of 6 m over a second
   assert(t.speed < -4.5f && t.speed > -5.5f);                              // and slower
   for (int i = 0; i < 6000; ++i) update_trim(t, {0, 6, -20}, 1.0f / 60.0f);
   assert(near(t.climb, kMaxClimbTrim) && near(t.speed, -kMaxSpeedTrim));   // within its limits
   Trim far;
   update_trim(far, {0, 30, -50}, 0.5f);                                    // a turn or a join: left alone
   assert(far.climb == 0.0f && far.speed == 0.0f);
   Trim bad;
   update_trim(bad, {0, 1, 1}, 0.0f);
   update_trim(bad, {0, 1, 1}, 0.6f);
   update_trim(bad, {kNan, 1, 1}, 0.1f);
   assert(bad.climb == 0.0f && bad.speed == 0.0f);
}

// A wingman may fly faster or slower than its class allows, to match its leader.
void stretching()
{
   assert(near(stretched_max(100.0f, 60.0f), 100.0f));                      // fast enough already
   assert(near(stretched_max(100.0f, 95.0f), 95.0f + kStretchUp));          // a fast or boosting leader
   assert(near(stretched_min(60.0f, 70.0f), 50.0f));                        // to wait for a slow one
   assert(near(stretched_min(30.0f, 70.0f), 30.0f));                        // slow enough already
   assert(near(stretched_min(60.0f, 15.0f), kSlowest));
   assert(near(stretched_max(100.0f, kNan), 100.0f) && near(stretched_min(60.0f, kNan), 60.0f));
}

// Behind its slot it flies faster than the leader, ahead of it slower, always
// within what it can fly.
void speeds()
{
   const float lead = 70.0f, lo = 30.0f, hi = 100.0f;
   assert(near(formation_speed(lead, 0.0f, lo, hi), lead));
   assert(near(formation_speed(lead, 20.0f, lo, hi), lead + 20.0f * kCatchUp));
   assert(near(formation_speed(lead, -20.0f, lo, hi), lead - 20.0f * kCatchUp));
   assert(near(formation_speed(lead, 500.0f, lo, hi), hi));
   assert(near(formation_speed(lead, -500.0f, lo, hi), lo));
   // A minimum at or above the maximum is ignored rather than trusted.
   assert(near(formation_speed(lead, -500.0f, 120.0f, hi), 0.0f));
   // Bad input asks for its top speed, so it does not fall out of the sky.
   assert(near(formation_speed(kNan, 0.0f, lo, hi), hi));
   assert(near(formation_speed(lead, kNan, lo, hi), hi));
   assert(near(formation_speed(lead, 0.0f, lo, 0.0f), 0.0f));
}

// The game's own chain, as read: AIUtil::GetThrustForTargetSpeed, then
// EntityFlyer::Update's throttle (modtools 0x004FED19), kept within min and max.
float game_thrust(float target, float mn, float md, float mx)
{
   float zero = -1.0f;
   if (mn <= 0.0f) {
      if (md <= 0.0f) {
         zero = 1.0f;
         if (0.0f < mx) zero = (0.0f - md) / (mx - md);
      } else {
         zero = mn / (md - mn);
      }
   }
   if (0.0f <= target) return (1.0f - zero) * target + zero;
   return (zero + 1.0f) * target - 1.0f;
}

float game_speed(float thrust, float mn, float md, float mx)
{
   const float s = thrust < 0.0f ? md - (mn - md) * thrust : md + (mx - md) * thrust;
   return s < mn ? mn : s > mx ? mx : s;
}

// The target asked for flies the speed wanted, through the game's own chain.
void targets()
{
   struct Flyer { float mn, md, mx; } flyers[] = {
      {60, 80, 100},    // a stock fighter
      {20, 75, 125},    // stretched to match a slow leader and a boosting one
      {10, 70, 70},     // no headroom above mid
      {70, 70, 110},    // none below
   };
   for (const Flyer& f : flyers) {
      for (float s = f.mn; s <= f.mx; s += 0.5f) {
         const float target = target_for_speed(s, f.mn, f.md, f.mx);
         assert(target >= 0.0f && target <= 1.0f);
         assert(near(game_speed(game_thrust(target, f.mn, f.md, f.mx), f.mn, f.md, f.mx), s, 1e-3f));
      }
      // Beyond what it can fly: the nearest end.
      const auto flown = [&](float s) {
         return game_speed(game_thrust(target_for_speed(s, f.mn, f.md, f.mx), f.mn, f.md, f.mx), f.mn, f.md, f.mx);
      };
      assert(near(flown(f.mn - 30.0f), f.mn, 1e-3f) && near(flown(f.mx + 30.0f), f.mx, 1e-3f));
      assert(near(target_for_speed(f.mx + 30.0f, f.mn, f.md, f.mx), 1.0f));
   }
   // The first test's wingman: the old share-of-top-speed target asked a 60/75/105
   // flyer for 45 m/s and got ~73 (the log showed ~71); this one gets 45.
   assert(near(game_speed(game_thrust(45.0f / 105.0f, 60, 75, 105), 60, 75, 105), 72.86f, 0.05f));
   assert(near(game_speed(game_thrust(target_for_speed(45, 30, 75, 105), 30, 75, 105), 30, 75, 105), 45.0f, 1e-3f));
   // A flyer whose slowest speed is 0 or below cannot be asked under its mid speed.
   assert(near(game_speed(game_thrust(target_for_speed(20, 0, 40, 90), 0, 40, 90), 0, 40, 90), 40.0f, 1e-3f));
   assert(near(game_speed(game_thrust(target_for_speed(60, 0, 40, 90), 0, 40, 90), 0, 40, 90), 60.0f, 1e-3f));
   assert(near(target_for_speed(kNan, 60, 80, 100), 1.0f) && near(target_for_speed(70, 60, kNan, 100), 1.0f));
}

// In formation it flies its formation speed until its point is 60 degrees
// off its nose; past that it slows as the game would have it.
void turning()
{
   assert(near(keep_speed(0.1f, 0.8f, 1.0f), 0.8f));
   assert(near(keep_speed(0.9f, 0.3f, 0.6f), 0.3f));                   // slower too
   assert(near(keep_speed(0.1f, 0.8f, kKeepSpeedCos - 0.01f), 0.1f));
   assert(near(keep_speed(0.1f, 0.8f, kNan), 0.1f) && near(keep_speed(0.1f, kNan, 1.0f), 0.1f));
}

// Only a flyer that can keep up joins.
void who_flies()
{
   assert(can_keep_up(90.0f, 100.0f) && can_keep_up(130.0f, 100.0f));
   assert(!can_keep_up(89.0f, 100.0f) && !can_keep_up(90.0f, 0.0f) && !can_keep_up(kNan, 100.0f));
}

// A leader class's keys: which classes may follow, how many, whether they join
// at takeoff; none set, the INI decides.
void class_keys()
{
   // Names on one line or several.
   int n = 0;
   char seen[4][16] = {};
   for_each_name(" \"rep_fly_vwing, rep_fly_arc170\" ", [&](const char* b, unsigned len) {
      for (unsigned i = 0; i < len && i < 15; ++i) seen[n][i] = b[i];
      ++n;
   });
   assert(n == 2 && std::string(seen[0]) == "rep_fly_vwing" && std::string(seen[1]) == "rep_fly_arc170");
   n = 0;
   for_each_name("  ", [&](const char*, unsigned) { ++n; });
   for_each_name(nullptr, [&](const char*, unsigned) { ++n; });
   assert(n == 0);

   ClassKeys parent;
   add_class(parent, 11);
   add_class(parent, 22);
   add_class(parent, 22);   // once
   assert(parent.hasClasses && parent.classCount == 2);
   assert(accepts(&parent, 22, false) && !accepts(&parent, 33, true));      // the list decides, not keeping up
   // A child inherits the list until its own lines replace it.
   ClassKeys child = inherited(parent);
   assert(accepts(&child, 11, false));
   add_class(child, 33);
   assert(child.classCount == 1 && accepts(&child, 33, false) && !accepts(&child, 11, true));
   ClassKeys grandchild = inherited(child);
   add_class(grandchild, 44);
   add_class(grandchild, 55);
   assert(grandchild.classCount == 2 && !accepts(&grandchild, 33, true));
   // No list: keeping up decides.
   ClassKeys none;
   assert(accepts(&none, 99, true) && !accepts(&none, 99, false) && accepts(nullptr, 99, true));
   // The list holds kMaxClasses.
   ClassKeys many;
   for (uint32_t i = 1; i <= kMaxClasses + 5; ++i) add_class(many, i);
   assert(many.classCount == kMaxClasses);

   ClassKeys k;
   assert(!sets_any(&k) && !sets_any(nullptr));
   assert(set_count(k, " \"2\" ") && k.count == 2 && sets_any(&k));
   assert(set_count(k, "9") && k.count == kMaxWingmen);
   assert(set_count(k, "-1") && k.count == 0);
   assert(!set_count(k, "two") && !set_count(k, "") && k.count == 0);
   assert(count_for(&k, 3) == 0 && count_for(&none, 3) == 3 && count_for(nullptr, 20) == kMaxWingmen);
   assert(set_auto(k, "0") && !k.autoForm && !auto_for(&k, true));
   assert(set_auto(k, "1") && k.autoForm && auto_for(&k, false));
   assert(!set_auto(k, "yes"));
   assert(auto_for(&none, false) == false && auto_for(&none, true) == true);
   // Any one key says the class has wingmen.
   ClassKeys onlyAuto, onlyList;
   assert(set_auto(onlyAuto, "1") && sets_any(&onlyAuto));
   add_class(onlyList, 0);   // WingmanClass = "": no class follows, but it is set
   assert(sets_any(&onlyList) && onlyList.classCount == 0 && !accepts(&onlyList, 7, true));
}

// A leader class's own formation: WingmanSlot lines, written as a command
// vehicle's SpawnPointLocation lines are.
void slot_keys()
{
   ClassKeys k;
   assert(add_slot(k, "\"-12.0 2.0 -8.0\"", test_hash) && add_slot(k, "12 2 -8 355", test_hash) &&
          add_slot(k, "0,4,-16", test_hash));
   assert(k.hasSlots && k.slotCount == 3 && sets_any(&k));
   assert(near(k.slots[0], {-12, 2, -8}) && near(k.slots[1], {12, 2, -8}) && near(k.slots[2], {0, 4, -16}));
   // Not three numbers: ignored.
   assert(!add_slot(k, "1 2", test_hash) && !add_slot(k, "", test_hash) && !add_slot(k, nullptr, test_hash) &&
          !add_slot(k, "a b c", test_hash));
   assert(k.slotCount == 3);
   // Its slots, then the vee past them; no keys, the vee.
   assert(near(slot_for(&k, 1, 25.0f), {12, 2, -8}) && near(slot_for(&k, 3, 25.0f), slot_offset(3, 25.0f)));
   assert(near(slot_for(nullptr, 0, 25.0f), slot_offset(0, 25.0f)) && near(slot_for(&k, -1, 25.0f), slot_offset(0, 25.0f)));
   // How many: its slots, unless WingmanCount asks for fewer.
   assert(count_for(&k, 1) == 3);
   ClassKeys fewer = k;
   set_count(fewer, "2");
   assert(count_for(&fewer, 5) == 2);
   ClassKeys more = k;
   set_count(more, "6");
   assert(count_for(&more, 5) == 3);
   // A child inherits them until its own lines replace them.
   ClassKeys child = inherited(k);
   assert(child.slotCount == 3 && near(slot_for(&child, 2, 25.0f), {0, 4, -16}));
   assert(add_slot(child, "5 0 -5", test_hash));
   assert(child.slotCount == 1 && near(child.slots[0], {5, 0, -5}) && count_for(&child, 3) == 1);
   // At most kMaxWingmen.
   ClassKeys many;
   for (int i = 0; i < kMaxWingmen + 3; ++i) add_slot(many, "1 1 1", test_hash);
   assert(many.slotCount == kMaxWingmen && count_for(&many, 1) == kMaxWingmen);
}

// A WingmanSlot line may keep its slot for some classes: those of them on the
// leader's WingmanClass list; a slot left with none is open to any follower.
void slot_classes()
{
   const uint32_t A = test_hash("rep_fly_vwing", 13), B = test_hash("rep_fly_arc170", 14),
                  C = test_hash("cis_fly_droid", 13);
   ClassKeys k;
   add_class(k, A);
   add_class(k, B);                                                               // A and B may follow
   assert(add_slot(k, "-15 0 -10 rep_fly_vwing", test_hash));                     // kept for A
   assert(add_slot(k, "15 0 -10 355 cis_fly_droid", test_hash));                  // C cannot follow: open
   assert(add_slot(k, "0 5 -25 cis_fly_droid, rep_fly_arc170", test_hash));       // C ignored: B only
   assert(add_slot(k, "0 0 -40", test_hash));                                     // open
   assert(k.slotClassCount[0] == 1 && k.slotClassCount[1] == 1 && k.slotClassCount[2] == 2 &&
          k.slotClassCount[3] == 0);
   assert(near(k.slots[1], {15, 0, -10}));                                        // the angle passed over
   assert(slot_accepts(&k, 0, A) && !slot_accepts(&k, 0, B));
   assert(slot_accepts(&k, 1, A) && slot_accepts(&k, 1, B));
   assert(slot_accepts(&k, 2, B) && !slot_accepts(&k, 2, A) && !slot_accepts(&k, 2, C));
   assert(slot_accepts(&k, 3, A) && slot_accepts(&k, 3, B));
   // Past its slots (the vee), or no keys: open.
   assert(slot_accepts(&k, 6, A) && slot_accepts(&k, -1, A) && slot_accepts(nullptr, 0, A));
   // No follow list: a slot's classes keep it for them.
   ClassKeys open;
   assert(add_slot(open, "1 2 3 cis_fly_droid", test_hash));
   assert(slot_accepts(&open, 0, C) && !slot_accepts(&open, 0, A));
   ClassKeys angle;                                                               // the angle is no class
   assert(add_slot(angle, "1 2 3 90 cis_fly_droid", test_hash) && angle.slotClassCount[0] == 1);
   // At most kMaxSlotClasses names, each once; a name starting with a digit is a name.
   ClassKeys many, twice, digit;
   add_slot(many, "0 0 -10 a b c d e a", test_hash);
   add_slot(twice, "0 0 -10 a a", test_hash);
   add_slot(digit, "0 0 -10 1st_fly", test_hash);
   assert(many.slotClassCount[0] == kMaxSlotClasses && twice.slotClassCount[0] == 1);
   assert(digit.slotClassCount[0] == 1 && digit.slotClasses[0][0] == test_hash("1st_fly", 7));
   // Inherited slots keep their classes; a child's own lines replace both.
   ClassKeys child = inherited(k);
   assert(slot_accepts(&child, 0, A) && !slot_accepts(&child, 0, B));
   assert(add_slot(child, "5 0 -5", test_hash));
   assert(child.slotCount == 1 && child.slotClassCount[0] == 0 && slot_accepts(&child, 0, B));
}

// The leader's trick copied down the formation in slot order, kTrickRipple apart,
// the first one kTrickRipple after the leader.
void ripples()
{
   const int vee[] = {0, 1, 2, 3};
   for (int i = 0; i < 4; ++i) assert(ripple_rank(vee[i], vee, 4) == i);
   const int gaps[] = {5, 0, 3};   // slots held, in no order
   assert(ripple_rank(0, gaps, 3) == 0 && ripple_rank(3, gaps, 3) == 1 && ripple_rank(5, gaps, 3) == 2);
   assert(ripple_rank(2, gaps, 0) == 0);
   assert(std::fabs(ripple_delay(0) - kTrickRipple) < 1e-9 && std::fabs(ripple_delay(3) - 4.0 * kTrickRipple) < 1e-9);
   assert(std::fabs(ripple_delay(-2) - kTrickRipple) < 1e-9);
   assert(kTrickRipple > 0.0 && kTrickWait > kTrickRipple && kTrickSettle > 0.0 && kHoldAhead > 0.0f);
   // The flips (0 to 5) turn the leader round; the side roll (6) does not.
   for (int t = 0; t <= 5; ++t) assert(is_flip(t));
   assert(!is_flip(6) && !is_flip(-1) && !is_flip(7));
   // Mirrored, left for right: a right-hand slot behind becomes the left-hand one.
   assert(near(mirrored({15, 2, -10}, true), {-15, 2, -10}) && near(mirrored({15, 2, -10}, false), {15, 2, -10}));
   assert(near(mirrored(slot_offset(0, 20.0f), true), slot_offset(1, 20.0f)));
}

// AI squadrons, by the game's rule: a try every 5.5 to 8.5 s, the reach, who
// may join and in what order, how many.
void squadron_rule()
{
   assert(std::fabs(squadron_wait(0.0) - 5.5) < 1e-9 && std::fabs(squadron_wait(1.0) - 8.5) < 1e-9);
   assert(std::fabs(squadron_wait(0.5) - 7.0) < 1e-9 && std::fabs(squadron_wait(-3.0) - 7.0) < 1e-9);
   // The class's own reach (squared, as it holds it), else 30 bounding radii.
   assert(near(squadron_reach2(40000.0f, 7.0f), 40000.0f));
   assert(near(squadron_reach2(0.0f, 7.0f), 210.0f * 210.0f));
   assert(near(squadron_reach2(0.0f, 0.0f), 0.0f) && near(squadron_reach2(kNan, kNan), 0.0f));
   // Within reach, heading within the class's angle (its cosine; the default 0 is 90 degrees).
   float a = 0.0f, b = 0.0f, c = 0.0f;
   assert(squadron_candidate(100.0f * 100.0f, 200.0f * 200.0f, 0.9f, 0.0f, a));
   assert(squadron_candidate(150.0f * 150.0f, 200.0f * 200.0f, 0.9f, 0.0f, b) && a > b);   // nearer first
   assert(squadron_candidate(100.0f * 100.0f, 200.0f * 200.0f, 0.2f, 0.0f, c) && a > c);   // better lined up first
   assert(!squadron_candidate(201.0f * 201.0f, 200.0f * 200.0f, 1.0f, 0.0f, a));           // too far
   assert(!squadron_candidate(10.0f, 200.0f * 200.0f, -0.1f, 0.0f, a));                    // heading away
   assert(squadron_candidate(10.0f, 200.0f * 200.0f, -0.9f, -1.0f, a));                    // -1: any heading ...
   assert(near(a, 1.0f - 10.0f / (200.0f * 200.0f)));                                      // ... the heading left out of the score
   assert(!squadron_candidate(kNan, 200.0f * 200.0f, 1.0f, 0.0f, a) && !squadron_candidate(10.0f, 0.0f, 1.0f, 0.0f, a));
   // How many: the most a shape holds with its leader, then one fewer each try, a pair
   // only when one was all there was.
   const auto sizes = [](std::initializer_list<int> have) {
      return [have](int n) {
         for (int h : have)
            if (h == n) return true;
         return false;
      };
   };
   assert(squadron_members(5, sizes({3, 4})) == 3);   // five found: four flyers, a box
   assert(squadron_members(2, sizes({3, 4})) == 2);   // two: three flyers
   assert(squadron_members(5, sizes({3})) == 2);      // down through the sizes to the one it has
   assert(squadron_members(1, sizes({3, 4})) == 0);   // one: no pair shape
   assert(squadron_members(1, sizes({2})) == 1);      // one, with a pair shape
   assert(squadron_members(3, sizes({2})) == 0);      // never down to a pair after more
   assert(squadron_members(0, sizes({2, 3})) == 0 && squadron_members(4, sizes({})) == 0);
}

// A map's formation shapes, as the game reads them, turned into slots from the
// leader: Tri-form, five-form and Box-Form from the stock space maps.
void formation_shapes()
{
   // Points as EntityFormation::Read leaves them: from its root.
   const auto from_root = [](const V3* raw, int n, int root, V3* out) {
      for (int i = 0; i < n; ++i) out[i] = raw[i] - raw[root];
   };
   V3 slots[kMaxWingmen];
   // Tri-form (spa1): the root, the middle point, is the leader; the others its two
   // members, in order, ~25 m off: left as they are.
   const V3 tri[3] = {{-2341.548340f, -256.0f, 2047.477295f}, {-2319.903564f, -256.0f, 2061.713135f},
                      {-2298.883057f, -256.0f, 2048.499023f}};
   V3 triRel[3];
   from_root(tri, 3, 1, triRel);
   assert(shape_size(3, true) == 3);
   assert(shape_slots(triRel, 3, 1, true, 2, slots) == 2);
   assert(near(slots[0], {-21.644775f, 0.0f, -14.235840f}, 1e-3f) && near(slots[1], {21.020507f, 0.0f, -13.214112f}, 1e-3f));
   assert(shape_slots(triRel, 3, 1, true, 1, slots) == 1);   // fewer members than the shape holds
   // five-form (spa2): no root named, so the first point; its members 12.6 m off,
   // scaled up to kMinShapeGap.
   const V3 five[3] = {{-1902.432373f, 0.0f, -2945.223145f}, {-1893.092651f, 0.0f, -2953.621582f},
                       {-1883.008667f, 0.0f, -2944.732178f}};
   V3 fiveRel[3];
   from_root(five, 3, 0, fiveRel);
   assert(shape_slots(fiveRel, 3, 0, true, 5, slots) == 2);   // as many as it holds
   assert(near(length(slots[0]), kMinShapeGap, 1e-3f));
   const float scale = kMinShapeGap / length(five[1] - five[0]);
   assert(near(slots[1], (five[2] - five[0]) * scale, 1e-3f));
   assert(length(slots[1] - slots[0]) >= kMinShapeGap - 1e-3f);
   // Box-Form (spa1): its root (the fifth point, the middle) is no slot; the leader takes
   // the first corner, its members the other three.
   const V3 box[5] = {{-2408.126953f, -256.0f, 2012.925049f}, {-2407.313232f, -256.0f, 1954.514526f},
                      {-2338.574463f, -256.0f, 1957.552124f}, {-2338.720703f, -256.0f, 2012.261597f},
                      {-2371.845215f, -256.0f, 1986.511597f}};
   V3 boxRel[5];
   from_root(box, 5, 4, boxRel);
   assert(shape_size(5, false) == 4);
   assert(shape_slots(boxRel, 5, 4, false, 3, slots) == 3);
   assert(near(slots[0], box[1] - box[0], 1e-3f) && near(slots[1], box[2] - box[0], 1e-3f) &&
          near(slots[2], box[3] - box[0], 1e-3f));
   // Two members far behind but close together: scaled until they are kMinShapeGap apart.
   const V3 pair[3] = {{0, 0, 0}, {-5, 0, -40}, {5, 0, -40}};
   assert(shape_slots(pair, 3, 0, true, 2, slots) == 2);
   assert(near(length(slots[1] - slots[0]), kMinShapeGap, 1e-3f) && near(slots[0], V3{-5, 0, -40} * 2.2f, 1e-3f));
   // A root at 0 that is no slot: the leader at the next point.
   const V3 line[3] = {{0, 0, 0}, {0, 0, 30}, {0, 0, -30}};
   assert(shape_slots(line, 3, 0, false, 1, slots) == 1 && near(slots[0], {0, 0, -60}));
   // Nothing it can use.
   const V3 bad[2] = {{kNan, 0, 0}, {0, 0, 0}};
   assert(shape_slots(bad, 2, 0, true, 1, slots) == 0 && shape_slots(line, 1, 0, true, 1, slots) == 0);
   assert(shape_slots(line, 3, 5, true, 1, slots) == 0 && shape_slots(line, 3, 0, true, 0, slots) == 0);
   assert(shape_slots(nullptr, 3, 0, true, 1, slots) == 0 && shape_size(0, true) == 0);
}

// A formation's points as the game holds them after EntityFormation::Read: from
// the root, z negated (RedPath's node reader), from the .pth's text.
void from_text(const V3* text, int count, int root, V3* game)
{
   for (int i = 0; i < count; ++i) game[i] = {text[i].x - text[root].x, text[i].y - text[root].y, -(text[i].z - text[root].z)};
}

// Which way the maps' shapes face: as drawn (z negated back), and the BF3 maps'
// copies turned back by their nodes' 105 degrees, decided per map.
void drawn_shapes()
{
   // The turn of a node's rotation, the game's (x, y, z, s) from the .pth's w x y z.
   assert(near(drawn_yaw(0, 0, 0, 1), 0.0f) && near(drawn_yaw(0, 0, 0, 0), 0.0f) && near(drawn_yaw(kNan, 0, 0, 1), 0.0f));
   assert(near(drawn_yaw(-0.0f, 0.793353f, -0.0f, 0.608761f) * 57.29578f, 105.0f, 0.05f));    // Cato's formations
   assert(near(drawn_yaw(0.0f, -0.697227f, 0.0f, 0.716856f) * 57.29578f, -88.40f, 0.05f));    // spa1's Tri-form
   assert(near(drawn_yaw(0.0f, 0.793353f * 2, 0.0f, 0.608761f * 2) * 57.29578f, 105.0f, 0.05f));   // unnormalised
   assert(near(turned({1, 2, 0}, 1.5707963f), {0, 2, -1}, 1e-5f) && near(turned({0, 0, 1}, 1.5707963f), {1, 0, 0}, 1e-5f));
   assert(near(as_drawn({1, 2, 3}), {1, 2, -3}));

   V3 slots[kMaxWingmen], other[kMaxWingmen];
   // spa1 (stock): as drawn, its Tri-form is a vee behind its point (the root, the leader);
   // as the game flies it the vee is reversed, its wingmen ahead.
   const V3 triText[3] = {{-2341.548340f, -256.0f, 2047.477295f}, {-2319.903564f, -256.0f, 2061.713135f},
                          {-2298.883057f, -256.0f, 2048.499023f}};
   const V3 lineText[4] = {{-2285.419922f, -256.0f, 2017.704590f}, {-2285.100586f, -248.550903f, 1997.295898f},
                           {-2285.253418f, -256.0f, 1977.072510f}, {-2284.074951f, -256.0f, 1953.823120f}};
   const V3 boxText[5] = {{-2408.126953f, -256.0f, 2012.925049f}, {-2407.313232f, -256.0f, 1954.514526f},
                          {-2338.574463f, -256.0f, 1957.552124f}, {-2338.720703f, -256.0f, 2012.261597f},
                          {-2371.845215f, -256.0f, 1986.511597f}};
   V3 tri[3], line[4], box[5];
   from_text(triText, 3, 1, tri);
   from_text(lineText, 4, 0, line);
   from_text(boxText, 5, 4, box);
   const float triYaw = drawn_yaw(0.0f, -0.697227f, 0.0f, 0.716856f), lineYaw = drawn_yaw(0.0f, -0.702369f, 0.0f, 0.711856f);
   assert(tri[0].z > 0.0f && tri[2].z > 0.0f);                                          // the game's: wingmen ahead
   assert(drawn_slots(tri, 3, 1, true, triYaw, false, 2, slots) == 2);
   assert(near(slots[0], {-21.644776f, 0.0f, -14.235840f}, 1e-3f) && near(slots[1], {21.020507f, 0.0f, -13.214112f}, 1e-3f));
   V3 pts[kMaxShapePoints];
   drawn_points(tri, 3, triYaw, false, pts);
   assert(led_from_front(pts, 3, 1, true));
   drawn_points(tri, 3, triYaw, true, pts);
   assert(!led_from_front(pts, 3, 1, true));                                            // its node turn means nothing
   const ShapeView spa1[] = {{tri, 3, 1, true, triYaw}, {line, 4, 0, true, lineYaw}, {box, 5, 4, false, 0.0f}};
   assert(!unturn_shapes(spa1, 3));
   // ... its Line-Form a line astern, its Box-Form a box with the leader at a front corner.
   assert(drawn_slots(line, 4, 0, true, lineYaw, false, 3, slots) == 3 && slots[0].z < -20.0f && slots[2].z < -60.0f);
   assert(drawn_slots(box, 5, 4, false, 0.0f, false, 3, slots) == 3);
   for (int i = 0; i < 3; ++i) assert(slots[i].z <= kAheadTolerance && slots[i].x > -1.0f);
   drawn_points(box, 5, 0.0f, false, pts);
   assert(led_from_front(pts, 5, 4, false));                                            // led from its front corner, not its middle
   drawn_points(tri, 3, triYaw, false, pts);
   pts[2].z = pts[1].z + 3.5f;                                                          // a wingman just ahead of the leader
   assert(!led_from_front(pts, 3, 1, true));
   pts[2].z = pts[1].z + 2.5f;                                                          // within the tolerance
   assert(led_from_front(pts, 3, 1, true));
   // A root that is no slot is no member either, wherever it lies.
   const V3 ahead[3] = {{0, 0, 0}, {0, 0, -20}, {10, 0, -30}};
   assert(led_from_front(ahead, 3, 0, false) && !led_from_front(ahead, 3, 1, true));

   // spa2 (stock, no turns): its echelons as named, behind the leader.
   const V3 fiveText[3] = {{-1902.432373f, 0.0f, -2945.223145f}, {-1893.092651f, 0.0f, -2953.621582f},
                           {-1883.008667f, 0.0f, -2944.732178f}};
   const V3 leftText[4] = {{-1759.084229f, 0.0f, -3039.962891f}, {-1771.064209f, 0.0f, -3051.339844f},
                           {-1780.917358f, 0.0f, -3060.177734f}, {-1791.252197f, 0.0f, -3069.149902f}};
   const V3 rightText[4] = {{-1661.393677f, 0.0f, -2944.405762f}, {-1651.297119f, 0.0f, -2954.517334f},
                            {-1640.706787f, 0.0f, -2966.205566f}, {-1632.218994f, 0.0f, -2974.588135f}};
   V3 five[3], left[4], right[4];
   from_text(fiveText, 3, 0, five);
   from_text(leftText, 4, 0, left);
   from_text(rightText, 4, 0, right);
   const ShapeView spa2[] = {{five, 3, 0, true, 0.0f}, {left, 4, 0, true, 0.0f}, {right, 4, 0, true, 0.0f}};
   assert(!unturn_shapes(spa2, 3));                                                     // a tie: as drawn
   assert(drawn_slots(left, 4, 0, true, 0.0f, false, 3, slots) == 3);
   for (int i = 0; i < 3; ++i) assert(slots[i].x < 0.0f && slots[i].z < 0.0f);         // left and behind
   assert(drawn_slots(right, 4, 0, true, 0.0f, false, 3, slots) == 3);
   for (int i = 0; i < 3; ++i) assert(slots[i].x > 0.0f && slots[i].z < 0.0f);         // right and behind

   // Cato Neimoidia (BF3): the same shapes turned 105 degrees. Turned back they are spa2's.
   const V3 catoFiveText[3] = {{-2074.438965f, 21.208265f, -1045.012817f}, {-2084.968262f, 21.208265f, -1051.860596f},
                               {-2078.992188f, 21.208265f, -1063.901733f}};
   const V3 catoLeftText[4] = {{-2104.013672f, 21.208265f, -1136.364258f}, {-2111.902344f, 21.208265f, -1121.847900f},
                               {-2117.888672f, 21.208265f, -1110.043091f}, {-2123.880371f, 21.208265f, -1097.738281f}};
   const V3 catoRightText[4] = {{-2129.026855f, 21.208265f, -1230.798706f}, {-2141.406738f, 21.208265f, -1237.934204f},
                                {-2155.437500f, 21.208265f, -1245.138550f}, {-2165.731445f, 21.208265f, -1251.167603f}};
   const V3 catoWallText[3] = {{-2139.280273f, 21.208265f, -1307.570312f}, {-2147.571289f, 32.149750f, -1302.051025f},
                               {-2157.229004f, 21.208265f, -1314.002197f}};
   const V3 catoBoxText[4] = {{-1947.796021f, -18.109297f, -1132.585083f}, {-1970.805908f, -18.109297f, -1127.366821f},
                              {-1977.229126f, -18.109297f, -1155.021362f}, {-1954.839722f, -18.109297f, -1159.071655f}};
   V3 cFive[3], cLeft[4], cRight[4], cWall[3], cBox[4];
   from_text(catoFiveText, 3, 0, cFive);
   from_text(catoLeftText, 4, 0, cLeft);
   from_text(catoRightText, 4, 0, cRight);
   from_text(catoWallText, 3, 0, cWall);
   from_text(catoBoxText, 4, 0, cBox);
   const float y105 = drawn_yaw(-0.0f, 0.793353f, -0.0f, 0.608761f);
   const ShapeView cato[] = {{cFive, 3, 0, true, y105}, {cLeft, 4, 0, true, y105}, {cRight, 4, 0, true, y105},
                             {cWall, 3, 0, true, y105}, {cBox, 4, 0, true, y105}};
   assert(unturn_shapes(cato, 5));
   const struct { const V3* cato; const V3* stock; int count; } same[] = {{cFive, five, 3}, {cLeft, left, 4}, {cRight, right, 4}};
   for (const auto& s : same) {
      const int n = drawn_slots(s.cato, s.count, 0, true, y105, true, s.count - 1, slots);
      assert(n == s.count - 1 && drawn_slots(s.stock, s.count, 0, true, 0.0f, false, s.count - 1, other) == n);
      for (int i = 0; i < n; ++i) assert(near(slots[i], other[i], 0.3f));
   }
   // As drawn but not turned back, Cato's echelon left would sit ahead of its leader.
   drawn_points(cLeft, 4, y105, false, pts);
   assert(!led_from_front(pts, 4, 0, true));
   // Bad input.
   assert(drawn_slots(nullptr, 3, 0, true, 0.0f, false, 2, slots) == 0 && drawn_slots(tri, 17, 0, true, 0.0f, false, 2, slots) == 0);
   assert(!led_from_front(pts, 1, 0, true) && !led_from_front(nullptr, 3, 0, true) && !led_from_front(pts, 3, 5, true));
   assert(!unturn_shapes(nullptr, 0));
}

// WingmanFormation names: in order, each once, a child's own replacing its parent's.
void formation_keys()
{
   ClassKeys k;
   assert(!sets_any(&k));
   add_formation(k, test_hash("Box-Form", 8));
   add_formation(k, test_hash("Tri-form", 8));
   add_formation(k, test_hash("Box-Form", 8));
   assert(k.hasFormations && k.formationCount == 2 && k.formations[0] == test_hash("box-form", 8) && sets_any(&k));
   ClassKeys child = inherited(k);
   assert(child.formationCount == 2);
   add_formation(child, test_hash("five-form", 9));
   assert(child.formationCount == 1 && child.formations[0] == test_hash("five-form", 9));
   ClassKeys many;
   for (uint32_t i = 1; i <= kMaxClassFormations + 3; ++i) add_formation(many, i);
   assert(many.formationCount == kMaxClassFormations);
   ClassKeys none;
   add_formation(none, 0);   // WingmanFormation = "": set, but naming none
   assert(none.hasFormations && none.formationCount == 0);

   // WingmanExitOnLand is about flying as a wingman: it gives a class no wingmen
   // of its own; with no key (or 1) the pilot gets out, with 0 it waits aboard.
   ClassKeys follower;
   assert(exits_on_land(&follower) && exits_on_land(nullptr));
   assert(set_exit_on_land(follower, " 0 ") && !exits_on_land(&follower) && !sets_any(&follower));
   assert(!inherited(follower).exitOnLand);
   assert(set_exit_on_land(follower, "1") && follower.hasExitOnLand && exits_on_land(&follower));
   assert(!set_exit_on_land(follower, "no") && exits_on_land(&follower));

   // WingmanLand, on the leader's class: over the INI where set, and like every
   // leader key it gives the class wingmen.
   ClassKeys leader;
   assert(lands_with(&leader, true) && !lands_with(&leader, false) && lands_with(nullptr, true));
   assert(set_land(leader, "0") && !lands_with(&leader, true) && sets_any(&leader));
   assert(set_land(leader, "1") && lands_with(&leader, false));
   assert(!set_land(leader, "") && lands_with(&leader, false));
   assert(inherited(leader).hasLand && inherited(leader).landWith);
}

// The log's numbers.
void stats()
{
   Stats s;
   s.add({3, 0, 4}, 80.0f, 75.0f);
   s.add({0, 0, -10}, 60.0f, 75.0f);
   s.add({kNan, 0, 0}, 1.0f, 1.0f);   // dropped
   assert(s.samples == 2 && near((float)s.distance, 15.0f) && near(s.worst, 10.0f));
   assert(near((float)s.ahead, -6.0f) && near((float)s.right, 3.0f) && near((float)s.speed, 140.0f));
}

} // namespace

// The orders: the squad key's turns, what an attack goes back to, how long a
// far recruit has, who takes on whom.
void orders()
{
   using namespace flyer_orders;
   assert(after_squad_key(Order::FormUp) == Order::Engage);
   assert(after_squad_key(Order::Engage) == Order::FormUp);
   assert(after_squad_key(Order::Attack) == Order::FormUp && after_squad_key(Order::Cover) == Order::FormUp);
   assert(resume_after_attack(Order::Cover, Order::FormUp) == Order::Cover);
   assert(resume_after_attack(Order::Engage, Order::Cover) == Order::Engage);
   assert(resume_after_attack(Order::Attack, Order::Cover) == Order::Cover);   // a second attack keeps the first's

   // A recruit has longer the further it starts, never past the cap.
   assert(join_seconds(0.0f) == kJoinBase);
   assert(join_seconds(1000.0f) > join_seconds(500.0f) && join_seconds(500.0f) > kJoinBase);
   assert(near((float)join_seconds(1000.0f), (float)(kJoinBase + 1000.0 / kJoinClosing)));
   assert(join_seconds(100000.0f) == kJoinMax && join_seconds(kNan) == kJoinMax && join_seconds(-5.0f) == kJoinMax);
   // A fighter 300 m/s fast over the leader's... one at 30 m/s over its leader
   // (the stretch) closes 2 km in 67 s, inside the 120 s it is given.
   assert(join_seconds(2000.0f) > 2000.0 / 30.0);

   // The nearest foe; in the air before the ground; spread before doubling up.
   Foe foes[3] = {{{0, 0, 300}, false, 0}, {{0, 0, 50}, true, 0}, {{0, 0, 500}, false, 0}};
   assert(pick_foe(V3{}, foes, 3) == 0);       // the ground one is nearer but costs kGroundCost more
   foes[0].taken = 1;
   assert(pick_foe(V3{}, foes, 3) == 2);       // 300 + 400 against 500
   foes[2].taken = 1;
   assert(pick_foe(V3{}, foes, 3) == 1);       // 50 + 600 against 700 and 900
   assert(pick_foe(V3{}, foes, 0) == -1 && pick_foe(V3{}, nullptr, 3) == -1);
   Foe bad[2] = {{{kNan, 0, 0}, false, 0}, {{0, 0, 900}, false, 0}};
   assert(pick_foe(V3{}, bad, 2) == 1);        // a foe with no position is passed over
   assert(near(foe_cost(V3{0, 0, 50}, Foe{{0, 0, 100}, true, 2}), 50.0f + kGroundCost + 2.0f * kSpreadCost));
}

// A flyer making passes at a target on the ground (turning at most 90 degrees a
// second at a steady speed): how many it makes in two minutes, how near it comes
// and how far it goes.
struct Passes {
   int   passes = 0;
   float closest = 1e9f, furthest = 0.0f;
};

Passes fly_passes(const flyer_orders::RunShape& shape, float speed, V3 start)
{
   using namespace flyer_orders;
   Passes out;
   V3 me = start, nose{0, 0, 1};
   const V3 target{};
   const float dt = 0.05f, turn = 1.5708f * dt;
   Run run;
   bool was = false;
   for (int step = 0; step < 2400; ++step) {   // 120 s
      V3 to = target - me;
      const float d = length(to);
      normalise(to);
      update_run(run, shape, d, dot(nose, to), step * (double)dt);
      if (run.extending && !was) ++out.passes;
      was = run.extending;
      V3 want = (run.extending ? extend_point(me, nose) : target) - me;
      normalise(want);
      // Turn the nose toward `want` by at most `turn`.
      const float c = clampf(dot(nose, want), -1.0f, 1.0f);
      const float angle = std::acos(c);
      if (angle > 1e-4f) {
         V3 side = want - nose * c;
         normalise(side);
         const float a = angle < turn ? angle : turn;
         nose = nose * std::cos(a) + side * std::sin(a);
         normalise(nose);
      }
      me = me + nose * (speed * dt);
      if (step > 400 && d > out.furthest) out.furthest = d;
      if (d < out.closest) out.closest = d;
   }
   return out;
}

// Passes at a target on the ground, shaped by the wingman's own weapon: in, out
// short of it, on and up, back in; and the gap it keeps behind a flyer.
void ground_runs()
{
   using namespace flyer_orders;
   // The shape, from the weapon's ranges and the speed, on one scale: in from
   // MaxRange, out just before MinRange or where it needs the room to pull out.
   const RunShape laser = run_shape(0.0f, 800.0f, 1600.0f, 60.0f);    // stock fighters' multilasers
   assert(near(laser.breakAt, 150.0f) && near(laser.extendTo, 1600.0f) && near(laser.gap, 120.0f));
   assert(near((float)laser.extendFor, 1450.0f / 60.0f + (float)kTurnBack, 1e-3f));
   const RunShape recoil = run_shape(0.0f, 300.0f, 600.0f, 40.0f);    // a gunship's recoilless cannon
   assert(near(recoil.breakAt, 100.0f) && near(recoil.extendTo, 600.0f) && near(recoil.gap, kMinGap));
   const RunShape chain = run_shape(0.0f, 200.0f, 400.0f, 80.0f);     // a chaingun on a fast ship: room to pull out
   assert(near(chain.breakAt, 200.0f) && near(chain.extendTo, 400.0f));
   const RunShape speeder = run_shape(0.0f, 100.0f, 200.0f, 60.0f);   // short reach: turning room sets the way out
   assert(near(speeder.breakAt, 150.0f) && near(speeder.extendTo, 270.0f));
   const RunShape torpedo = run_shape(200.0f, 800.0f, 1600.0f, 60.0f); // just before its MinRange
   assert(near(torpedo.breakAt, 220.0f) && near(torpedo.extendTo, 1600.0f));
   const RunShape bombs = run_shape(25.0f, 150.0f, 300.0f, 60.0f);     // a short MinRange: the room decides
   assert(near(bombs.breakAt, 150.0f));
   // No MaxRange: twice the optimal; no ranges at all: the default; a crawl counts
   // as kSlowestRun; a long reach keeps its gap at kMaxGap.
   assert(near(run_shape(0.0f, 500.0f, 0.0f, 60.0f).extendTo, 1000.0f));
   assert(near(run_shape(0.0f, kNan, kNan, 60.0f).extendTo, kDefaultRange));
   assert(near(run_shape(0.0f, 40.0f, 80.0f, 1.0f).breakAt, kSlowestRun * kPullOutSeconds));
   assert(near(run_shape(0.0f, 40.0f, 80.0f, kNan).breakAt, kSlowestRun * kPullOutSeconds));
   assert(near(run_shape(-50.0f, 800.0f, 1600.0f, 60.0f).breakAt, 150.0f));
   assert(near(run_shape(kNan, 800.0f, 1600.0f, 60.0f).breakAt, 150.0f));
   assert(near(run_shape(0.0f, 4000.0f, 8000.0f, 60.0f).gap, kMaxGap));

   Run r;
   update_run(r, laser, 2000.0f, 1.0f, 0.0);
   assert(!r.extending);
   update_run(r, laser, 1000.0f, 1.0f, 0.5);
   assert(!r.extending);                                  // diving in, inside where it turns back: in
   update_run(r, laser, 300.0f, 1.0f, 0.8);
   assert(!r.extending);                                  // well inside its optimal range: still in
   update_run(r, laser, 140.0f, 1.0f, 1.0);
   assert(r.extending && r.since == 1.0);
   update_run(r, laser, 1000.0f, -1.0f, 2.0);
   assert(r.extending);                                   // still pulling out
   update_run(r, laser, 1610.0f, -1.0f, 3.0);
   assert(!r.extending && r.since == 3.0);                // far enough: back in
   update_run(r, laser, 1200.0f, -0.5f, 4.0);
   assert(r.extending);                                   // past it, inside where it turns back
   update_run(r, laser, 500.0f, 1.0f, 4.0 + laser.extendFor + 0.1);
   assert(!r.extending);                                  // pulled out long enough
   update_run(r, laser, 1700.0f, -0.5f, 60.0);
   assert(!r.extending);                                  // behind it, but further than it turns back
   update_run(r, laser, kNan, 1.0f, 70.0);
   assert(!r.extending);
   // No distance changes nothing, even with the time out.
   Run out{true, 0.0};
   update_run(out, laser, kNan, 1.0f, 100.0);
   assert(out.extending && out.since == 0.0);
   // Pulling out: level along its heading and up, whatever its pitch.
   const V3 e = extend_point(V3{0, 50, 0}, V3{0, -0.8f, 0.6f});
   assert(near(e, V3{0, 50 + kRunClimb, kRunOut}, 1e-3f));
   assert(near(extend_point(V3{}, V3{0, -1, 0}), V3{0, kRunClimb, kRunOut}));   // straight down: ahead is +z

   // Flown: a fighter and a gunship keep coming back, pull out about where their
   // weapons say, and never run off. Measured: the fighter 4 passes in the 2
   // minutes, 54 to 1638 m from the target; the gunship 7, 34 to 625 m (both
   // passing over it).
   const Passes fighter = fly_passes(laser, 60.0f, V3{0, 300, -1500});
   assert(fighter.passes >= 3 && fighter.closest > 30.0f && fighter.furthest < laser.extendTo + 100.0f);
   const Passes gunship = fly_passes(recoil, 40.0f, V3{0, 150, -600});
   assert(gunship.passes >= 5 && gunship.closest > 20.0f && gunship.furthest < recoil.extendTo + 100.0f);
}

// Landing beside the leader: a slot's place on the ground, whether the ground
// there suits, when to come down, and the circle overhead.
void landing()
{
   using namespace flyer_orders;
   // Spread out, height left out, never nearer than the gap.
   assert(near(land_offset(V3{20, 5, -20}), V3{30, 0, -30}));
   assert(near(length(land_offset(V3{3, 0, -4})), kLandMinGap, 1e-3f));
   assert(near(land_offset(V3{}), V3{kLandMinGap, 0, 0}) && near(land_offset(V3{0, 9, 0}), V3{kLandMinGap, 0, 0}));
   assert(near(land_offset(V3{kNan, 0, 0}), V3{kLandMinGap, 0, 0}));
   // Level with the leader's ground (within 8 m) and flat (up 0.8 or more), or not.
   assert(level_spot(101.0f, 1.0f, 100.0f) && level_spot(93.0f, 0.82f, 100.0f) && level_spot(107.5f, 1.0f, 100.0f));
   assert(!level_spot(109.0f, 1.0f, 100.0f) && !level_spot(100.0f, 0.7f, 100.0f) && !level_spot(kNan, 1.0f, 100.0f));
   assert(!level_spot(91.0f, 1.0f, 100.0f));   // a hollow is no better than a rise
   // Down once near across (25 m) and not far up.
   assert(may_land(V3{20, 130, 0}, V3{0, 100, 0}) && !may_land(V3{30, 110, 0}, V3{0, 100, 0}));
   assert(!may_land(V3{0, 150, 0}, V3{0, 100, 0}) && !may_land(V3{0, 50, 0}, V3{0, 100, 0}));
   assert(!may_land(V3{kNan, 100, 0}, V3{0, 100, 0}));
   assert(near(across(V3{3, 50, 4}, V3{}), 5.0f));
   // The circle: on it, at the centre's height, ahead of where it is round it.
   const V3 c{100, 300, -50};
   for (const V3 me : {V3{300, 250, -50}, V3{100, 280, 400}, V3{-20, 310, -60}, V3{100, 300, -50}}) {
      const V3 p = loiter_point(c, me);
      assert(near(p.y, c.y) && near(across(p, c), kLoiterRadius, 1e-2f));
   }
   const V3 a = loiter_point(c, V3{c.x + kLoiterRadius, 0, c.z});   // at angle 0: the point at kLoiterLead
   assert(near(a.x, c.x + kLoiterRadius * std::cos(kLoiterLead), 1e-2f) &&
          near(a.z, c.z + kLoiterRadius * std::sin(kLoiterLead), 1e-2f));
   // A flyer following the point goes round and round, near the circle.
   V3 me{c.x + 400, c.y, c.z}, nose{0, 0, 1};
   const float dt = 0.05f, speed = 50.0f, turn = 1.5708f * dt;
   float angle = 0.0f, last = std::atan2(me.z - c.z, me.x - c.x), worst = 0.0f;
   for (int step = 0; step < 2400; ++step) {
      V3 want = loiter_point(c, me) - me;
      normalise(want);
      const float co = clampf(dot(nose, want), -1.0f, 1.0f);
      const float ang = std::acos(co);
      if (ang > 1e-4f) {
         V3 side = want - nose * co;
         normalise(side);
         const float t = ang < turn ? ang : turn;
         nose = nose * std::cos(t) + side * std::sin(t);
         normalise(nose);
      }
      me = me + nose * (speed * dt);
      const float now = std::atan2(me.z - c.z, me.x - c.x);
      float d = now - last;
      if (d > 3.14159f) d -= 6.28318f;
      if (d < -3.14159f) d += 6.28318f;
      angle += d;
      last = now;
      if (step > 600) worst = std::fmax(worst, std::fabs(across(me, c) - kLoiterRadius));
   }
   assert(angle > 3.0f * 6.28318f);   // round at least three times in the 2 minutes, one way
   assert(worst < 60.0f);
}

// Where a wingman tries to land, in order, and how high it comes down to under a
// roof: probed from 40 m up, a hangar's ground lies under its roof.
void landing_places()
{
   using namespace flyer_orders;
   V3 tries[kLandTries];
   // A vee slot back on the right: spread out, the slot's own distance, 0.6 of
   // it, the other side, then behind.
   const V3 slot{17.7f, 3.0f, -17.7f};
   assert(land_tries(slot, 0, tries) == kLandTries);
   assert(near(tries[0], land_offset(slot)));
   const float d = length(V3{slot.x, 0, slot.z});
   assert(near(length(tries[1]), d, 1e-3f) && near(length(tries[2]), 0.6f * d, 1e-3f));
   assert(near(tries[1].y, 0.0f) && near(tries[2].y, 0.0f));
   assert(tries[1].x > 0.0f && tries[1].z < 0.0f);                       // the same way ...
   assert(near(tries[1].x / tries[1].z, slot.x / slot.z, 1e-3f));
   assert(near(tries[3], V3{-tries[0].x, 0.0f, tries[0].z}));             // ... the other side ...
   assert(near(tries[4], V3{0.0f, 0.0f, -15.0f}));                        // ... then behind, 15 m ...
   assert(land_tries(slot, 2, tries) == kLandTries && near(tries[4], V3{0.0f, 0.0f, -39.0f}));   // ... 12 m more each
   assert(land_tries(slot, -3, tries) == kLandTries && near(tries[4], V3{0.0f, 0.0f, -15.0f}));
   // Nearer in, never nearer than 10 m; a slot on the leader goes to its right.
   assert(land_tries(V3{6, 0, -6}, 0, tries) == kLandTries);
   assert(near(length(tries[1]), 10.0f, 1e-3f) && near(length(tries[2]), 10.0f, 1e-3f));
   assert(land_tries(V3{}, 0, tries) == kLandTries && near(tries[1], V3{10.0f, 0, 0}));
   assert(land_tries(V3{kNan, 0, 0}, 0, tries) == kLandTries && near(tries[2], V3{10.0f, 0, 0}));
   // Spaced 12 m across from the places taken.
   const V3 taken[2] = {V3{0, 100, 0}, V3{30, 100, 0}};
   assert(spaced(V3{15, 90, 0}, taken, 2) && !spaced(V3{5, 100, 0}, taken, 2) && !spaced(V3{25, 100, 5}, taken, 2));
   assert(!spaced(V3{10, 100, 0}, taken, 2) && spaced(V3{0, 60, 12.5f}, taken, 2));
   assert(spaced(V3{0, 100, 0}, taken, 0) && spaced(V3{0, 100, 0}, nullptr, 2) && !spaced(V3{kNan, 0, 0}, taken, 1));
   // Probed from 40 m up, or 2 m under a roof, never under 3 m; hovering 15 m up,
   // or 4 m under a low roof, never under 4 m.
   assert(near(probe_height(kNoRoof), 40.0f) && near(probe_height(100.0f), 40.0f));
   assert(near(probe_height(40.0f), 38.0f));   // a roof 40 m up still counts
   assert(near(probe_height(12.0f), 10.0f) && near(probe_height(4.0f), 3.0f) && near(probe_height(kNan), 40.0f));
   assert(near(hover_height(kNoRoof), 15.0f) && near(hover_height(12.0f), 8.0f));
   assert(near(hover_height(6.0f), 4.0f) && near(hover_height(kNan), 15.0f));

   // On ahead: 12 m to its slot's side, from 15 m ahead every 12 m, 16 places to 195 m.
   V3 ahead[kLandAhead];
   assert(forward_tries(V3{-25.0f, 0.0f, -25.0f}, 0, ahead) == 16);
   assert(near(ahead[0].x, -12.0f) && near(ahead[0].z, 15.0f) && near(ahead[1].z, 27.0f) && near(ahead[0].y, 0.0f));
   assert(near(ahead[15].x, -12.0f) && near(ahead[15].z, 195.0f));
   assert(forward_tries(V3{25.0f, 0.0f, -25.0f}, 1, ahead) == 16 && near(ahead[3].x, 12.0f));
   assert(forward_tries(V3{0.0f, 0.0f, -25.0f}, 0, ahead) == 16 && near(ahead[0].x, 12.0f));    // on the line: by rank
   assert(forward_tries(V3{0.0f, 0.0f, -25.0f}, 1, ahead) == 16 && near(ahead[0].x, -12.0f));
   assert(forward_tries(V3{kNan, 0.0f, 0.0f}, 2, ahead) == 16 && near(ahead[0].x, 12.0f));
   assert(kLandAheadSide == 12.0f && kLandLookAgain == 2.0);
   // Circling under a roof: the room every way less 5 m, 15 to 30 m round; else no room.
   assert(near(roof_loiter_radius(50.0f), 30.0f) && near(roof_loiter_radius(35.0f), 30.0f));
   assert(near(roof_loiter_radius(30.0f), 25.0f) && near(roof_loiter_radius(20.0f), 15.0f));
   assert(roof_loiter_radius(19.9f) < 0.0f && roof_loiter_radius(kNan) < 0.0f && roof_loiter_radius(-5.0f) < 0.0f);
   // A circle's point at a radius of its own.
   const V3 tight = loiter_point(V3{10.0f, 5.0f, 10.0f}, V3{40.0f, 5.0f, 10.0f}, 20.0f);
   assert(near(across(tight, V3{10.0f, 5.0f, 10.0f}), 20.0f) && near(tight.y, 5.0f));
   assert(near(across(loiter_point(V3{}, V3{1.0f, 0.0f, 0.0f}), V3{}), kLoiterRadius));   // the open sky's
}

// The keys and words a mission sets: orders by name, the order a flight starts
// with, a wingman's role, how far engaging reaches, who may fly as a wingman,
// and how many are called up on their own.
void mission_keys()
{
   using namespace flyer_orders;
   // Names: case, quotes, spaces, '_' and '-' ignored; nothing more, nothing less.
   assert(word_is("Form Up", "formup") && word_is("form_up", "formup") && word_is("FORMUP", "formup"));
   assert(word_is("\"engage\"", "engage") && word_is(" cover-me ", "coverme"));
   assert(!word_is("engaged", "engage") && !word_is("engag", "engage") && !word_is("", "formup"));
   assert(!word_is(nullptr, "formup") && !word_is("formup", nullptr));
   Order o = Order::Engage;
   assert(order_from("formup", o) && o == Order::FormUp);
   assert(order_from("Engage", o) && o == Order::Engage);
   assert(order_from("cover", o) && o == Order::Cover);
   o = Order::FormUp;
   assert(order_from("Cover Me", o) && o == Order::Cover);
   assert(!order_from("attack", o) && o == Order::Cover);   // only where an attack is allowed; o kept
   assert(order_from("attack", o, true) && o == Order::Attack);
   assert(order_from("attack my target", o, true) && o == Order::Attack);
   assert(!order_from("retreat", o, true) && !order_from(nullptr, o, true));
   for (Order each : {Order::FormUp, Order::Engage, Order::Attack, Order::Cover}) {
      Order back = Order::FormUp;
      assert(order_from(order_word(each), back, true) && back == each);
   }
   assert(std::string(order_word(Order::Cover)) == "cover" && std::string(order_word(Order::FormUp)) == "formup");

   // WingmanStartOrder: form up, engage or cover, never attack; a leader key.
   ClassKeys leader;
   assert(start_order(&leader) == Order::FormUp && start_order(nullptr) == Order::FormUp);
   assert(set_start_order(leader, "engage") && start_order(&leader) == Order::Engage && sets_any(&leader));
   assert(!set_start_order(leader, "attack") && start_order(&leader) == Order::Engage);
   assert(set_start_order(leader, "Cover Me") && start_order(&leader) == Order::Cover);
   const ClassKeys child = flyer_wingmen::inherited(leader);
   assert(start_order(&child) == Order::Cover);

   // WingmanEngageRange: in place of the 800 m, clamped, the chase a quarter further.
   ClassKeys ranged;
   assert(engage_range(&ranged) == kEngageRange && engage_range(nullptr) == kEngageRange);
   assert(near(engage_keep(kEngageRange), kEngageKeep));
   assert(set_engage_range(ranged, "1500") && near(engage_range(&ranged), 1500.0f) && sets_any(&ranged));
   assert(near(engage_keep(engage_range(&ranged)), 1875.0f));
   assert(set_engage_range(ranged, "20") && near(engage_range(&ranged), 100.0f));
   assert(set_engage_range(ranged, "99999") && near(engage_range(&ranged), 5000.0f));
   assert(!set_engage_range(ranged, "far") && near(engage_range(&ranged), 5000.0f));

   // WingmanRole: a wingman key; a bomber goes for the ground first.
   ClassKeys bomber;
   assert(role_of(&bomber) == Role::Fighter && role_of(nullptr) == Role::Fighter);
   assert(set_role(bomber, "Bomber") && role_of(&bomber) == Role::Bomber && !sets_any(&bomber));
   assert(!set_role(bomber, "tank") && role_of(&bomber) == Role::Bomber);
   assert(set_role(bomber, "fighter") && role_of(&bomber) == Role::Fighter);
   Foe foes[2] = {{{0, 0, 300}, false, 0}, {{0, 0, 700}, true, 0}};
   assert(pick_foe(V3{}, foes, 2, Role::Fighter) == 0);   // 300 against 700 + 600
   assert(pick_foe(V3{}, foes, 2, Role::Bomber) == 1);    // 300 + 600 against 700
   assert(pick_foe(V3{}, foes, 2) == 0);                  // a fighter's, unless it says
   assert(near(foe_cost(V3{}, foes[0], Role::Bomber), 300.0f + kGroundCost));
   assert(near(foe_cost(V3{}, foes[1], Role::Bomber), 700.0f));
   foes[1].taken = 1;
   assert(pick_foe(V3{}, foes, 2, Role::Bomber) == 0);    // 900 against 1100: spread first

   // Wingman = 0: never called up; a wingman key.
   ClassKeys hero;
   assert(may_follow(&hero) && may_follow(nullptr));
   assert(set_wingman(hero, "0") && !may_follow(&hero) && !sets_any(&hero));
   assert(!flyer_wingmen::inherited(hero).mayFollow);
   assert(set_wingman(hero, "1") && may_follow(&hero));
   assert(!set_wingman(hero, "never") && may_follow(&hero));

   // How many are called up on their own: a script's number where it gave one
   // (0 to 8), else the class's.
   ClassKeys four;
   assert(set_count(four, "4"));
   assert(flight_size(-1, &four, 3) == 4 && flight_size(-1, nullptr, 3) == 3);
   assert(flight_size(2, &four, 3) == 2 && flight_size(0, &four, 3) == 0);
   assert(flight_size(20, &four, 3) == kMaxWingmen);
}

// The squad key's hold (it breaks the flight off after a second, once a press,
// as the trooper squad key's hold lets followers go), when losses are made up,
// whom an aimed squad key picks, and the order a flight called up after a break
// off starts with.
void squad_key_rules()
{
   using namespace flyer_orders;
   SquadHold h;
   // Pressed at 10 s and held: it breaks off at 11 s, once.
   assert(!squad_hold(h, true, true, 10.0));
   assert(!squad_hold(h, true, false, 10.5) && !squad_hold(h, true, false, 10.99));
   assert(squad_hold(h, true, false, 11.0));
   assert(!squad_hold(h, true, false, 11.5) && !squad_hold(h, true, false, 20.0));   // once a press
   // Let go: a new hold next time, and a tap breaks nothing off.
   assert(!squad_hold(h, false, false, 21.0) && h.since < 0.0 && !h.fired);
   assert(!squad_hold(h, true, true, 22.0) && !squad_hold(h, true, false, 22.5) && !squad_hold(h, false, false, 22.6));
   assert(!squad_hold(h, true, false, 30.0));   // down again, its press unseen: it counts from here
   assert(squad_hold(h, true, false, 31.0));
   // A press while it seemed held (let go and pressed between looks) counts from that press.
   SquadHold g;
   assert(!squad_hold(g, true, true, 0.0) && !squad_hold(g, true, false, 0.9));
   assert(!squad_hold(g, true, true, 0.95) && !squad_hold(g, true, false, 1.5) && squad_hold(g, true, false, 1.95));
   // Broken off, then let go and pressed again between looks: the new hold breaks off again.
   SquadHold r;
   assert(!squad_hold(r, true, true, 0.0) && squad_hold(r, true, false, 1.0));
   assert(!squad_hold(r, true, true, 5.0) && squad_hold(r, true, false, 6.0));
   SquadHold n;
   assert(!squad_hold(n, true, true, 0.0) && !squad_hold(n, true, false, kNan));
   // Losses made up: called up, or WingmanAuto; never locked, broken off or picked by hand.
   assert(tops_up(false, false, false, true, false) && tops_up(false, false, false, false, true));
   assert(!tops_up(false, false, false, false, false));
   assert(!tops_up(true, false, false, true, true) && !tops_up(false, true, false, true, true) &&
          !tops_up(false, false, true, true, true));
   // An aimed squad key: no friendly AI flyer there, or one of the flight, and the key does what it does.
   assert(pick_verdict(false, false, false, true, true) == Pick::None);
   assert(pick_verdict(true, true, false, true, true) == Pick::None);
   assert(pick_verdict(true, false, false, true, true) == Pick::Join);
   assert(pick_verdict(true, false, true, true, true) == Pick::Refuse);    // the mission locked the flight
   assert(pick_verdict(true, false, false, false, true) == Pick::Refuse);  // it may not fly with this leader
   assert(pick_verdict(true, false, false, true, false) == Pick::Refuse);  // no room
   // Called up after a break off: form up, whatever the flight starts with otherwise.
   assert(call_up_order(true, Order::Engage) == Order::FormUp && call_up_order(false, Order::Engage) == Order::Engage);
   assert(call_up_order(false, Order::Cover) == Order::Cover);
}

// A pass's line of sight and its target's open side (a hardpoint under a
// capital ship's hull): the approach directions, which side is open, being on
// it, the way round to it, pulling out along any side, and a blocked run
// broken off.
void attack_sides()
{
   using namespace flyer_orders;
   for (int k = 0; k < kApproachDirs; ++k) {
      assert(near(length(approach_dir(k)), 1.0f, 1e-4f));
      for (int j = 0; j < k; ++j) assert(!near(approach_dir(k), approach_dir(j)));
   }
   assert(kApproachDirs == 14 && near(approach_dir(0), V3{0, 1, 0}) && near(approach_dir(1), V3{0, -1, 0}));
   // All of it open, 500 m in along each: up from above or from on top of it,
   // and up a little before a corner as near the wingman.
   float clear[kApproachDirs];
   for (float& c : clear) c = 500.0f;
   assert(pick_open(clear, V3{0, 300, 50}) == 0 && pick_open(clear, V3{}) == 0);
   assert(pick_open(clear, V3{0.6f, 0.8f, 0}) == 0);
   clear[5] = 499.0f;
   assert(pick_open(clear, V3{0, 0, -300}) != 5);   // not quite open
   // Under a hull: up and the upper corners shut, the rest open. From below and
   // to one side, that side; from straight below, down; from above the ship, the
   // first side as near.
   for (int k = 0; k < kApproachDirs; ++k) clear[k] = approach_dir(k).y > 0.1f ? 40.0f : 500.0f;
   assert(pick_open(clear, V3{300, -200, 0}) == 2);
   assert(pick_open(clear, V3{0, -300, 0}) == 1);
   assert(pick_open(clear, V3{0, 300, 0}) == 2);
   // None open: the clearest, NaN never.
   for (int k = 0; k < kApproachDirs; ++k) clear[k] = 10.0f * (float)k;
   assert(pick_open(clear, V3{0, 300, 0}) == kApproachDirs - 1);
   clear[0] = kNan;
   clear[kApproachDirs - 1] = kNan;
   assert(pick_open(clear, V3{0, 300, 0}) == kApproachDirs - 2);
   for (float& c : clear) c = kNan;
   assert(pick_open(clear, V3{0, 300, 0}) == 0);
   // On the open side: within 60 degrees of it, seen from the target.
   const V3 down{0, -1, 0};
   assert(on_open_side(V3{0, -300, 0}, V3{}, down) && !on_open_side(V3{0, 300, 0}, V3{}, down));
   assert(on_open_side(V3{0.819f, -0.574f, 0}, V3{}, down));    // 55 degrees off
   assert(!on_open_side(V3{0.906f, -0.423f, 0}, V3{}, down));   // 65 degrees off
   assert(on_open_side(V3{5, 5, 5}, V3{5, 5, 5}, down));        // on top of it
   assert(near(setup_point(V3{10, 20, 30}, down), V3{10, -380, 30}));
   // The way round: the nearest point it reaches that sees the setup point.
   bool reach[kApproachDirs] = {}, sees[kApproachDirs] = {};
   float dist[kApproachDirs];
   for (int k = 0; k < kApproachDirs; ++k) dist[k] = 100.0f + (float)k;
   assert(pick_via(reach, sees, dist) == -1);
   reach[3] = sees[3] = true;
   reach[5] = sees[5] = true;
   reach[2] = true;
   sees[4] = true;
   assert(pick_via(reach, sees, dist) == 3);
   dist[2] = dist[4] = 1.0f;   // nearer, but each only one of the two
   assert(pick_via(reach, sees, dist) == 3);
   dist[5] = 50.0f;
   assert(pick_via(reach, sees, dist) == 5);
   dist[3] = kNan;
   dist[5] = 60.0f;
   assert(pick_via(reach, sees, dist) == 5);   // NaN never
   dist[5] = kNan;
   assert(pick_via(reach, sees, dist) == -1);
   // Else out across the open side at its own height; right over the target,
   // along its heading flattened; straight up its nose, still across.
   assert(near(escape_point(V3{100, 300, 0}, V3{}, down, V3{0, 0, 1}), V3{500, 300, 0}));
   assert(near(escape_point(V3{0, 300, 0}, V3{}, down, V3{0, 0.6f, 0.8f}), V3{0, 300, 400}));
   const V3 upNose = escape_point(V3{0, 300, 0}, V3{}, down, V3{0, 1, 0});
   assert(near(upNose.y, 300.0f) && near(length(upNose - V3{0, 300, 0}), 400.0f, 1e-2f));
   // Pulling out along a side; up is the pull-out passes at the ground make.
   assert(near(extend_point_along(V3{}, V3{0, 0, 1}, V3{0, 1, 0}), extend_point(V3{}, V3{0, 0, 1})));
   assert(near(extend_point_along(V3{0, -300, 0}, V3{0, 0.6f, 0.8f}, down), V3{0, -360, 300}));
   const V3 outNose = extend_point_along(V3{}, down, down);
   assert(near(outNose.y, -60.0f) && near(length(V3{outNose.x, 0, outNose.z}), 300.0f, 1e-2f));
   // A run with its line blocked breaks off inside where it would turn back; not
   // further out, nor with the line clear.
   const RunShape s = run_shape(0.0f, 800.0f, 1600.0f, 60.0f);
   Run r{false, 0.0};
   update_run(r, s, 1000.0f, 1.0f, 1.0, true);
   assert(r.extending && r.since == 1.0);
   Run far{false, 0.0};
   update_run(far, s, s.extendTo + 100.0f, 1.0f, 1.0, true);
   assert(!far.extending);
   Run seen{false, 0.0};
   update_run(seen, s, 1000.0f, 1.0f, 1.0, false);
   assert(!seen.extending);
   assert(kSightShort == 10.0f);
}

// Landing right away (the user: "When I land, AI should land right away"), and a
// landing interrupted: onto a flyer or vehicle, not down in time, or down on top
// of one.
void landing_now()
{
   using namespace flyer_orders;
   const float inf = std::numeric_limits<float>::infinity();
   // Near, ground 60 m down, level with the leader's and flat, nothing on it: here.
   assert(land_here(true, 60.0f, 101.0f, 1.0f, 100.0f, inf));
   assert(!land_here(false, 60.0f, 101.0f, 1.0f, 100.0f, inf));    // not near its leader
   assert(!land_here(true, inf, 101.0f, 1.0f, 100.0f, inf));       // no ground within reach
   assert(!land_here(true, 201.0f, 101.0f, 1.0f, 100.0f, inf));    // ground past 200 m
   assert(land_here(true, 200.0f, 101.0f, 1.0f, 100.0f, inf));
   assert(!land_here(true, 60.0f, 120.0f, 1.0f, 100.0f, inf));     // not level with the leader's
   assert(!land_here(true, 60.0f, 101.0f, 0.5f, 100.0f, inf));     // a slope
   assert(!land_here(true, 60.0f, 101.0f, 1.0f, 100.0f, 30.0f));   // a ship on the way down
   assert(land_here(true, 60.0f, 101.0f, 1.0f, 100.0f, 80.0f));    // one under the ground (a deck below)
   assert(!land_here(true, kNan, 101.0f, 1.0f, 100.0f, inf));
   // Coming down onto something; not down after 8 s; down on top of something.
   assert(landing_interrupted(true, false, 1.0, 50.0f, 20.0f) == Interrupt::Onto);
   assert(landing_interrupted(true, false, 1.0, 50.0f, inf) == Interrupt::None);
   assert(landing_interrupted(true, false, 1.0, 50.0f, 60.0f) == Interrupt::None);   // past the ground
   assert(landing_interrupted(true, false, 8.5, 50.0f, inf) == Interrupt::Stuck);
   assert(landing_interrupted(true, false, 7.5, 50.0f, inf) == Interrupt::None);
   assert(landing_interrupted(true, false, 9.0, inf, inf) == Interrupt::Stuck);       // nothing under it at all
   assert(landing_interrupted(false, true, 20.0, 4.0f, 1.5f) == Interrupt::OnTop);
   assert(landing_interrupted(false, true, 20.0, 1.0f, inf) == Interrupt::None);      // down on the ground
   assert(landing_interrupted(false, false, 20.0, 50.0f, 1.0f) == Interrupt::None);   // flying
   assert(landing_interrupted(true, false, 1.0, kNan, 1.0f) == Interrupt::None);
   assert(kLandRetries == 2 && kLandNowRange == 150.0f);
   // Coming in to its place: cruising until 120 m off, easing to its class's
   // lowest speed by 25 m, its slowest within 25 m.
   assert(approach_speed(200.0f, 10.0f, 20.0f, 50.0f) == 50.0f && approach_speed(120.0f, 10.0f, 20.0f, 50.0f) == 50.0f);
   assert(near(approach_speed(72.5f, 10.0f, 20.0f, 50.0f), 35.0f) && near(approach_speed(119.0f, 10.0f, 20.0f, 50.0f), 20.0f + 30.0f * (94.0f / 95.0f)));
   assert(approach_speed(25.0f, 10.0f, 20.0f, 50.0f) == 10.0f && approach_speed(0.0f, 10.0f, 20.0f, 50.0f) == 10.0f);
   assert(near(approach_speed(26.0f, 10.0f, 20.0f, 50.0f), 20.0f + 30.0f / 95.0f));
   assert(approach_speed(kNan, 10.0f, 20.0f, 50.0f) == 10.0f && kLandEaseFrom == 120.0f && kLandStart == 25.0f);
   // With the player out: 60 s for those still on their way.
   assert(!arrival_over(0.0) && !arrival_over(59.5) && !arrival_over(kArriveFor));
   assert(arrival_over(60.5) && arrival_over(kNan) && kArriveFor == 60.0);
}

// The leader's trail: a point every 8 m it flies, the last 256, started again
// past a jump; where a wingman is on it, where it may cut to, its place in the
// line, and the slots closing up under a roof.
void trail()
{
   using namespace flyer_trail;
   Trail t;
   assert(t.count == 0 && nearest(t, V3{}, -1) == -1 && cut_candidates(t, 0, nullptr) == 0);
   assert(leader_along(t, V3{5.0f, 0.0f, 0.0f}) == 0.0f);
   // The first point at once; then only once 8 m on.
   assert(record(t, V3{0.0f, 10.0f, 0.0f}) && t.count == 1 && newest(t) == 0 && along(t, 0) == 0.0f);
   assert(!record(t, V3{7.9f, 10.0f, 0.0f}) && t.count == 1);
   assert(record(t, V3{8.0f, 10.0f, 0.0f}) && t.count == 2 && near(along(t, 1), 8.0f));
   assert(!record(t, V3{kNan, 10.0f, 0.0f}) && t.count == 2);
   for (int i = 2; i < 40; ++i) assert(record(t, V3{8.0f * (float)i, 10.0f, 0.0f}));
   assert(t.count == 40 && newest(t) == 39 && near(along(t, 39), 312.0f) && near(point(t, 39).x, 312.0f));
   // The leader 5 m past its newest point; a wingman three spaces of 25 m back
   // is at its place as the second in line.
   const float lead = leader_along(t, V3{317.0f, 10.0f, 0.0f});
   assert(near(lead, 317.0f));
   assert(near(behind_place(t, lead, 0, 25.0f, 36), 317.0f - 25.0f - 288.0f));   // the first: 4 m behind its place
   assert(near(behind_place(t, lead, 1, 25.0f, 33), 317.0f - 50.0f - 264.0f));   // the second: 3 m behind
   assert(behind_place(t, lead, 0, 25.0f, 39) < 0.0f);                          // ahead of it: slows
   assert(behind_place(t, lead, 0, 25.0f, 99) == 0.0f && behind_place(t, kNan, 0, 25.0f, 36) == 0.0f);
   // Its point: the nearest from its own on, never back; the whole trail with none.
   float d = 0.0f;
   assert(nearest(t, V3{97.0f, 13.0f, 0.0f}, -1, &d) == 12 && near(d, std::sqrt(10.0f)));
   assert(nearest(t, V3{100.0f, 13.0f, 0.0f}, -1) == 13);     // as near as 12: the newer
   assert(nearest(t, V3{100.0f, 13.0f, 0.0f}, 20) == 20);     // not back past its own
   assert(nearest(t, V3{kNan, 0.0f, 0.0f}, -1) == -1);
   Trail longer;
   for (int i = 0; i < 100; ++i) record(longer, V3{8.0f * (float)i, 0.0f, 0.0f});
   assert(nearest(longer, V3{720.0f, 0.0f, 0.0f}, 0) == kWindow);   // looked for kWindow on at most
   assert(nearest(longer, V3{720.0f, 0.0f, 0.0f}, 40) == 90 && nearest(longer, V3{720.0f, 0.0f, 0.0f}, -1) == 90);
   // Where it may cut to: 16, 8, 4 and 2 points on, never past the newest.
   int cuts[kMaxCuts];
   assert(cut_candidates(t, 10, cuts) == 4 && cuts[0] == 26 && cuts[1] == 18 && cuts[2] == 14 && cuts[3] == 12);
   assert(cut_candidates(t, 34, cuts) == 3 && cuts[0] == 39 && cuts[1] == 38 && cuts[2] == 36);
   assert(cut_candidates(t, 38, cuts) == 1 && cuts[0] == 39);
   assert(cut_candidates(t, 39, cuts) == 0 && cut_candidates(t, 40, cuts) == 0);
   assert(next_of(t, 10) == 11 && next_of(t, 39) == 39);
   // In and out again the same way: of points as near, the newest (the way out).
   Trail back;
   for (int i = 0; i <= 10; ++i) record(back, V3{8.0f * (float)i, 0.0f, 0.0f});
   for (int i = 9; i >= 0; --i) record(back, V3{8.0f * (float)i, 1.0f, 0.0f});
   assert(back.count == 21 && nearest(back, V3{40.0f, 0.4f, 0.0f}, -1) == 15);
   // The way out: back along it; of points as near, the oldest (the way in);
   // never forward past its own; kWindow back at most.
   assert(nearest_back(back, V3{40.0f, 0.4f, 0.0f}, -1, &d) == 5 && near(d, 0.4f));
   assert(nearest_back(back, V3{40.0f, 0.6f, 0.0f}, -1, &d) == 5 && near(d, 0.6f));   // the way in, though a bit further
   assert(nearest_back(back, V3{40.0f, 0.4f, 0.0f}, 20) == 5);
   assert(nearest_back(back, V3{80.0f, 0.0f, 0.0f}, 4) == 4);
   assert(nearest_back(longer, V3{0.0f, 0.0f, 0.0f}, 99) == 99 - kWindow);
   assert(nearest_back(t, V3{kNan, 0.0f, 0.0f}, -1) == -1 && nearest_back(Trail{}, V3{}, -1) == -1);
   // Through a hangar's mouth: to the middle between two walls within 40 m;
   // away from one nearer than it needs; nothing else moves it.
   assert(near(centre_offset(10.0f, 30.0f, 8.0f), 10.0f) && near(centre_offset(30.0f, 10.0f, 8.0f), -10.0f));
   assert(near(centre_offset(39.0f, 39.5f, 8.0f), 0.25f));
   assert(near(centre_offset(5.0f, kInf, 8.0f), 3.0f) && near(centre_offset(kInf, 5.0f, 8.0f), -3.0f));
   assert(centre_offset(20.0f, kInf, 8.0f) == 0.0f && centre_offset(kInf, 20.0f, 8.0f) == 0.0f);
   assert(centre_offset(kInf, kInf, 8.0f) == 0.0f && centre_offset(40.0f, 5.0f, 8.0f) == -3.0f);
   assert(centre_offset(kNan, 10.0f, 8.0f) == 0.0f && centre_offset(kNan, kNan, 8.0f) == 0.0f);
   // Through the door: cruising until 50 m short of it, easing to its class's
   // lowest speed at it, that inside; with no door, cruising.
   assert(door_speed(60.0f, 20.0f, 80.0f) == 80.0f && door_speed(50.0f, 20.0f, 80.0f) == 80.0f);
   assert(near(door_speed(25.0f, 20.0f, 80.0f), 50.0f) && near(door_speed(49.0f, 20.0f, 80.0f), 78.8f));
   assert(door_speed(0.0f, 20.0f, 80.0f) == 20.0f && door_speed(-30.0f, 20.0f, 80.0f) == 20.0f);
   assert(door_speed(kNan, 20.0f, 80.0f) == 80.0f);
   assert(kFitMargin == 2.0f && kCentreReach == 40.0f && kDoorEase == 50.0f);
   // Where it may cut back to: 16, 8, 4 and 2 points back, never before the oldest.
   assert(back_candidates(back, 20, cuts) == 4 && cuts[0] == 4 && cuts[1] == 12 && cuts[2] == 16 && cuts[3] == 18);
   assert(back_candidates(back, 3, cuts) == 2 && cuts[0] == 0 && cuts[1] == 1);
   assert(back_candidates(back, 0, cuts) == 0 && back_candidates(back, 21, cuts) == 0);
   assert(prev_of(back, 10) == 9 && prev_of(back, 0) == 0);
   // Out once no roof is over it two looks in a row.
   int outLooks = 0;
   assert(!out_clear(outLooks, false) && out_clear(outLooks, false));
   assert(!out_clear(outLooks, true) && outLooks == 0 && !out_clear(outLooks, false) && out_clear(outLooks, false));
   assert(kOutLooks == 2 && kOutJoin == 2.0 && kOutMax == 45.0);
   // A jump is not flown: it starts again, its numbers after every earlier one.
   const int was = newest(t);
   assert(record(t, V3{312.0f, 10.0f, 150.0f}) && t.count == 1 && newest(t) == was + 1 && along(t, newest(t)) == 0.0f);
   assert(!has(t, was) && has(t, was + 1));
   clear(t);
   assert(t.count == 0 && !has(t, was + 1) && record(t, V3{}) && newest(t) == was + 2);
   // The last 256 kept, the oldest making way.
   Trail ring;
   for (int i = 0; i < kPoints + 10; ++i) record(ring, V3{8.0f * (float)i, 0.0f, 0.0f});
   assert(ring.count == kPoints && ring.first == 10 && newest(ring) == kPoints + 9);
   assert(!has(ring, 9) && has(ring, 10) && near(point(ring, 10).x, 80.0f) && near(along(ring, kPoints + 9),
                                                                                   8.0f * (float)(kPoints + 9)));
   assert(nearest(ring, V3{}, -1) == 10);   // the oldest kept
   assert(cut_candidates(ring, 5, cuts) == 0 && nearest(ring, V3{}, 5) == 10);   // a point no longer kept
   // A jump of exactly kJump starts it again too.
   Trail jump;
   record(jump, V3{});
   record(jump, V3{8.0f, 0.0f, 0.0f});
   assert(record(jump, V3{8.0f, 0.0f, kJump}) && jump.count == 1 && newest(jump) == 2);
   assert(record(jump, V3{8.0f, 0.0f, kJump + 99.0f}) && jump.count == 2 && near(along(jump, 3), 99.0f));
   // Taking it, and leaving it after three clear looks in a row.
   assert(takes(true, 299.0f) && takes(true, kNear) && !takes(true, 301.0f) && !takes(false, 10.0f) && !takes(true, kNan));
   int clearLooks = 0;
   assert(!leaves(clearLooks, true) && !leaves(clearLooks, true) && !leaves(clearLooks, false) && clearLooks == 0);
   assert(!leaves(clearLooks, true) && !leaves(clearLooks, true) && leaves(clearLooks, true));
   // Stuck: no further point for 6 s with its place a gap or more ahead.
   assert(stuck(6.5, 30.0f, 25.0f) && !stuck(5.5, 30.0f, 25.0f) && !stuck(6.5, 20.0f, 25.0f) && !stuck(6.5, -5.0f, 25.0f));
   // Under a roof the slots close up to half their spacing, half of it a second.
   assert(near(squeeze(1.0f, true, 0.5f), 0.75f) && near(squeeze(0.75f, true, 1.0f), 0.5f));
   assert(near(squeeze(0.5f, true, 1.0f), 0.5f) && near(squeeze(0.5f, false, 0.2f), 0.6f));
   assert(near(squeeze(0.9f, false, 5.0f), 1.0f) && near(squeeze(1.0f, true, 5.0f), 0.5f));   // no further
   assert(near(squeeze(1.0f, true, kNan), 1.0f) && near(squeeze(1.0f, true, -1.0f), 1.0f));
   assert(near(squeeze(kNan, true, 0.0f), 1.0f) && near(squeeze(0.2f, false, 0.0f), 1.0f));   // out of range: open
   // The game's lift off: under a roof itself; or the leader under one, its line
   // clear as looked within a second.
   assert(lift_off(true, false, true, 99.0) && lift_off(true, true, true, kNan));
   assert(lift_off(false, true, false, 0.5) && lift_off(false, true, false, kFreshLook) && lift_off(false, true, false, 0.0));
   assert(!lift_off(false, true, true, 0.5));     // a wall in its way: the lift stays
   assert(!lift_off(false, true, false, 1.5));    // an old look says nothing
   assert(!lift_off(false, true, false, -1.0) && !lift_off(false, true, false, kNan));
   assert(!lift_off(false, false, false, 0.5));   // open sky
   assert(kStep == 8.0f && kPoints == 256 && kLookEvery == 0.25 && kLeaveAfter == 3 && kSqueeze == 0.5f && kFreshLook == 1.0);
}

// A map's landing regions: each shape the game knows, inside or not as its
// IsPointInside says, and its top.
void landing_regions()
{
   using namespace landing_region;
   Region box;   // half sizes 20, 10, 40 at (100, 50, 0), lined up with the world
   box.shape = kBox;
   box.at = V3{100.0f, 50.0f, 0.0f};
   box.x = 20.0f;
   box.y = 10.0f;
   box.z = 40.0f;
   assert(inside(box, V3{100.0f, 50.0f, 0.0f}) && inside(box, V3{119.0f, 59.0f, 39.0f}));
   assert(!inside(box, V3{121.0f, 50.0f, 0.0f}) && !inside(box, V3{100.0f, 60.0f, 0.0f}));
   assert(!inside(box, V3{100.0f, 50.0f, -40.0f}) && near(top(box), 60.0f));
   // Turned a quarter round: its right the world's z, its forward the world's -x.
   Region turned = box;
   turned.right = V3{0.0f, 0.0f, 1.0f};
   turned.fwd = V3{-1.0f, 0.0f, 0.0f};
   assert(inside(turned, V3{135.0f, 50.0f, 15.0f}) && !inside(turned, V3{125.0f, 50.0f, 25.0f}));
   assert(near(top(turned), 60.0f));
   // Tipped over (its up the world's z): its top is its forward half size up.
   Region tipped = box;
   tipped.up = V3{0.0f, 0.0f, 1.0f};
   tipped.fwd = V3{0.0f, -1.0f, 0.0f};
   assert(near(top(tipped), 90.0f));
   // A sphere, radius w.
   Region ball;
   ball.shape = kSphere;
   ball.at = V3{0.0f, 10.0f, 0.0f};
   ball.w = 5.0f;
   assert(inside(ball, V3{0.0f, 14.9f, 0.0f}) && !inside(ball, V3{0.0f, 15.0f, 0.0f}) && near(top(ball), 15.0f));
   // A cylinder upright, radius x, half height y; then on its side, its top its rim.
   Region can;
   can.shape = kCylinder;
   can.x = 10.0f;
   can.y = 5.0f;
   assert(inside(can, V3{7.0f, 4.0f, 7.0f}) && !inside(can, V3{8.0f, 0.0f, 8.0f}) && !inside(can, V3{0.0f, 5.0f, 0.0f}));
   assert(near(top(can), 5.0f));
   Region lying = can;
   lying.up = V3{1.0f, 0.0f, 0.0f};
   lying.right = V3{0.0f, 1.0f, 0.0f};
   assert(near(top(lying), 10.0f) && inside(lying, V3{4.9f, 9.0f, 0.0f}) && !inside(lying, V3{5.1f, 0.0f, 0.0f}));
   // On the world's axes: a box, and cylinders along x, y and z.
   Region aligned;
   aligned.shape = kBoxAligned;
   aligned.at = V3{0.0f, 20.0f, 0.0f};
   aligned.x = 5.0f;
   aligned.y = 6.0f;
   aligned.z = 7.0f;
   assert(inside(aligned, V3{4.9f, 25.9f, -6.9f}) && !inside(aligned, V3{0.0f, 26.1f, 0.0f}) && near(top(aligned), 26.0f));
   assert(!inside(aligned, V3{5.1f, 20.0f, 0.0f}) && !inside(aligned, V3{0.0f, 20.0f, 7.1f}));
   Region cx;
   cx.shape = kCylinderX;
   cx.x = 3.0f;
   cx.y = 8.0f;
   assert(inside(cx, V3{7.9f, 2.0f, 2.0f}) && !inside(cx, V3{8.1f, 0.0f, 0.0f}) && !inside(cx, V3{0.0f, 2.5f, 2.5f}));
   assert(near(top(cx), 3.0f));
   Region cy = cx;
   cy.shape = kCylinderY;
   assert(inside(cy, V3{2.0f, 7.9f, 2.0f}) && !inside(cy, V3{0.0f, 8.1f, 0.0f}) && !inside(cy, V3{2.5f, 0.0f, 2.5f}));
   assert(near(top(cy), 8.0f));
   Region cz = cx;
   cz.shape = kCylinderZ;
   assert(inside(cz, V3{2.0f, 2.0f, 7.9f}) && !inside(cz, V3{0.0f, 0.0f, 8.1f}) && !inside(cz, V3{2.5f, 2.5f, 0.0f}));
   assert(near(top(cz), 3.0f));
   // A plane: inside below it, and no top.
   Region ground;
   ground.shape = kPlane;
   ground.fwd = V3{0.0f, 1.0f, 0.0f};
   ground.w = 5.0f;
   assert(inside(ground, V3{100.0f, 4.9f, -3.0f}) && !inside(ground, V3{0.0f, 5.1f, 0.0f}) && std::isnan(top(ground)));
   // Unknown shapes and bad points are never inside.
   Region odd;
   odd.shape = 9;
   assert(!inside(odd, V3{}) && std::isnan(top(odd)));
   assert(!inside(box, V3{kNan, 50.0f, 0.0f}) && !inside(ball, V3{kNan, 10.0f, 0.0f}) && !inside(can, V3{0.0f, kNan, 0.0f}));
   assert(kSphere == 1 && kBox == 2 && kCylinder == 3 && kPlane == 4 && kBoxAligned == 5 && kCylinderZ == 8);
}

int main()
{
   the_vee();
   level_frames();
   slots_in_the_world();
   steering();
   trimming();
   stretching();
   speeds();
   targets();
   turning();
   who_flies();
   class_keys();
   slot_keys();
   slot_classes();
   ripples();
   squadron_rule();
   formation_shapes();
   drawn_shapes();
   formation_keys();
   stats();
   orders();
   ground_runs();
   landing();
   landing_places();
   mission_keys();
   squad_key_rules();
   attack_sides();
   landing_now();
   trail();
   landing_regions();
   std::printf("Flyer wingmen tests passed (the vee, level frames, slots in the world, steering, trimming, "
               "stretching, speeds, targets through the game's throttle, turning, who flies, class keys, slot keys, "
               "slot classes, ripples, the squadron rule, formation shapes, shapes as drawn, formation keys, stats, "
               "orders, passes at the ground, landing, landing places, mission keys, the squad key's hold and "
               "picks, attack sides, landing right away, the leader's trail, landing regions).\n");
   return 0;
}
