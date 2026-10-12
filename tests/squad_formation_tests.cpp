// Standalone tests (not a DLL build) for squads: where members stand, which way
// the squad faces, and how ODF keys and the INI decide each setting - the logic
// in squad_formation_core.hpp, fed what squad_formation.cpp reads.
#include "../PatcherDLL/src/ai/squad_formation_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>

using namespace squad_formation;

namespace {

constexpr float kS = 3.0f;
const float kNan = std::numeric_limits<float>::quiet_NaN();

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
float dist(Vec2 a, Vec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

void names()
{
   assert(parse_layout("wedge", Layout::Stock) == Layout::Wedge);
   assert(parse_layout("WEDGE", Layout::Stock) == Layout::Wedge);
   assert(parse_layout("File", Layout::Stock) == Layout::File);
   assert(parse_layout("column", Layout::Stock) == Layout::Column);
   assert(parse_layout("line", Layout::Stock) == Layout::Line);
   assert(parse_layout("auto", Layout::Stock) == Layout::Auto);
   assert(parse_layout("stock", Layout::Auto) == Layout::Stock);
   assert(parse_layout(" \"wedge\" ", Layout::Stock) == Layout::Wedge);   // as an ODF may quote it
   assert(parse_layout("wedges", Layout::Auto) == Layout::Auto);
   assert(parse_layout("wed", Layout::Auto) == Layout::Auto);
   assert(parse_layout("", Layout::Auto) == Layout::Auto);
   assert(parse_layout(nullptr, Layout::Line) == Layout::Line);
}

void spacing_limits()
{
   assert(clamp_spacing(0.5f) == kMinSpacing);
   assert(clamp_spacing(50.0f) == kMaxSpacing);
   assert(clamp_spacing(kNan) == kDefaultSpacing);
   assert(clamp_spacing(4.0f) == 4.0f);
}

// Every slot behind or beside the leader, a full spacing clear of the leader
// and of every other slot, for squads of up to eight.
void slots_keep_their_distance()
{
   for (Layout layout : {Layout::Wedge, Layout::File, Layout::Column, Layout::Line}) {
      for (int n = 1; n <= kMaxSquadSize; ++n) {
         for (int i = 0; i < n; ++i) {
            const Vec2 a = slot_offset(layout, i, kS, true);
            assert(a.z <= 0.0f);
            assert(dist(a, {}) >= kS * 0.99f);
            for (int j = i + 1; j < n; ++j) assert(dist(a, slot_offset(layout, j, kS, true)) >= kS * 0.99f);
         }
      }
   }
}

void slot_shapes()
{
   for (int i = 0; i < 6; ++i) {
      const Vec2 c = slot_offset(Layout::Column, i, kS, true);
      assert(near(c.x, 0.0f) && near(c.z, -(i + 1) * kS));
   }
   for (Layout layout : {Layout::Wedge, Layout::File, Layout::Line}) {
      for (int i = 0; i < 6; i += 2) {
         const Vec2 r = slot_offset(layout, i, kS, true), l = slot_offset(layout, i + 1, kS, true);
         assert(r.x > 0.0f && near(l.x, -r.x) && near(l.z, r.z));
      }
   }
   const Vec2 w0 = slot_offset(Layout::Wedge, 0, kS, true), w2 = slot_offset(Layout::Wedge, 2, kS, true);
   assert(near(w0.x, -w0.z) && near(dist(w0, {}), kS) && near(dist(w0, w2), kS));
   const Vec2 f0 = slot_offset(Layout::File, 0, kS, true), f1 = slot_offset(Layout::File, 1, kS, true);
   assert(near(dist(f0, f1), kS) && near(f0.z, -kS));
   assert(near(slot_offset(Layout::Line, 3, kS, true).z, 0.0f));
   assert(near(slot_offset(Layout::Column, 0, 100.0f, true).z, -kMaxSpacing));
   assert(near(slot_offset(Layout::Column, -3, kS, true).z, -kS));
}

void auto_reads_the_room()
{
   for (int i = 0; i < 6; ++i) {
      const Vec2 open = slot_offset(Layout::Auto, i, kS, true), wedge = slot_offset(Layout::Wedge, i, kS, true);
      const Vec2 tight = slot_offset(Layout::Auto, i, kS, false), column = slot_offset(Layout::Column, i, kS, false);
      assert(near(open.x, wedge.x) && near(open.z, wedge.z));
      assert(near(tight.x, column.x) && near(tight.z, column.z));
   }
}

void turned_to_the_heading()
{
   Vec2 w = to_world({1.0f, -2.0f}, {0.0f, 1.0f});
   assert(near(w.x, 1.0f) && near(w.z, -2.0f));
   w = to_world({0.0f, -2.0f}, {1.0f, 0.0f});   // facing +x: behind is -x
   assert(near(w.x, -2.0f) && near(w.z, 0.0f));
   w = to_world({1.0f, 0.0f}, {1.0f, 0.0f});    // right of +x is -z (left-handed, y up)
   assert(near(w.x, 0.0f) && near(w.z, -1.0f));
   for (int a = 0; a < 360; a += 15) {
      const float r = a * 3.14159265f / 180.0f;
      const Vec2 s = slot_offset(Layout::Wedge, 3, kS, true);
      assert(near(dist(to_world(s, {std::sin(r), std::cos(r)}), {}), dist(s, {}), 1e-3f));
   }
}

void heading_follows_travel_not_the_camera()
{
   Heading none;
   Heading h = next_heading(none, {0.0f, 5.0f}, {1.0f, 0.0f}, 0.0f);
   assert(h.valid && near(h.dir.x, 0.0f) && near(h.dir.z, 1.0f));
   h = next_heading(none, {0.0f, 0.0f}, {1.0f, 0.0f}, 0.0f);
   assert(h.valid && near(h.dir.x, 1.0f) && near(h.dir.z, 0.0f));
   assert(!next_heading(none, {0.0f, 0.0f}, {0.0f, 0.0f}, 0.1f).valid);
   // Standing still, the camera turning round changes nothing.
   const Heading north = next_heading(none, {0.0f, 5.0f}, {0.0f, 1.0f}, 0.0f);
   h = next_heading(north, {0.2f, -0.3f}, {0.0f, -1.0f}, 1.0f);
   assert(near(h.dir.x, 0.0f) && near(h.dir.z, 1.0f));
   // However long it stands.
   for (float dt : {10.0f, 100.0f, 1e6f}) {
      h = next_heading(north, {0.0f, 0.0f}, {1.0f, 0.0f}, dt);
      assert(near(h.dir.x, 0.0f) && near(h.dir.z, 1.0f));
   }
   // Travelling east, it eases round: about 63% of the way in kTurnTime, then all of it.
   h = next_heading(north, {5.0f, 0.0f}, {0.0f, 1.0f}, kTurnTime);
   const float angle = std::atan2(h.dir.x, h.dir.z);
   assert(angle > 0.6f && angle < 1.2f);
   for (int i = 0; i < 100; ++i) h = next_heading(h, {5.0f, 0.0f}, {0.0f, 1.0f}, 0.05f);
   assert(near(h.dir.x, 1.0f, 1e-3f) && near(h.dir.z, 0.0f, 1e-3f));
   // Doubling straight back never leaves it without a direction.
   for (float dt : {0.01f, kTurnTime, 10.0f}) {
      h = next_heading(north, {0.0f, -5.0f}, {0.0f, 1.0f}, dt);
      assert(h.valid && near(std::sqrt(h.dir.x * h.dir.x + h.dir.z * h.dir.z), 1.0f, 1e-3f));
   }
   // A bad velocity or time keeps what it had.
   assert(near(next_heading(north, {kNan, 3.0f}, {1.0f, 0.0f}, 0.1f).dir.z, 1.0f));
   assert(near(next_heading(north, {5.0f, 0.0f}, {1.0f, 0.0f}, kNan).dir.z, 1.0f));
   assert(near(next_heading(north, {5.0f, 0.0f}, {1.0f, 0.0f}, 0.0f).dir.z, 1.0f));
}

// The slot survives the follow agent's scales as ours, and nothing stock is
// taken for it.
void tagged_slots()
{
   const Vec2 slot = slot_offset(Layout::Wedge, 1, kS, true);
   const Vec3 t = tagged_slot(slot);
   for (float scale : {2.0f, 2.2f, 1.7f}) {
      assert(is_tagged(t.y * scale));
      assert(near(t.x * scale, slot.x * scale / kAgentScale) && near(t.z * scale, slot.z * scale / kAgentScale));
   }
   assert(near(t.x * kAgentScale, slot.x) && near(t.z * kAgentScale, slot.z));
   for (float y : {0.0f, -0.0f, 1e-6f, 0.25f, -3.0f, kNan, 1e-30f}) assert(!is_tagged(y));
   // The same slot is the same offset, bit for bit, so the navigator is kept.
   const Vec3 again = tagged_slot(slot_offset(Layout::Wedge, 1, kS, true));
   assert(again.x == t.x && again.y == t.y && again.z == t.z);
}

void slot_points()
{
   const float position[3] = {10.0f, 2.0f, -4.0f};
   // Heading +x, the slot 3 m behind: 3 m along -x from the leader, level with it.
   const Vec3 p = slot_point({0.0f, kTag, -3.0f}, {1.0f, 0.0f}, position);
   assert(near(p.x, 7.0f) && near(p.y, 2.0f) && near(p.z, -4.0f));
   const Vec3 q = slot_point({1.5f, kTag, 0.0f}, {0.0f, 1.0f}, position);
   assert(near(q.x, 11.5f) && near(q.z, -4.0f));
}

void keys_from_odf_values()
{
   ClassKeys k;
   assert(set_key(k, Key::Leader, "3") && k.hasLeader && k.leader == 3);
   assert(set_key(k, Key::Leader, "-2") && k.leader == 0);
   assert(set_key(k, Key::Leader, "5000") && k.leader == kMaxRank);
   assert(set_key(k, Key::Size, " 6 ") && k.hasSize && k.size == 6);
   assert(set_key(k, Key::Size, "40") && k.size == kMaxSquadSize);
   assert(set_key(k, Key::Size, "0") && k.size == 0);
   assert(set_key(k, Key::Formation, "\"Column\"") && k.hasFormation && k.formation == Layout::Column);
   assert(set_key(k, Key::Spacing, "4.5") && k.hasSpacing && near(k.spacing, 4.5f));
   assert(set_key(k, Key::Spacing, "100") && near(k.spacing, kMaxSpacing));
   assert(set_key(k, Key::Follower, "0") && k.hasFollower && !k.follower);
   assert(set_key(k, Key::Follower, "1") && k.follower);
   // Values that are not numbers or names leave the key as it was.
   ClassKeys u;
   assert(!set_key(u, Key::Leader, "officer") && !u.hasLeader);
   assert(!set_key(u, Key::Size, "4x") && !u.hasSize);
   assert(!set_key(u, Key::Formation, "phalanx") && !u.hasFormation);
   assert(!set_key(u, Key::Spacing, "") && !u.hasSpacing);
   assert(!set_key(u, Key::Spacing, "nan") && !u.hasSpacing);
   assert(!set_key(u, Key::Follower, nullptr) && !u.hasFollower);
}

// A key a class sets decides that setting, whatever the INI says; one it leaves
// out takes the INI's.
void odf_over_the_ini()
{
   Defaults off;
   off.formation = Layout::Stock;
   off.spacing = 2.0f;
   off.size = 2;
   off.autoFormUp = false;
   off.aiSquads = false;

   // No keys: the INI, off or on.
   assert(formation_for(nullptr, off) == Layout::Stock);
   assert(near(spacing_for(nullptr, off), 2.0f));
   assert(size_for(nullptr, off) == 2);
   assert(!forms_up_for(nullptr, off));
   assert(leader_rank_for(nullptr, off, false) == 0);
   Defaults on;
   assert(formation_for(nullptr, on) == Layout::Auto && forms_up_for(nullptr, on));
   assert(leader_rank_for(nullptr, on, false) == 1);

   // A class's keys win with the INI off.
   ClassKeys k;
   k.hasFormation = true; k.formation = Layout::Wedge;
   k.hasSpacing = true;   k.spacing = 5.0f;
   assert(formation_for(&k, off) == Layout::Wedge);
   assert(near(spacing_for(&k, off), 5.0f));
   assert(size_for(&k, off) == 2);                 // not set: the INI's
   assert(!forms_up_for(&k, off));                 // nothing says tag-alongs form up
   k.hasSize = true; k.size = 3;
   assert(size_for(&k, off) == 3 && forms_up_for(&k, off));
   k.size = 0;
   assert(!forms_up_for(&k, on));                  // and SquadSize 0 says they do not
   k.hasLeader = true; k.leader = 4;
   assert(leader_rank_for(&k, off, false) == 4);
   k.leader = 0;
   assert(leader_rank_for(&k, on, false) == 0);
   // Keys a class leaves out still come from the INI while another is set.
   ClassKeys onlyLeader;
   onlyLeader.hasLeader = true; onlyLeader.leader = 2;
   assert(formation_for(&onlyLeader, on) == Layout::Auto && size_for(&onlyLeader, on) == on.size);

   // A stock layout set on a class turns its squads back to stock under an INI that is on.
   ClassKeys stock;
   stock.hasFormation = true; stock.formation = Layout::Stock;
   assert(formation_for(&stock, on) == Layout::Stock);

   // An AI leader's squad: its class's SquadSize, as for the player, or the INI's for AI squads.
   assert(ai_size_for(nullptr, on) == on.aiSize && on.aiSize != on.size);
   ClassKeys sized;
   sized.hasSize = true; sized.size = 5;
   assert(ai_size_for(&sized, off) == 5 && size_for(&sized, off) == 5);
   sized.size = 40;
   assert(ai_size_for(&sized, on) == kMaxSquadSize);
}

void heroes_keep_out_unless_told()
{
   Defaults on;
   assert(leader_rank_for(nullptr, on, true) == 0);
   assert(!joins_for(nullptr, true));
   assert(joins_for(nullptr, false));
   ClassKeys k;
   k.hasLeader = true; k.leader = 5;
   k.hasFollower = true; k.follower = true;
   assert(leader_rank_for(&k, on, true) == 5);
   assert(joins_for(&k, true));
   k.follower = false;
   assert(!joins_for(&k, false));
}

void the_tag_along_stop()
{
   // Loose as stock: the game's own stop at two formation followers.
   for (int c = 0; c < 5; ++c) assert(capped_count(c, false, 4) == c);
   // In formation: recruiting until the squad is full.
   assert(capped_count(0, true, 4) == 0);
   assert(capped_count(3, true, 4) == 0);
   assert(capped_count(4, true, 4) == 2);
   assert(capped_count(7, true, 4) == 2);
   assert(capped_count(0, true, 1) == 0 && capped_count(1, true, 1) == 2);
}

} // namespace

int main()
{
   names();
   spacing_limits();
   slots_keep_their_distance();
   slot_shapes();
   auto_reads_the_room();
   turned_to_the_heading();
   heading_follows_travel_not_the_camera();
   tagged_slots();
   slot_points();
   keys_from_odf_values();
   odf_over_the_ini();
   heroes_keep_out_unless_told();
   the_tag_along_stop();
   std::printf("Squad formation tests passed (names, spacing, slots, auto, headings, tags, slot points, "
               "ODF keys over the INI, heroes, the tag-along stop).\n");
   return 0;
}
