#include "pch.h"
#include "hero_team_switch_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstring>

// =============================================================================
// "Cannot switch teams after dying as a hero" fix.
//
// The bug, traced in the Phantom build where the whole path is symbolized:
//
//   Character::mHeroFlag (Character+0x165) has exactly one writer in the entire
//   exe, at the end of Character::Spawn:
//
//       this->mHeroFlag = (unit->flags >> 1) & 1;   // "spawned as a hero"
//
//   Nothing clears it.  Dying routes through Unlink() -> Character::SetUnit(NULL)
//   and then Character::Respawn, which resets mUnit, mVehicle and mCarriedFlagPtr
//   and leaves mHeroFlag exactly as the last spawn left it.  So between a hero's
//   death and their next spawn the character is flagged as a hero while holding
//   no unit at all.
//
//   Character::ChangeTeam opens with:
//
//       if (mHeroFlag || mTeamNumber == newTeam || target team full)
//           return false;
//
//   which is the whole bug: the team switch is refused for as long as the stale
//   flag is set.  Character::SelectTeam - the only path the team-select screen
//   uses - drops out silently when ChangeTeam returns false, so the request just
//   does nothing.  The next Character::Spawn reassigns mHeroFlag from whatever
//   unit actually spawned, which is why respawning as a regular unit restores
//   team switching and respawning as the hero again does not.
//
// The fix qualifies the refusal with "and the character currently has a unit"
// (mUnit, Character+0x148, is NULL for the whole time a character is dead).  A
// live hero is still refused, exactly as before; a dead one is not.  Nothing is
// written to mHeroFlag, so every other reader keeps seeing vanilla state - which
// matters, because Character::Update deliberately reads the flag on a *dead* AI
// character to exempt a dead AI hero from the netNumBots cap.
//
// Leaving mHeroFlag alone is also what the engine itself does: Character::IsHero
// answers the "is this dead player the hero" question through
// netHeroLastPlayerID rather than through mHeroFlag, so the flag is only ever
// meaningful while a unit exists.
//
// Codegen at the patch site.  Both builds test the same byte and jump to the
// same "return false" epilogue; only the register and the encoding differ.
//
//   modtools 0x643480 Character::ChangeTeam:
//       00643483  MOV EDI,ECX
//       00643485  MOV AL,[EDI+0x165]      } patch site (6 bytes), `this` = EDI
//       0064348b  TEST AL,AL              <- resume
//       0064348d  JNZ 0x6434dc            <- return false
//
//   Steam 0x452330 / GOG 0x452310 Character::ChangeTeam:
//       00452335  MOV ESI,ECX
//       00452338  CMP byte ptr [ESI+0x165],0  } patch site (7 bytes), `this` = ESI
//       0045233f  JNZ 0x45238e                <- resume / return false
//
// The site is displaced into a code cave that checks mUnit first and, when it is
// NULL, substitutes a zeroed AL - which satisfies both resumes at once, since
// XOR AL,AL leaves AL == 0 for modtools' TEST and ZF == 1 for retail's JNZ.
// =============================================================================

static uint8_t* g_site     = nullptr;
static uint8_t  g_siteOrig[8] = {};
static size_t   g_siteLen  = 0;
static uint8_t* g_cave     = nullptr;

// Character field offsets (identical on all three builds).
static constexpr uint32_t kOffMUnit    = 0x148;
static constexpr uint32_t kOffHeroFlag = 0x165;

// The displaced hero-flag test, per build.
static const uint8_t kOrigModtools[] = {0x8A, 0x87, 0x65, 0x01, 0x00, 0x00};       // MOV AL,[EDI+0x165]
static const uint8_t kOrigRetail[]   = {0x80, 0xBE, 0x65, 0x01, 0x00, 0x00, 0x00}; // CMP byte ptr [ESI+0x165],0

