#pragma once

#include <stdint.h>

#include "soldier_anim_tables_core.hpp"

struct SoldierAnimation;

// =============================================================================
// One view of the soldier animation tables, whichever GameExt patches moved
// them, so a feature that reads or swaps a soldier animation, or walks the
// bank and weapon parents the way AnimationFinder does, never has to know
// which other features are on.
//
// Stock, each animation map's table lives in SoldierAnimatorClass::sInstance
// (the action getters read [class + (map * 0x97 + action) * 8 + 0x24], + 0x28
// for the lower body), and the maps and banks in SoldierAnimationBank's s_aMap
// and s_aBank. With [LimitIncreases] ComboAnimIncrease (on by default),
// combo_anim_limit replaces the getters with its own and keeps the tables
// (combo_anim_map::action) and the map and bank registries (90 and 64 entries)
// itself; s_aWeapon stays where it was. Layouts: SoldierAnimator.h and
// combo_anim_layout.hpp; research: docs/RE/SoldierActionAnimations.md.
// =============================================================================

namespace soldier_anim_tables {

// The most maps or banks either layout holds.
constexpr int kMaxMapsAny  = 90;
constexpr int kMaxBanksAny = 64;
constexpr int kMaxWeapons  = 20;
constexpr int kNameBuffer  = 33;   // a 32-byte table name and its NUL

// A slot's value for "never assigned"; read as no animation.
constexpr uintptr_t kUnassigned = 0xFFFFFFFF;

// Resolves the stock addresses and, unless ComboAnimIncrease replaced them,
// checks the stock getters, whose code is the table's layout. Call from an
// installer, after the patch sets; idempotent. False when this build lacks them.
bool init(uintptr_t exe_base);
// Whether ComboAnimIncrease's layout is the one in use, for logs.
bool expanded();

int max_maps();    // 30, or 90 with ComboAnimIncrease
int max_banks();   // 16, or 64

// A map's bank and weapon; false for a map outside the registry.
bool map_key(int map, int& bank, int& weapon);
// A bank's or weapon's name, and its parent (itself for a root, -1 for none).
void bank_name(int bank, char (&out)[kNameBuffer]);
void weapon_name(int weapon, char (&out)[kNameBuffer]);
int bank_parent(int bank);
int weapon_parent(int weapon);

// The slot holding one half (0 upper, 1 lower) of a map's action animation, or
// null. Its value may be null or kUnassigned for none.
SoldierAnimation** action_slot(int map, int action, int half);
// That value as an animation, null for none.
SoldierAnimation* action_animation(int map, int action, int half);

// SoldierAnimatorClass::FindAnimation: the ZephyrAnim a PblTEMPHash names in
// any loaded soldier bank, or null.
void* find_animation(uint32_t hash);

// The ZephyrAnim for one half (0 upper, 1 lower) of an animation named like a
// map's action animations, found as AnimationFinder finds them: the map's
// weapon and then each parent weapon, each with the map's bank and then each
// parent bank, each by the half's three names (half_names), hashed with
// PblTEMPHash and looked up with find_animation. Null when no loaded bank has
// one. `name` gets the name it was found by, `match` which of the three.
void* find_named(int map, const char* anim, int half, char (&name)[kNameMax], Match& match);

} // namespace soldier_anim_tables
