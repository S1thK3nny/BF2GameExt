#pragma once

// Screen-position lifetime, separate from native HUD enable/disable/alpha.
// Keeping this independent of game memory lets us test death/despawn fades.
namespace target_bar_fade {

struct Position {
   float value[3] = { -2.0f, -2.0f, 0.0f };

   void reset()
   {
      value[0] = value[1] = -2.0f;
      value[2] = 0.0f;
   }

   void update(bool sameTarget, bool alive, const float* projected)
   {
      // A new target (including a reused pointer with a new handle ID) must
      // never inherit the preceding target's death-fade position.
      if (!sameTarget) reset();
      if (!alive) return; // death/despawn: keep the exact last published anchor
      if (!projected) { reset(); return; } // live but behind camera/invalid
      for (int axis = 0; axis < 3; ++axis) value[axis] = projected[axis];
   }
};

} // namespace target_bar_fade
