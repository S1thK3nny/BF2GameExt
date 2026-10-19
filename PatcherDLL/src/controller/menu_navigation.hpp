#pragma once

#include <stdint.h>

// Gamepad navigation for the menus, the shell screens and the spawn screen.
// Gated on [Controller] Enabled. Install inside dllmain's RW window.
void menu_navigation_install(uintptr_t exe_base);
void menu_navigation_uninstall();
