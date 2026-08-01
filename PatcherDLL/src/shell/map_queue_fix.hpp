#pragma once

#include <stdint.h>

// Map-queue next-mission fix (modtools only).
//
// Finishing a match in the modtools build always returns to the main menu, even
// when the mission playlist still has maps queued.  GameLoop::UpdateStats is
// missing the "playlist advanced -> enter MissionState" branch that Phantom and
// retail both emit.  Restores it.  Always on: this is a missing branch, not a
// behaviour choice.

void map_queue_fix_install(uintptr_t exe_base);
void map_queue_fix_uninstall();
