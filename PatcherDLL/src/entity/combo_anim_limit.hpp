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

// Crash-only diagnostics for the verified Modtools lower-movement caller.
// Returns bytes written, excluding the terminator; zero for unrelated faults.
size_t combo_anim_limit_crash_details(uintptr_t fault, uintptr_t frame, uintptr_t animator,
                                      unsigned movement, char* output, size_t capacity);
