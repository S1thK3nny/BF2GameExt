#pragma once

#include "target_bar_geometry.hpp"

// World-anchor lifetime, separate from native HUD enable/disable/alpha.
// Keeping this independent of game memory lets us test death/despawn fades.
namespace target_bar_fade {

struct Position {
   using Project = bool (*)(const target_bar_geometry::Box&, float*);
   float value[3] = { -2.0f, -2.0f, 0.0f };

   void reset()
   {
      hasWorldBounds = false;
      hide();
   }

   void update(bool sameTarget, bool alive, const target_bar_geometry::Box* liveBounds,
               Project project)
   {
      // A new target (including a reused pointer with a new handle ID) must
      // never inherit the preceding target's death-fade anchor.
      if (!sameTarget) reset();
      if (alive) {
         hasWorldBounds = liveBounds && target_bar_geometry::valid_box(*liveBounds);
         if (hasWorldBounds) worldBounds = *liveBounds;
      }
      // On death/despawn retain the last live WORLD snapshot, not its screen
      // coordinates. The box keeps the same top-centre anchor and close-up
      // camera-plane policy without consulting a corpse or expired handle.
      hide();
      float projected[3];
      if (hasWorldBounds && project && project(worldBounds, projected))
         for (int axis = 0; axis < 3; ++axis) value[axis] = projected[axis];
   }

private:
   target_bar_geometry::Box worldBounds = {};
   bool hasWorldBounds = false;

   void hide()
   {
      value[0] = value[1] = -2.0f;
      value[2] = 0.0f;
   }
};

} // namespace target_bar_fade
