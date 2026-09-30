#pragma once

#include <stdint.h>

// =============================================================================
// Team. Team_data starts at +0x04. Offsets read off each build's instructions:
//
//   field                    modtools @               Steam @ / GOG @ (same code)
//   Team (Team::sTeams: game_addrs team_array_base, a pointer to the array)
//   +0x1C mIcon (a PblHash)  SetTeamIcon 0x00470302        0x0058B883 / 0x0058C833
//   +0x68 mColor[8] (RedColor, as this team sees team i)
//                            post icons 0x006A8439         0x0055469B / 0x0055540B
//
// Team::sTeams[0] is the neutral team.
// =============================================================================

namespace layout::Team {

constexpr uint32_t kIcon  = 0x1C;
constexpr uint32_t kColor = 0x68;   // RedColor[8]
constexpr int      kColorCount = 8;

} // namespace layout::Team
