#pragma once
#include <cstdint>

// Uses the already-installed shared GameEvents Open/Update hooks. It adds no
// detour and is inert unless a HUD declares TransformNumberMath. Resolve only
// after those hooks install successfully; never call engine code during install.
void hud_number_math_resolve(uintptr_t exe_base);
void hud_number_math_open();
void hud_number_math_update();
