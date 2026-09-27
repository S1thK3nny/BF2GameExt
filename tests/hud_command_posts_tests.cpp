// Standalone tests (not a DLL build). Compile with /std:c++17 /W4 /WX, no NDEBUG.
#include "../PatcherDLL/src/render/hud_command_posts_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace hud_command_posts;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

struct Post {
   Entry entry;
   char  name;
};

} // namespace

int main()
{
   // Numbered posts first, by number; the rest after them in map order.
   Post posts[] = { {{0, 0}, 'a'}, {{3, 1}, 'b'}, {{0, 2}, 'c'}, {{1, 3}, 'd'},
                    {{2, 4}, 'e'}, {{0, 5}, 'f'} };
   order(posts, 6, &Post::entry);
   const char expected[] = "debacf";
   for (int i = 0; i < 6; ++i) assert(posts[i].name == expected[i]);

   // Unnumbered posts alone keep map order, and equal numbers keep it too.
   Post plain[] = { {{0, 4}, 'x'}, {{0, 1}, 'y'}, {{2, 3}, 'z'}, {{2, 0}, 'w'} };
   order(plain, 4, &Post::entry);
   assert(plain[0].name == 'w' && plain[1].name == 'z' && plain[2].name == 'y' &&
          plain[3].name == 'x');
   order(plain, 0, &Post::entry);   // nothing to do

   // An owned post drains as it is neutralised; a neutral one fills as it is
   // captured. The colour follows the owner, else the capturer once started.
   Capture owned = { 2, 0, 0.0f, 10.0f, 0.0f, 20.0f, true };
   assert(near(control(owned), 1) && control_team(owned) == 2);
   owned.neutralize = 2.5f;
   assert(near(control(owned), 0.75f) && control_team(owned) == 2);
   owned.neutralize = 12;   // past the class time: clamped
   assert(near(control(owned), 0));

   Capture neutral = { 0, 1, 10.0f, 10.0f, 0.0f, 20.0f, true };
   assert(near(control(neutral), 0) && control_team(neutral) == 0);
   neutral.capture = 5;
   assert(near(control(neutral), 0.25f) && control_team(neutral) == 1);
   neutral.biasTeam = 0;    // no capturing team yet
   assert(control_team(neutral) == 0);

   // A client far from the post does not simulate it: show it at rest.
   Capture far = { 3, 0, 5.0f, 10.0f, 0.0f, 20.0f, false };
   assert(near(control(far), 1) && control_team(far) == 3);
   far = { 0, 2, 0.0f, 10.0f, 15.0f, 20.0f, false };
   assert(near(control(far), 0) && control_team(far) == 0);

   // A class time of zero, or garbage, gives no progress rather than a NaN.
   const float nan = std::numeric_limits<float>::quiet_NaN();
   assert(ratio(5, 0) == 0 && ratio(5, -1) == 0 && ratio(nan, 10) == 0 && ratio(5, nan) == 0);
   Capture broken = { 1, 0, 5.0f, 0.0f, 0.0f, 0.0f, true };
   assert(near(control(broken), 1));

   // Teams outside the colour table count as neutral.
   assert(valid_team(3, 8) == 3 && valid_team(-1, 8) == 0 && valid_team(8, 8) == 0);

   // Change detection: first value always goes out, repeats do not, and an
   // invalidated slot sends again.
   Sent sent;
   assert(sent.set(5) && !sent.set(5) && sent.set(6));
   sent.invalidate();
   assert(sent.set(6));
   Sent value;
   assert(value.set_float(0.5f) && !value.set_float(0.5f) && value.set_float(0.25f));

   std::puts("Command post strip tests passed (slot order, control, capture colour, at-rest client posts, "
             "change detection).");
}
