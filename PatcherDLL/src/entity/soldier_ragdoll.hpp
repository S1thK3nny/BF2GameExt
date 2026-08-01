#pragma once

#include <stdint.h>

// =============================================================================
// Soldier ragdoll - Verlet bone simulation for dead soldiers.
//
//   INI: [Features] SoldierRagdoll=1      (default 0)
//
// Replaces the canned death animation's pose with a simulated one: every
// skeleton joint becomes a Verlet particle, bones become distance constraints,
// and the result is written back over the world matrices the animator just
// built.  Purely cosmetic and entirely client-local - no net state, no engine
// physics, no ODF or asset changes, and it works on any soldier skeleton
// because the constraint rest lengths are measured off the live pose.
//
// Modtools only for now; Steam/GOG addresses are not yet ported, so the
// installer no-ops there.
// =============================================================================

extern bool g_soldierRagdollEnabled;

// [Features] SoldierRagdollDebug=1 - dumps the seed configuration (joint count,
// hierarchy, model-space positions, contact plane) for the first corpse of each
// level to the game log.
extern bool g_soldierRagdollDebug;

void soldier_ragdoll_install(uintptr_t exe_base);
void soldier_ragdoll_uninstall();

// Drops all live ragdoll state.  Slots are keyed by SoldierAnimator*, which come
// from a pool and are reused across levels, so stale entries would graft one
// corpse's simulation onto an unrelated soldier.  Called from lua_hooks'
// hooked_init_state() on modtools.
void soldier_ragdoll_reset();
