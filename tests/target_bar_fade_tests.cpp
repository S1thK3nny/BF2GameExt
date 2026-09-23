// Standalone tests, not a DLL build. Run without NDEBUG.
// cl /std:c++17 /EHsc /W4 /WX tests\target_bar_fade_tests.cpp
#include "../PatcherDLL/src/render/target_bar_fade.hpp"

#include <cassert>
#include <cstdio>

static void at(const target_bar_fade::Position& position, float x, float y)
{
   assert(position.value[0] == x && position.value[1] == y && position.value[2] == 0);
}

int main()
{
   target_bar_fade::Position first, second;
   const float standing[] = {0.4f, 0.2f, 0};
   const float moved[] = {0.5f, 0.3f, 0};
   const float corpse[] = {0.5f, 0.6f, 0};
   const float next[] = {0.8f, 0.4f, 0};
   at(first, -2, -2); // no previous target: never publish an uninitialised origin
   first.update(false, true, standing);
   at(first, standing[0], standing[1]);
   first.update(true, true, moved); // ordinary live fade still follows movement
   at(first, moved[0], moved[1]);
   first.update(true, false, corpse); // dead bounds must be ignored
   at(first, moved[0], moved[1]);
   for (int i = 0; i < 100; ++i) {
      first.update(true, false, nullptr); // death, then removed/stale handle
      at(first, moved[0], moved[1]);
   }
   first.update(false, true, next); // another target takes over immediately
   at(first, next[0], next[1]);
   first.update(false, false, nullptr); // new target already dead, no own anchor
   at(first, -2, -2);
   first.update(false, true, standing);
   first.update(false, true, nullptr); // new target cannot project: not old position
   at(first, -2, -2);
   first.update(true, true, moved);
   first.update(true, true, nullptr); // same live target goes behind the camera
   at(first, -2, -2);
   first.update(true, false, nullptr); // dying must not resurrect an old visible anchor
   at(first, -2, -2);
   first.update(true, true, next);
   second.update(false, true, standing);
   first.update(true, false, nullptr);
   second.update(true, true, moved); // weapon channels retain independent positions
   at(first, next[0], next[1]);
   at(second, moved[0], moved[1]);
   first.reset(); // mission reset or removal of listener
   first.update(true, false, nullptr);
   at(first, -2, -2);
   std::puts("Target-bar death/fade position tests passed.");
}
