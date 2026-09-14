#pragma once

#include <stdint.h>

// Enable a cannon ODF's HeldOrdnanceEffectBone and transfer the same TrailEffect
// to the released ordnance. Does not change damage, salvo timing, or animation.
extern bool g_heldOrdnanceEffectEnabled;
void held_ordnance_effect_install(uintptr_t exe_base);
void held_ordnance_effect_uninstall();

// Called after the engine resets its level. Old engine pointers are invalid:
// discard our bookkeeping without querying or calling an old effect.
void held_ordnance_effect_reset();
