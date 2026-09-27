#pragma once

#include <stdint.h>

// =============================================================================
// CommandPost, CommandPostClass and Team - what the command post strip reads.
// Names from the Phantom PDB (CommandPost_data starts at +0x18, Team_data at
// +0x04); every offset read off each build's own instructions:
//
//   field                    modtools @               Steam @ / GOG @ (same code)
//   CommandPost
//   +0x1C mHUDPostIndex      HUDIndex store 0x0064C47A    0x0047BA21
//   +0x2C mObject handle     GetCommandPostTeam 0x004730C3  0x0058FFDF / 0x00590F7F
//   +0x40 mHoldTeam          Update reset 0x0064E302      0x0047CE7F
//   +0x78 mBiasTeam          Update 0x0064E7F3            0x0047D416
//   +0xA0 mNeutralizeTimer   Update 0x0064EE09            0x0047DF3F
//   +0xA4 mCaptureTimer      Update 0x0064E904            0x0047D76E
//   flags byte, bit 1 = HUDIndexDisplay
//         0x1A58             store 0x0064C948
//         0x0B40                                          store 0x0047BED6
//   mClass: game_addrs command_post_class_off (0x1A54 modtools, 0x0B3C retail).
//
//   CommandPostClass
//   +0x04 mNeutralizeTime    GetNeutralizeTime 0x00649A80  NeutralizeTime store 0x0047F1DB
//   +0x08 mCaptureTime       GetCaptureTime 0x00649A70     CaptureTime store 0x0047F1BE
//
//   Team (Team::sTeams: game_addrs team_array_base, a pointer to the array)
//   +0x1C mIcon (a PblHash)  SetTeamIcon 0x00470302        0x0058B883 / 0x0058C833
//   +0x68 mColor[8] (RedColor, as this team sees team i)
//                            post icons 0x006A8439         0x0055469B / 0x0055540B
//
//   GameObject
//   +0x234 team, low 4 bits signed   GetCommandPostTeam 0x004730F7  0x00590035 / 0x00590FD5
//
// HUDIndexDisplay defaults on in CommandPostClass's constructor and is copied
// to each post; stock ODFs turn it off for invisible posts and command vehicles.
// A post's team is its object's team; Team::sTeams[0] is the neutral team, which
// the engine itself dereferences for neutral posts.
// =============================================================================

namespace layout::CommandPost {

constexpr uint32_t kHudIndex        = 0x1C;
constexpr uint32_t kObject          = 0x2C;   // PblHandle<GameObject>: object, id
constexpr uint32_t kHoldTeam        = 0x40;   // 0 none, -1 more than one team
constexpr uint32_t kBiasTeam        = 0x78;   // the team the capture timer counts for
constexpr uint32_t kNeutralizeTimer = 0xA0;
constexpr uint32_t kCaptureTimer    = 0xA4;
constexpr uint32_t kFlagsModtools   = 0x1A58;
constexpr uint32_t kFlagsRelease    = 0x0B40;
constexpr uint8_t  kFlagHudIndexDisplay = 0x02;

constexpr uint32_t kClassNeutralizeTime = 0x04;
constexpr uint32_t kClassCaptureTime    = 0x08;

} // namespace layout::CommandPost

namespace layout::Team {

constexpr uint32_t kIcon  = 0x1C;
constexpr uint32_t kColor = 0x68;   // RedColor[8]
constexpr int      kColorCount = 8;

} // namespace layout::Team
