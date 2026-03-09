#pragma once

#include <stdint.h>

// Doubles EntityDoor hash table from 32 to 64 buckets.
// INI: [Patches] DoorLimitIncrease=1
void patch_door_limit(uintptr_t exe_base);
void unpatch_door_limit();
