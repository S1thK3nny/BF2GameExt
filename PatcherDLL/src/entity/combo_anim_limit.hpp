#pragma once

#include "combo_anim_layout.hpp"

// Called by the numeric patch set after all its expected bytes have matched.
// Failure leaves the storage hooks and numeric limits unchanged. These patches
// have process lifetime, like the other engine array relocations.
bool combo_anim_limit_install(uintptr_t exe_base);
bool combo_anim_limit_active();

// Shared with the always-on damage guard; never call an old-map trampoline when
// the expanded contract is active.
void* combo_anim_limit_get_body(void* owner, int map, int index, bool lower);
int combo_anim_limit_map_count();

// While installed: the registries that stand in for SoldierAnimationBank's
// s_aMap and s_aBank (kComboMapCount and kComboBankCount entries), and the slot
// holding one half of a map's action animation (action < 38). Null when not
// installed, or for a map or action out of range. Read by soldier_anim_tables.
const combo_map_key* combo_anim_limit_map_registry();
const combo_anim_bank* combo_anim_limit_bank_registry();
void** combo_anim_limit_action_slot(int map, int action, bool lower);

// Crash-only diagnostics for the verified Modtools lower-movement caller.
// Returns bytes written, excluding the terminator; zero for unrelated faults.
size_t combo_anim_limit_crash_details(uintptr_t fault, uintptr_t frame, uintptr_t animator,
                                      unsigned movement, char* output, size_t capacity);
