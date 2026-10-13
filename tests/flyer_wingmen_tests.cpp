// Standalone tests (not a DLL build) for flyer wingmen: where each wingman's
// slot is, the point it is steered at, the speed it is asked for and who may
// fly as one - the logic in flyer_wingmen_core.hpp, fed what flyer_wingmen.cpp
// reads.
#include "../PatcherDLL/src/ai/flyer_wingmen_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

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

// It is steered at a point carried on ahead of its slot.
void steering()
{
   const V3 p = steer_point({10, 0, 0}, {0, 0, 80});
   assert(near(p, {10, 0, 80 * kLeadSeconds}));
   assert(near(steer_point({1, 2, 3}, {0, 0, 0}), {1, 2, 3}));
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
   speeds();
   turning();
   who_flies();
   stats();
   std::printf("Flyer wingmen tests passed (the vee, level frames, slots in the world, steering, speeds, "
               "turning, who flies, stats).\n");
   return 0;
}