// What must follow the site, to positively identify it before patching.
static const uint8_t kNextModtools[] = {0x84, 0xC0, 0x75}; // TEST AL,AL ; JNZ rel8
static const uint8_t kNextRetail[]   = {0x75};             // JNZ rel8

// ModRM byte for `CMP dword ptr [reg+disp32],0` (opcode 0x83 /7): mod=10, reg=7.
static constexpr uint8_t kModrmEdi = 0xBF;
static constexpr uint8_t kModrmEsi = 0xBE;

void hero_team_switch_fix_install(uintptr_t exe_base)
{
   const uint8_t* orig;
   size_t         origLen;
   const uint8_t* next;
   size_t         nextLen;
   uint8_t        modrm;

   switch (g_build) {
   case GameBuild::Modtools:
      orig = kOrigModtools; origLen = sizeof(kOrigModtools);
      next = kNextModtools; nextLen = sizeof(kNextModtools);
      modrm = kModrmEdi;
      break;
   case GameBuild::Steam:
   case GameBuild::GOG:
      orig = kOrigRetail;   origLen = sizeof(kOrigRetail);
      next = kNextRetail;   nextLen = sizeof(kNextRetail);
      modrm = kModrmEsi;
      break;
   default:
      return; // unknown build
   }

   if (g_addr->character_change_team_hero_test == 0) return;

   uint8_t* site = (uint8_t*)resolve(exe_base, g_addr->character_change_team_hero_test);

   // Bail (no-op) unless both the test and the branch that consumes it match.
   if (std::memcmp(site, orig, origLen) != 0 ||
       std::memcmp(site + origLen, next, nextLen) != 0) {
      get_gamelog()("[HeroTeamSwitchFix] unexpected bytes at ChangeTeam hero test, skipping\n");
      return;
   }

   uint8_t* cave = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT,
                                          PAGE_EXECUTE_READWRITE);
   if (!cave) return;

   uint8_t* const resume = site + origLen;

   //  +0             CMP dword ptr [this+0x148],0   ; mUnit
   //  +7             JZ  -> no unit: report "not a hero"
   //  +9             <displaced hero-flag test>
   //  +9+origLen     JMP resume
   //  +14+origLen    XOR AL,AL                      ; AL = 0 / ZF = 1
   //  +16+origLen    JMP resume
   int o = 0;
   cave[o++] = 0x83; cave[o++] = modrm;
   *(uint32_t*)(cave + o) = kOffMUnit; o += 4;
   cave[o++] = 0x00;
   cave[o++] = 0x74; cave[o++] = (uint8_t)(origLen + 5); // JZ over the test + its JMP
   std::memcpy(cave + o, orig, origLen);
   o += (int)origLen;
   cave[o++] = 0xE9;
   *(int32_t*)(cave + o) = (int32_t)(resume - (cave + o + 4));
   o += 4;
   cave[o++] = 0x32; cave[o++] = 0xC0;                   // XOR AL,AL
   cave[o++] = 0xE9;
   *(int32_t*)(cave + o) = (int32_t)(resume - (cave + o + 4));
   o += 4;

   // JMP cave + NOP padding to fill the site exactly.  .text is RW during
   // install (dllmain re-protects afterwards), so no VirtualProtect here.
   std::memcpy(g_siteOrig, site, origLen);
   site[0] = 0xE9;
   *(int32_t*)(site + 1) = (int32_t)(cave - (site + 5));
   std::memset(site + 5, 0x90, origLen - 5);

   g_site    = site;
   g_siteLen = origLen;
   g_cave    = cave;
}

void hero_team_switch_fix_uninstall()
{
   // Sections are re-protected by the time this runs, so the restore cannot be
   // a plain write.
   if (g_site) {
      protected_write(g_site, g_siteOrig, g_siteLen);
      g_site = nullptr;
   }
   if (g_cave) {
      VirtualFree(g_cave, 0, MEM_RELEASE);
      g_cave = nullptr;
   }
}
