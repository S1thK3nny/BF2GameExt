#include "pch.h"
#include "hud_team_texture.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <cstring>

// =============================================================================
// Read on each build. The HUD's per-player team update (modtools 0x006B1870,
// Steam 0x00562690, GOG 0x00563410; its one caller is the HUD events' Update,
// modtools 0x006B50A0) sends the two team icons each time the player's side
// changes, team1 being the player's own side and team2 the other:
//
//     if (!spaceAssault || !spaceAssault->enabled) {
//        if (sFlagGameplayType == 2 || sFlagGameplayType == 3) {   // 1flag, 2flag
//           texture = find(showsPoints ? team->pointsIcon : team->reinforcementsIcon)
//                  ?: find(team->icon);
//           if (texture) send player%d.team%d.texture
//        }
//     } else {
//        ShowTeamPoints(team, true);          // Space Assault: points, no icon
//     }
//
// SetTeamIcon(team, icon, reinforcementsIcon, pointsIcon) fills the three
// names (team +0x1C, +0x20, +0x24; the last two default to the first), and
// ShowTeamPoints picks which of the last two a side shows. The stock
// setup_teams.lua passes "hud_reinforcement_icon" and "flag_icon". The test
// is the only thing that keeps the icons out of conquest and hunt:
//
//                                       modtools    Steam       GOG
//   MOV EAX,[sFlagGameplayType]         0x006B190A  0x00562712  0x00563492
//   CMP EAX,2 / JE send                 0x006B190F  0x00562717  0x00563497
//   CMP EAX,3 / JNE skip
//   send: the [ESI+4] showsPoints test  0x006B191D  0x00562721  0x005634A1
//
// With HudTeamTextureAlways on, the CMP EAX,2 becomes JMP send: EB and the
// JE's displacement plus 3, as the jump is 2 bytes at the CMP and the JE
// ends 5 bytes on. Only an opcode and a displacement are written, nothing a
// retail load moves; the flag type's address in the MOV is, so the guard
// reads around it.
// =============================================================================

bool g_hudTeamTextureAlways = false;

namespace {

// CMP EAX,2 / JE rel8 / CMP EAX,3, then the JNE: 0F 85 rel32 on modtools,
// 75 rel8 on retail.
constexpr uint8_t kTest[]          = { 0x83, 0xF8, 0x02, 0x74 };
constexpr uint8_t kTestThree[]     = { 0x83, 0xF8, 0x03 };
constexpr uint8_t kJneModtools[]   = { 0x0F, 0x85 };
constexpr uint8_t kJneRetail[]     = { 0x75 };
// The first instruction of the send: modtools MOV AL,[ESI+4] / TEST AL,AL,
// retail CMP byte [ESI+4],0.
constexpr uint8_t kSendModtools[]  = { 0x8A, 0x46, 0x04, 0x84, 0xC0 };
constexpr uint8_t kSendRetail[]    = { 0x80, 0x7E, 0x04, 0x00 };

constexpr size_t kPatchLen = 2;

uint8_t* s_site = nullptr;
uint8_t  s_orig[kPatchLen] = {};

bool site_matches(const uint8_t* site, bool modtools)
{
   if (site[-5] != 0xA1) return false;                                 // MOV EAX,[imm32]
   if (std::memcmp(site, kTest, sizeof(kTest)) != 0) return false;
   const uint8_t rel = site[4];
   if (rel > 0x7F - 3) return false;                                   // JMP's must stay a forward rel8
   if (std::memcmp(site + 5, kTestThree, sizeof(kTestThree)) != 0) return false;
   const uint8_t* jne  = modtools ? kJneModtools : kJneRetail;
   const size_t   jneN = modtools ? sizeof(kJneModtools) : sizeof(kJneRetail);
   if (std::memcmp(site + 8, jne, jneN) != 0) return false;
   const uint8_t* send = site + 5 + rel;
   return modtools ? std::memcmp(send, kSendModtools, sizeof(kSendModtools)) == 0
                   : std::memcmp(send, kSendRetail, sizeof(kSendRetail)) == 0;
}

} // namespace

void hud_team_texture_install(uintptr_t exe_base)
{
   if (!g_hudTeamTextureAlways) return;
   g_hudTeamTextureAlways = false;
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   const uintptr_t va = g_addr->hud_team_texture_flag_test;
   if (va == 0) {
      install_log("[HudTeamTextureAlways] NOT installed: no address set for this build");
      return;
   }
   uint8_t* site = static_cast<uint8_t*>(resolve(exe_base, va));
   if (!site_matches(site, modtools)) {
      install_log("[HudTeamTextureAlways] NOT installed: the flag type test at 0x%08X is not as expected",
                  (unsigned)va);
      return;
   }
   const uint8_t jmp[kPatchLen] = { 0xEB, static_cast<uint8_t>(site[4] + 3) };
   std::memcpy(s_orig, site, kPatchLen);
   // .text is RW for the whole install window (dllmain re-protects afterwards).
   std::memcpy(site, jmp, kPatchLen);
   s_site = site;
   g_hudTeamTextureAlways = true;
   install_log("[HudTeamTextureAlways] installed: the team icon events are sent in every mode "
               "(flag type test 0x%08X)", (unsigned)va);
}

void hud_team_texture_uninstall()
{
   // Sections are re-protected by the time this runs.
   if (!s_site) return;
   protected_write(s_site, s_orig, kPatchLen);
   s_site = nullptr;
}
