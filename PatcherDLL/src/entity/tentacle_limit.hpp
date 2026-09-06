#pragma once

#include <stdint.h>

// Tentacle limit increase: 4 -> 9 tentacles per soldier class.
// INI: [LimitIncreases] TentacleLimit. See entity/tentacle_limit.cpp.
extern bool g_tentacleLimitEnabled;

void tentacle_limit_install(uintptr_t exe_base);
void tentacle_limit_uninstall();
