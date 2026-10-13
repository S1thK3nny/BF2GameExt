// Standalone tests (not a DLL build) for flyer wingmen: where each wingman's
// slot is, the point it is steered at, the speed it is asked for and who may
// fly as one - the logic in flyer_wingmen_core.hpp, fed what flyer_wingmen.cpp
// reads.
#include "../PatcherDLL/src/ai/flyer_wingmen_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace flyer_wingmen;

namespace {

const float kNan = std::numeric_limits<float>::quiet_NaN();

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
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
   // The game's unit: a share of the top speed.
   assert(near(target_for(50.0f, 100.0f), 0.5f) && near(target_for(150.0f, 100.0f), 1.0f));
   assert(near(target_for(-5.0f, 100.0f), 0.0f) && near(target_for(50.0f, 0.0f), 1.0f));
   assert(near(target_for(kNan, 100.0f), 1.0f));
}

// Round a turn it keeps most of its formation speed; past 60 degrees off its
// nose it slows as the game would have it.
void turning()
{
   assert(near(keep_speed(0.1f, 0.8f, 1.0f), 0.8f * kKeepShare));
   assert(near(keep_speed(0.1f, 0.8f, 0.6f), 0.8f * kKeepShare));
   assert(near(keep_speed(0.9f, 0.8f, 0.9f), 0.9f));                  // faster than asked: left alone
   assert(near(keep_speed(0.1f, 0.8f, kKeepSpeedCos - 0.01f), 0.1f));
   assert(near(keep_speed(0.1f, 0.8f, kNan), 0.1f));
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
   assert(count_for(&k, 3) == 0 && count_for(&none, 3) == 3 && count_for(nullptr, 7) == kMaxWingmen);
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
   turning();
   who_flies();
   class_keys();
   stats();
   std::printf("Flyer wingmen tests passed (the vee, level frames, slots in the world, steering, trimming, "
               "stretching, speeds, turning, who flies, class keys, stats).\n");
   return 0;
}
