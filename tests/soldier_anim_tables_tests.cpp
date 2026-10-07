// Standalone tests (not a DLL build) for the names and parent chains
// soldier_anim_tables::find_named walks, the way AnimationFinder looks up a
// soldier animation. Used by the directional rolls and the directional jets.
#include "../PatcherDLL/src/entity/soldier_anim_tables_core.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace soldier_anim_tables;

namespace {

void test_names()
{
   char names[kNamesPerHalf][kNameMax];
   half_names(names, "human", "rifle", "diveleft", 0);
   assert(std::strcmp(names[0], "human_rifle_diveleft_upper") == 0);
   assert(std::strcmp(names[1], "human_rifle_diveleft") == 0);
   assert(std::strcmp(names[2], "human_rifle_diveleft_full") == 0);
   half_names(names, "bf3clones", "pistol", "diveright", 1);
   assert(std::strcmp(names[0], "bf3clones_pistol_diveright_lower") == 0);
   assert(std::strcmp(names[2], "bf3clones_pistol_diveright_full") == 0);
   half_names(names, "human", "rifle", "jetpack_hover_forward", 1);
   assert(std::strcmp(names[0], "human_rifle_jetpack_hover_forward_lower") == 0);
   assert(std::strcmp(names[1], "human_rifle_jetpack_hover_forward") == 0);
   assert(std::strcmp(names[2], "human_rifle_jetpack_hover_forward_full") == 0);
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
   test_names();
   test_chains();
   std::printf("soldier_anim_tables_tests: all passed\n");
   return 0;
}
