#pragma once

#include <stdint.h>

// The HUD's team icons in every mode. The game sends player%d.team1.texture
// (your side) and player%d.team2.texture (the other side) each time the player
// joins a side, but only while the mission's flag gameplay type is 1flag or
// 2flag, so a HUD that shows them in conquest or hunt gets nothing. With this
// on they are sent in every mode but Space Assault, where the same update
// switches both sides to points and sends no icon.
//
// INI: [Features] HudTeamTextureAlways = 1 (the default) or 0 (stock).
// Stays set only if the test was patched.
extern bool g_hudTeamTextureAlways;

void hud_team_texture_install(uintptr_t exe_base);
void hud_team_texture_uninstall();
