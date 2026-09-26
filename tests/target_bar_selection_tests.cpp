// Standalone tests (not a DLL build). Compile with /std:c++17 /W4 /WX, no NDEBUG.
#include "../PatcherDLL/src/render/target_bar_selection.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <limits>

using namespace target_bar_selection;

int main()
{
   uint8_t objects[3] = {};
   const Handle a = {objects, 1}, b = {objects + 1, 2}, reused = {objects, 3};
   Retention first, second;

   // No hit signal is involved: acquire only after native HUD acceptance.
   assert(!first.loan(a, false, 0).obj);
   first.observe(a, a, 0, 0.5f);
   assert(same(first.target, a));
   assert(same(first.loan({}, true, 0.499), a));
   first.observe({}, a, 0.499, 0.5f); // our own loan cannot refresh the hold
   assert(!first.loan({}, true, 0.5).obj);
   first.observe({}, a, 0.501, 0.5f); // stale HUD cache cannot reacquire either
   assert(!first.target.obj);

   // A natural target refreshes continuously, even when nothing else changes.
   for (int frame = 0; frame < 1000; ++frame) {
      const double now = frame / 60.0;
      assert(!first.loan(a, true, now).obj);
      first.observe(a, a, now, 0.5f);
      assert(first.expiry == now + 0.5);
   }
   assert(same(first.loan({}, true, first.expiry - 0.001), a));
   const double deadline = first.expiry;
   assert(!first.loan({}, true, deadline).obj);

   // Reacquisition during fade gets a fresh full hold.
   first.observe(a, a, deadline + 0.1, 0.5f);
   assert(same(first.loan({}, true, deadline + 0.59), a));
   assert(!first.loan({}, true, deadline + 0.61).obj);

   // Switch to another accepted target immediately, friendly or enemy alike.
   first.observe(a, a, 20, 0.5f);
   assert(!first.loan(b, true, 20.1).obj);
   first.observe(b, b, 20.1, 0.5f);
   assert(same(first.loan({}, true, 20.2), b));

   // Invalid/native-rejected new targets must not snap back to the old target.
   first.loan(a, true, 20.3);
   first.observe(a, {}, 20.3, 0.5f);
   assert(!first.loan({}, true, 20.4).obj);
   first.observe(a, b, 20.5, 0.5f);
   assert(!first.target.obj);

   // Death/stale generation and reused pointers cannot inherit selection memory.
   first.observe(a, a, 21, 0.5f);
   assert(!first.loan({}, false, 21.1).obj);
   first.observe(a, a, 22, 0.5f);
   assert(!first.loan(reused, true, 22.1).obj);
   first.observe(reused, a, 22.1, 0.5f);
   assert(!first.target.obj);
   first.observe(reused, reused, 22.2, 0.5f);
   assert(same(first.target, reused));

   // Native rejection of a loan ends retention; it cannot revive on a later tick.
   first.observe({}, {}, 22.3, 0.5f);
   assert(!first.loan({}, true, 22.4).obj);

   // Weapon channels and their deadlines are independent.
   first.observe(a, a, 30, 0.5f);
   second.observe(b, b, 30.4, 0.5f);
   assert(!first.loan({}, true, 30.5).obj);
   assert(same(second.loan({}, true, 30.5), b));
   second.reset(); // mission/controlled-object/weapon/listener reset
   assert(!second.loan({}, true, 30.6).obj);

   // Keep the explicit legacy zero override, but reject malformed durations/time.
   first.observe(a, a, 0, 0);
   assert(same(first.loan({}, true, 100000), a));
   for (float bad : {-1.0f, std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::quiet_NaN()}) {
      first.observe(a, a, 0, bad);
      assert(same(first.loan({}, true, 0.49), a));
      assert(!first.loan({}, true, 0.5).obj);
   }
   first.observe(a, a, 0, 0.5f);
   assert(!first.loan({}, true, std::numeric_limits<double>::quiet_NaN()).obj);

   // Vary update rate: thousands of borrowed frames must never extend a deadline.
   for (int hz = 15; hz <= 360; ++hz) {
      first.observe(a, a, 0, 0.5f);
      for (int frame = 1; frame < hz * 2; ++frame) {
         const double now = static_cast<double>(frame) / hz;
         const Handle held = first.loan({}, true, now);
         assert(bool(held.obj) == (now < 0.5));
         first.observe({}, held, now, 0.5f);
      }
   }

   // A vehicle and its exposed rider: the engine's pick flips between them.
   // Groups stand in for display_group(): the rider shows as part of the bike.
   uint8_t world[4] = {};
   const Handle bike = {world, 10}, rider = {world + 1, 11}, other = {world + 2, 12};
   const void* bikeGroup = world;
   const void* otherGroup = world + 2;
   const double dwell = 0.3;
   auto group = [&](Handle h) { return same(h, other) ? otherGroup : bikeGroup; };
   Pair pair;
   // The first member picked is shown; the other one flickering in never is.
   assert(same(pair.resolve(bike, bikeGroup, false, nullptr, 0, dwell), bike));
   for (int frame = 1; frame < 600; ++frame) {
      const Handle pick = frame % 3 ? rider : bike;
      const double now = frame / 60.0;
      assert(same(pair.resolve(pick, group(pick), true, group(pair.shown), now, dwell), bike));
   }
   // Picked on every tick for the dwell, the other member takes over, and the
   // way back needs the same.
   assert(same(pair.resolve(bike, bikeGroup, true, bikeGroup, 9.99, dwell), bike));
   double now = 10;
   for (; now < 10 + dwell - 1e-9; now += 1 / 144.0)
      assert(same(pair.resolve(rider, bikeGroup, true, bikeGroup, now, dwell), bike));
   assert(same(pair.resolve(rider, bikeGroup, true, bikeGroup, now, dwell), rider));
   assert(same(pair.resolve(bike, bikeGroup, true, bikeGroup, now + 0.1, dwell), rider));
   assert(same(pair.resolve(bike, bikeGroup, true, bikeGroup, now + 0.45, dwell), bike));
   // A tick with no pick breaks the run: the dwell starts again.
   pair.resolve(rider, bikeGroup, true, bikeGroup, 20, dwell);
   assert(!pair.resolve({}, nullptr, true, bikeGroup, 20.2, dwell).obj);
   assert(same(pair.resolve(rider, bikeGroup, true, bikeGroup, 20.4, dwell), bike));
   assert(same(pair.resolve(rider, bikeGroup, true, bikeGroup, 20.69, dwell), bike));
   assert(same(pair.resolve(rider, bikeGroup, true, bikeGroup, 20.71, dwell), rider));
   // Any other target replaces the pair at once, as before.
   assert(same(pair.resolve(other, otherGroup, true, bikeGroup, 21, dwell), other));
   // A dismounted rider is its own group, so it replaces the bike at once too.
   pair.reset();
   pair.resolve(bike, bikeGroup, false, nullptr, 30, dwell);
   assert(same(pair.resolve(rider, world + 1, true, bikeGroup, 30.01, dwell), rider));
   // A shown member that died, or a group that cannot be read, gives no hold.
   pair.reset();
   pair.resolve(bike, bikeGroup, false, nullptr, 40, dwell);
   assert(same(pair.resolve(rider, bikeGroup, false, nullptr, 40.01, dwell), rider));
   assert(same(pair.resolve(bike, nullptr, true, bikeGroup, 40.02, dwell), bike));
   assert(same(pair.resolve(rider, bikeGroup, true, bikeGroup,
                            std::numeric_limits<double>::quiet_NaN(), dwell), rider));
   std::puts("Selection retention tests passed (acquisition, replacement, expiry, no self-refresh, "
             "15-360 Hz, rider/vehicle pair).");
}
