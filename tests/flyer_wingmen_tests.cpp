// Standalone tests (not a DLL build) for flyer wingmen: where each wingman's
// slot is, the point it is steered at, the speed it is asked for and who may
// fly as one - the logic in flyer_wingmen_core.hpp, fed what flyer_wingmen.cpp
// reads.
#include "../PatcherDLL/src/ai/flyer_wingmen_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>

using namespace flyer_wingmen;

namespace {

const float kNan = std::numeric_limits<float>::quiet_NaN();

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
   std::printf("Flyer wingmen tests passed (the vee, level frames, slots in the world, steering, trimming, "
               "stretching, speeds, targets through the game's throttle, turning, who flies, class keys, slot keys, "
               "slot classes, ripples, the squadron rule, formation shapes, shapes as drawn, formation keys, stats).\n");
   return 0;
}
