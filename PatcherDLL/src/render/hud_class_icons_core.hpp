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

// -----------------------------------------------------------------------------
// State flags: player1.unit.state.<name> and player1.weaponN.state.<name>, each
// 1 or 0. Held flags stay 1 for as long as the state lasts; a moment (land,
// shot) is 1 for the one update it happens on. Easing is left to the .hud, with
// TransformNumberLerp.
// -----------------------------------------------------------------------------

enum UnitState : int {
   kUnitSprint, kUnitJump, kUnitFall, kUnitRoll, kUnitJet, kUnitHover, kUnitTumble, kUnitLand,
   kUnitStates
};
constexpr const char* kUnitStateNames[kUnitStates] = {
   "sprint", "jump", "fall", "roll", "jet", "hover", "tumble", "land",
};

// SoldierState (PDB enum): 0 STAND, 1 CROUCH, 2 PRONE, 3 SPRINT, 4 JUMP, 5 ROLL,
// 6 JET_JUMP, 7 JET_HOVER, 8 FALL, then 9 FLY, 10 TUMBLE, 11 BOUNCE,
// 12 FLY_RECOVER and 13 TUMBLE_RECOVER for being thrown and getting back up,
// and 19 SLIDE. The held flags, all but land.
constexpr bool unit_state(int soldierState, int flag)
{
   switch (flag) {
   case kUnitSprint: return soldierState == 3;
   case kUnitJump:   return soldierState == 4;
   case kUnitFall:   return soldierState == 8;
   case kUnitRoll:   return soldierState == 5;
   case kUnitJet:    return soldierState == 6;
   case kUnitHover:  return soldierState == 7;
   case kUnitTumble: return soldierState >= 9 && soldierState <= 13;
   default:          return false;
   }
}

constexpr bool soldier_airborne(int s) { return s == 4 || s == 6 || s == 7 || s == 8; }
constexpr bool soldier_grounded(int s) { return s == 0 || s == 1 || s == 2 || s == 3 || s == 5 || s == 19; }

// A landing is the first grounded state after an airborne one. Anything else in
// between (thrown, dead, in a vehicle) ends the jump without one.
struct Landing {
   bool airborne = false;

   void reset() { airborne = false; }

   bool update(int soldierState)
   {
      if (soldier_airborne(soldierState)) { airborne = true; return false; }
      const bool landed = airborne && soldier_grounded(soldierState);
      airborne = false;
      return landed;
   }
};

enum WeaponStateFlag : int {
   kWeaponFiring, kWeaponCharging, kWeaponReloading, kWeaponOverheated, kWeaponBlocking, kWeaponShot,
   kWeaponStates
};
constexpr const char* kWeaponStateNames[kWeaponStates] = {
   "firing", "charging", "reloading", "overheated", "blocking", "shot",
};

// WeaponState (PDB enum): 0 IDLE, 1 FIRE, 2 FIRE2, 3 CHARGE, 4 RELOAD,
// 5 OVERHEAT, 6 EMPTY. A melee weapon reuses FIRE for an attack, RELOAD for a
// block and OVERHEAT for the recovery after an attack (WeaponMelee::EnterState),
// so on one RELOAD is blocking and neither reload nor overheat is reported.
constexpr bool weapon_state(int state, bool melee, int flag)
{
   switch (flag) {
   case kWeaponFiring:     return state == 1 || state == 2;
   case kWeaponCharging:   return !melee && state == 3;
   case kWeaponReloading:  return !melee && state == 4;
   case kWeaponOverheated: return !melee && state == 5;
   case kWeaponBlocking:   return melee && state == 4;
   default:                return false;
   }
}

// A shot: Weapon::SignalFire stamps the weapon's last fire time with the mission
// time, so a new time on the same weapon is a new shot (a melee weapon signals
// each attack the same way). A different weapon is only noted.
struct Shots {
   const void* weapon   = nullptr;
   uint32_t    lastFire = 0;   // the float's bits

   void reset() { weapon = nullptr; }

   bool update(const void* w, uint32_t fireTimeBits)
   {
      const bool shot = w && w == weapon && fireTimeBits != lastFire;
      weapon   = w;
      lastFire = fireTimeBits;
      return shot;
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
