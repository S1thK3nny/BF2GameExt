#pragma once

#include <cmath>
#include <cstdint>

// Engine-independent selection memory. Never acquire/refresh from our own loan
// or from a HUD cache alone: require both a natural input and native acceptance.
namespace target_bar_selection {

struct Handle {
   uint8_t* obj = nullptr;
   uint32_t id = 0;
};

inline bool same(Handle a, Handle b) { return a.obj == b.obj && a.id == b.id; }

struct Retention {
   Handle target = {};
   double expiry = 0;
   bool unlimited = false;

   void reset() { target = {}; expiry = 0; unlimited = false; }

   Handle loan(Handle natural, bool targetValid, double now)
   {
      if (natural.obj) {
         // Even an ineligible new selection must prevent snap-back to the old one.
         if (!same(natural, target)) reset();
         return {};
      }
      if (!targetValid || !std::isfinite(now) || (!unlimited && now >= expiry)) reset();
      return target;
   }

   void observe(Handle natural, Handle accepted, double now, float holdSeconds)
   {
      if (natural.obj && same(natural, accepted) && std::isfinite(now)) {
         const float hold = std::isfinite(holdSeconds) && holdSeconds >= 0
            ? holdSeconds : 0.5f;
         target = accepted;
         expiry = now + hold;
         unlimited = hold == 0; // retain the existing explicit INI override
      } else if (natural.obj || !same(accepted, target)) {
         reset();
      }
      // A loan accepted by the native HUD is NOT a new selection. No refresh.
   }
};

// A vehicle and its exposed rider. The engine re-picks every frame by distance
// from the aim line and remembers nothing, so the two trade places while the
// crosshair is between them. Keep showing the member picked first; the other
// takes over only once it has been picked on every tick for `dwell` seconds.
// A pick from any other group replaces the pair at once, as before.
struct Pair {
   Handle shown = {};
   Handle pending = {};
   double since = 0;

   void reset() { shown = {}; pending = {}; since = 0; }

   // natural and naturalGroup: this tick's engine pick and the object it shows
   // as part of (a mounted rider's vehicle, anything else itself). shownValid
   // and shownGroup: whether the member being shown is still a live target, and
   // its group now, since riders dismount. Returns the member to show.
   Handle resolve(Handle natural, const void* naturalGroup, bool shownValid,
                  const void* shownGroup, double now, double dwell)
   {
      if (!natural.obj) { pending = {}; return {}; }
      if (!shown.obj || !shownValid || !naturalGroup || naturalGroup != shownGroup ||
          same(natural, shown) || !std::isfinite(now)) {
         shown = natural;
         pending = {};
         return shown;
      }
      if (!same(natural, pending)) { pending = natural; since = now; }
      if (now - since >= dwell) { shown = natural; pending = {}; }
      return shown;
   }
};

} // namespace target_bar_selection
