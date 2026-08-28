#pragma once

#include <stdint.h>

// Disables the leftover in-game HUD editor on the retail (Steam / GOG) builds.
// The modtools editor is untouched.
void hud_editor_disable_install(uintptr_t exe_base);
void hud_editor_disable_uninstall();
