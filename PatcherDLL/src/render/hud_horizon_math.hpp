#pragma once

#include <cmath>
#include <cstdint>

// Camera-only horizon levelling. World +Y projected onto the rendered camera's
// right/up axes defines the up direction, including inverted flight.
namespace hud_horizon {

struct State {
   float degrees = 0.0f;
   bool atPole = false;

   void reset() { degrees = 0.0f; atPole = false; }

   float update(const float camera[16], float tanHalfFovW, float tanHalfFovH,
                uint32_t width, uint32_t height)
   {
      if (!camera || !width || !height ||
          !std::isfinite(tanHalfFovW) || !std::isfinite(tanHalfFovH) ||
          tanHalfFovW <= 0.0f || tanHalfFovH <= 0.0f) {
         reset();
         return degrees;
      }
      // Camera bases are unit vectors. Reject an uninitialised/invalid pose,
      // but ignore translation and the unused W/padding lanes completely.
      for (int row = 0; row < 3; ++row) {
         double length2 = 0.0;
         for (int axis = 0; axis < 3; ++axis) {
            const double v = camera[row * 4 + axis];
            if (!std::isfinite(v)) { reset(); return degrees; }
            length2 += v * v;
         }
         if (length2 < 0.9 || length2 > 1.1) { reset(); return degrees; }
      }

      const double rightY = camera[1];
      const double upY = camera[5];
      const double projectedLength2 = rightY * rightY + upY * upY;
      // Within ~0.57 degrees of vertical there is no reliable horizon. Hold
      // the last direction; resume beyond ~1.15 degrees (hysteresis prevents
      // noisy camera samples toggling the hold). A fresh vertical view uses 0.
      if (projectedLength2 < (atPole ? 0.0004 : 0.0001)) {
         atPole = true;
         return degrees;
      }
      atPole = false;

      // Pixel-space projection, not normalised 0..1 viewport coordinates.
      // Normally the focal scales are equal, so FOV/zoom cancel out and this
      // reduces to atan2(right.y, up.y).
      const double x = rightY * double(width) / tanHalfFovW;
      const double y = upY * double(height) / tanHalfFovH;
      // HUD coordinates are Y-down. D3DX RotationZ(theta) maps screen-up
      // (0,-1) to (sin(theta),-cos(theta)), hence this sign, not its negative.
      // EventRotation is absolute and immediate: wrapping +/-180 does not spin.
      constexpr double kRadiansToDegrees = 57.295779513082320876;
      degrees = static_cast<float>(std::atan2(x, y) * kRadiansToDegrees);
      return degrees;
   }
};

} // namespace hud_horizon
