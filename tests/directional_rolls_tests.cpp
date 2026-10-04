// Standalone tests (not a DLL build) for the directional rolls' choice of
// side, against SoldierAnimator::SetAction's rule for directional jumps, and
// for the names and parent chains its lookup walks.
#include "../PatcherDLL/src/entity/directional_rolls_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace directional_rolls;

namespace {

// SoldierAnimator::SetAction's JUMP case, as decompiled from the Phantom PDB
// build (0x00753E80), with the speeds along the body's forward and right rows:
// 0 jump, 2 forward, 3 backward, 4 left, 5 right.
int bf2_jump(float f, float s)
{
   if (s * s + f * f < 4.0f) return 0;
   if (f <= std::fabs(s)) {
      if (-f <= std::fabs(s)) return s <= 0.0f ? 5 : 4;
      return 3;
   }
   return 2;
}

Side side_of(float f, float s, float dt = 1.0f / 60.0f)
{
   // A body facing +Z with its right row along +X, moving (s, 0, f) m/s.
   const float right[3] = { 1.0f, 0.0f, 0.0f };
   const float forward[3] = { 0.0f, 0.0f, 1.0f };
   const float move[3] = { s * dt, 0.0f, f * dt };
   return choose_side(move, right, forward, dt);
}

void test_matches_directional_jumps()
{
   // Every direction and a range of speeds: a jump's left and right are a
   // roll's, its forward, backward and standing jump all keep diveforward.
   for (int deg = 0; deg < 360; deg += 3) {
      for (float speed : { 0.5f, 1.9f, 2.1f, 4.0f, 7.0f }) {
         const float a = deg * 3.14159265f / 180.0f;
         const float f = speed * std::cos(a), s = speed * std::sin(a);
         // What choose_side reads back from the move side_of builds, so the
         // diagonals compare the same rounded speeds.
         const float dt = 1.0f / 60.0f;
         const int jump = bf2_jump((f * dt) / dt, (s * dt) / dt);
         const Side want = jump == 4 ? Side::Left : jump == 5 ? Side::Right : Side::Forward;
         assert(side_of(f, s) == want);
      }
   }
}

void test_cases()
{
   assert(side_of(0.0f, 5.0f) == Side::Left);     // along the right row: the body's left
   assert(side_of(0.0f, -5.0f) == Side::Right);
   assert(side_of(5.0f, 0.0f) == Side::Forward);
   assert(side_of(-5.0f, 0.0f) == Side::Forward); // backward has no dive
   assert(side_of(3.0f, 3.0f) == Side::Left);     // 45 degrees goes to the side, as for jumps
   assert(side_of(3.0f, 2.9f) == Side::Forward);
   assert(side_of(0.0f, 1.9f) == Side::Forward);  // under 2 m/s
   assert(side_of(0.0f, 2.1f) == Side::Left);
   // No time, or a broken move: diveforward.
   assert(side_of(0.0f, 5.0f, 0.0f) == Side::Forward);
   assert(side_of(0.0f, 5.0f, -1.0f) == Side::Forward);
   assert(side_of(0.0f, NAN) == Side::Forward);
   // The frame time scales the move: 5 m/s is 5 m/s at any frame rate.
   assert(side_of(0.0f, 5.0f, 1.0f / 144.0f) == Side::Left);
   assert(side_of(0.0f, 5.0f, 1.0f / 20.0f) == Side::Left);
}

// The angle SetupPose's DIVE case aims the root at for a move: atan2 of the
// move along the right row over the move along the forward row.
float aim_of(const float move[3], const float right[3], const float forward[3])
{
   return std::atan2(dot3(move, right), dot3(move, forward));
}

float wrap(float a)
{
   const float pi = 3.14159265f;
   while (a > pi) a -= 2.0f * pi;
   while (a < -pi) a += 2.0f * pi;
   return a;
}

void test_side_dive_aim()
{
   const float pi = 3.14159265f;
   // A body yawed 30 degrees, so the rows are not the world axes.
   const float yaw = 30.0f * pi / 180.0f;
   const float right[3] = { std::cos(yaw), 0.0f, -std::sin(yaw) };
   const float forward[3] = { std::sin(yaw), 0.0f, std::cos(yaw) };
   const float up[3] = { 0.0f, 1.0f, 0.0f };

   for (int deg = -180; deg < 180; deg += 5) {
      const float a = deg * pi / 180.0f;
      // A move at angle `a` from the body, 0.12 m this frame, a little climb.
      float move[3];
      for (int i = 0; i < 3; ++i)
         move[i] = 0.12f * (std::cos(a) * forward[i] + std::sin(a) * right[i]) + 0.01f * up[i];
      const float stock = aim_of(move, right, forward);
      assert(std::fabs(wrap(stock - a)) < 1e-4f);

      // A left dive moves at +90 degrees from its root, a right dive at -90:
      // each root aims that far back from the move.
      float out[3];
      side_dive_aim(move, right, forward, Side::Left, out);
      assert(std::fabs(wrap(aim_of(out, right, forward) - (a - pi / 2))) < 1e-4f);
      assert(std::fabs(std::sqrt(dot3(out, out)) - 0.12f) < 1e-5f);   // the move's length in the plane
      side_dive_aim(move, right, forward, Side::Right, out);
      assert(std::fabs(wrap(aim_of(out, right, forward) - (a + pi / 2))) < 1e-4f);
      side_dive_aim(move, right, forward, Side::Forward, out);
      assert(out[0] == move[0] && out[1] == move[1] && out[2] == move[2]);
   }

   // A move straight to the soldier's left (positive along the right row) or
   // right: the dive's root faces the aim, along the forward row.
   float out[3];
   side_dive_aim(right, right, forward, Side::Left, out);
   assert(std::fabs(aim_of(out, right, forward)) < 1e-5f && dot3(out, forward) > 0.0f);
   const float leftward[3] = { -right[0], -right[1], -right[2] };
   side_dive_aim(leftward, right, forward, Side::Right, out);
   assert(std::fabs(aim_of(out, right, forward)) < 1e-5f && dot3(out, forward) > 0.0f);

   // Mid-roll the body turns 40 degrees with the aim while the move keeps its
   // way: the root turns 40 degrees back, keeping the dive on the move.
   const float turn = 40.0f * pi / 180.0f;
   const float right2[3] = { std::cos(yaw + turn), 0.0f, -std::sin(yaw + turn) };
   const float forward2[3] = { std::sin(yaw + turn), 0.0f, std::cos(yaw + turn) };
   side_dive_aim(right, right2, forward2, Side::Left, out);
   assert(std::fabs(wrap(aim_of(out, right2, forward2) + turn)) < 1e-4f);
}

void test_names()
{
   assert(std::strcmp(dive_name(Side::Left), "diveleft") == 0);
   assert(std::strcmp(dive_name(Side::Right), "diveright") == 0);
   assert(std::strcmp(dive_name(Side::Forward), "diveforward") == 0);

   char names[kNamesPerHalf][kNameMax];
   half_names(names, "human", "rifle", "diveleft", 0);
   assert(std::strcmp(names[0], "human_rifle_diveleft_upper") == 0);
   assert(std::strcmp(names[1], "human_rifle_diveleft") == 0);
   assert(std::strcmp(names[2], "human_rifle_diveleft_full") == 0);
   half_names(names, "bf3clones", "pistol", "diveright", 1);
   assert(std::strcmp(names[0], "bf3clones_pistol_diveright_lower") == 0);
   assert(std::strcmp(names[2], "bf3clones_pistol_diveright_full") == 0);
   assert(kMatchOf[0] == Match::Half && kMatchOf[1] == Match::Plain && kMatchOf[2] == Match::Full);
}

// The stock weapons: rifle (root), bazooka and tool under the rifle, pistol and
// melee under the tool.
const int kStockWeaponParent[] = { 0, 0, 0, 2, 2 };

void test_chains()
{
   int out[kMaxChain];
   auto weapon = [](int i) { return kStockWeaponParent[i]; };
   int n = parent_chain(3, 5, weapon, out);   // pistol
   assert(n == 3 && out[0] == 3 && out[1] == 2 && out[2] == 0);
   n = parent_chain(0, 5, weapon, out);       // the rifle is its own parent
   assert(n == 1 && out[0] == 0);
   n = parent_chain(1, 5, weapon, out);
   assert(n == 2 && out[0] == 1 && out[1] == 0);

   // A parent outside the table, or none, ends the chain.
   auto none = [](int) { return -1; };
   n = parent_chain(2, 5, none, out);
   assert(n == 1 && out[0] == 2);
   n = parent_chain(-1, 5, weapon, out);
   assert(n == 0);
   n = parent_chain(7, 5, weapon, out);
   assert(n == 0);

   // A loop ends where it closes; a long chain at kMaxChain.
   auto loop = [](int i) { return i == 1 ? 2 : 1; };
   n = parent_chain(1, 5, loop, out);
   assert(n == 2 && out[0] == 1 && out[1] == 2);
   auto down = [](int i) { return i + 1; };
   n = parent_chain(0, 30, down, out);
   assert(n == kMaxChain);
}

} // namespace

int main()
{
   test_matches_directional_jumps();
   test_cases();
   test_side_dive_aim();
   test_names();
   test_chains();
   std::printf("directional_rolls_tests: all passed\n");
   return 0;
}
