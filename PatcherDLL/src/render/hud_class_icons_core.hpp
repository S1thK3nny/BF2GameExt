#pragma once

#include <stdint.h>

#include "../core/pbl_hash.hpp"

// =============================================================================
// Pure selection rules for the class, stance and vehicle health icons, shared
// with tests/hud_class_icons_tests.cpp. Nothing here touches the engine: the
// caller reads the states and class hashes and supplies the "is this texture
// loaded" test.
// =============================================================================

namespace hud_class_icons {

// player1.unit.stance values.
enum Stance : uint32_t { kStand = 0, kCrouch = 1, kProne = 2, kBall = 3 };

// SoldierState (PDB enum): 1 CROUCH, 2 PRONE. Every other state (sprint, jump,
// roll, jet, fall, knockdowns, choke, pilot animation...) counts as standing.
constexpr uint32_t soldier_stance(int state)
{
   return state == 1 ? kCrouch : state == 2 ? kProne : kStand;
}

// EntityDroideka state machine: only fully balled (0x0C) shows the ball icon.
// Rolling up (0x0B) and unrolling (0x0D) keep the standing one.
constexpr uint32_t droideka_stance(int state)
{
   return state == 0x0C ? kBall : kStand;
}

// Appended to the class's HealthTexture name to find a stance's icon.
constexpr const char* stance_suffix(uint32_t stance)
{
   switch (stance) {
   case kCrouch: return "_crouch";
   case kProne:  return "_prone";
   case kBall:   return "_ball";
   default:      return nullptr;
   }
}

// Where a stance's icon falls back to when it is not loaded: prone to crouch,
// everything else straight to standing.
constexpr uint32_t fallback_stance(uint32_t stance)
{
   return stance == kProne ? kCrouch : kStand;
}

// The icon for a HealthTexture hash in a stance: the first loaded one along the
// fallback chain (prone -> crouch -> stand, crouch -> stand, ball -> stand),
// ending at the base name, else 0 (nothing to show). `loaded` is only ever asked
// about non-zero hashes.
template <class Loaded>
uint32_t pick_texture(uint32_t base, uint32_t stance, Loaded&& loaded)
{
   if (base == 0) return 0;
   for (uint32_t s = stance; s != kStand; s = fallback_stance(s)) {
      const char* suffix = stance_suffix(s);
      if (!suffix) break;
      const uint32_t variant = pbl_hash_append(base, suffix);
      if (variant != 0 && loaded(variant)) return variant;
   }
   return loaded(base) ? base : 0;
}

// Remembers the last pick, so the texture table is probed only when the class
// or stance changes rather than every frame. A probe for a name that is not
// loaded walks the table, and that walk has no bound if the table is full.
struct Pick {
   uint32_t base   = 0;
   uint32_t stance = 0;
   uint32_t result = 0;
   bool     valid  = false;

   void invalidate() { valid = false; }

   template <class Loaded>
   uint32_t get(uint32_t b, uint32_t s, Loaded&& loaded)
   {
      if (!valid || b != base || s != stance) {
         base   = b;
         stance = s;
         result = pick_texture(b, s, loaded);
         valid  = true;
      }
      return result;
   }
};

// Change detection for one published value. Invalidated at GameEvents::Open,
// so the first update of every mission sends and later ones only on change.
struct Published {
   uint32_t value = 0;
   bool     valid = false;

   void invalidate() { valid = false; }

   // True when v must be sent.
   bool set(uint32_t v)
   {
      if (valid && value == v) return false;
      value = v;
      valid = true;
      return true;
   }
};

} // namespace hud_class_icons
