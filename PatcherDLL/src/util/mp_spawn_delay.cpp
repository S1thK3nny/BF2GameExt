#include "pch.h"
#include "mp_spawn_delay.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// =============================================================================
// Multiplayer spawn delay ([Features] MPSpawnDelay).  See the header for what
// the engine does and docs/RE/SpawnDelaySystem.md for the full write-up.
// =============================================================================

// Install-time logging must not go through the engine's logger: every section is
// PAGE_READWRITE for the whole installer sequence, so calling into .text raises
// an EXEC access violation.  Same reason as voice_limit.cpp.
static void install_log(const char* fmt, ...)
{
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fclose(f);
}

float g_mpSpawnDelay = 15.0f;

namespace {

// What the engine hardcodes. Asking for it is not an error, there is simply
// nothing to write, so the sites are left alone.
constexpr float kStockDelay = 15.0f;

// A wave clock at zero makes SpawnManager::Update advance its cycle index every
// frame, and a negative one can never expire at all, so the low end stops just
// short of zero. Fractions are fine -- the value stays a float the whole way to
// mCycleDelay, so 7.5 is seven and a half seconds.
constexpr float kMinDelay = 0.1f;
constexpr float kMaxDelay = 300.0f;

// The float the retail operands are repointed at.  It has to outlive the patch,
// so it is a DLL global rather than anything scoped.
float g_delayStorage = 15.0f;

// How the constant reaches the delay slot differs by build:
//
//   modtools   MOV dword ptr [ESP],0x41700000     imm32 rewritten in place
//              C7 04 24 <imm32>
//
//   retail     MOVSS XMM1,[0x007B22B0]            disp32 repointed at our float,
//              F3 0F 10 0D <disp32>               because the literal is shared
//                                                 by ~50 unrelated call sites
enum class Kind { Imm32, Disp32 };

struct Site {
   uintptr_t va;        // start of the instruction
   uint8_t   opcode[4]; // leading bytes that must match
   uint8_t   opcodeLen; // 3 on modtools, 4 on retail
   const char* what;
};

struct BuildSites {
   Kind      kind;
   uint32_t  expectValue;  // imm32: the 15.0f bit pattern.  disp32: the literal's VA
   Site      site[2];
};

// 0x41700000 == 15.0f
constexpr BuildSites kModtools = {
   Kind::Imm32, 0x41700000,
   {
      {0x0046C3B0, {0xC7, 0x04, 0x24}, 3, "SetSpawnDelay"},
      {0x0046C470, {0xC7, 0x04, 0x24}, 3, "SetSpawnDelayTeam"},
   },
};

constexpr BuildSites kSteam = {
   Kind::Disp32, 0x007B22B0,
   {
      {0x0058C655, {0xF3, 0x0F, 0x10, 0x0D}, 4, "SetSpawnDelay"},
      {0x0058C6F4, {0xF3, 0x0F, 0x10, 0x0D}, 4, "SetSpawnDelayTeam"},
   },
};

// Ported from Steam with tools/port_gog.py (score 1.00) and read back from the
// image; the instructions are byte-identical apart from the literal's address.
constexpr BuildSites kGOG = {
   Kind::Disp32, 0x007B3228,
   {
      {0x0058D605, {0xF3, 0x0F, 0x10, 0x0D}, 4, "SetSpawnDelay"},
      {0x0058D6A4, {0xF3, 0x0F, 0x10, 0x0D}, 4, "SetSpawnDelayTeam"},
   },
};

const BuildSites* sites_for_build()
{
   switch (g_build) {
   case GameBuild::Modtools: return &kModtools;
   case GameBuild::Steam:    return &kSteam;
   case GameBuild::GOG:      return &kGOG;
   default:                  return nullptr;
   }
}

} // namespace

void mp_spawn_delay_install(uintptr_t exe_base)
{
   float delay = g_mpSpawnDelay;
   if (delay < kMinDelay) delay = kMinDelay;
   if (delay > kMaxDelay) delay = kMaxDelay;
   if (delay != g_mpSpawnDelay)
      install_log("[MPSpawnDelay] %.2f is out of range (%.1f to %.0f), using %.2f\n",
                  g_mpSpawnDelay, kMinDelay, kMaxDelay, delay);

   // Nothing to do when the host wants what the engine already does. Skipping
   // keeps a default install from touching these bytes at all.
   if (delay == kStockDelay) return;

   const BuildSites* build = sites_for_build();
   if (!build) {
      install_log("[MPSpawnDelay] unidentified build, skipping\n");
      return;
   }

   // The expected operand value, as it will actually read at runtime: an imm32 is
   // a literal, but a disp32 holds a pointer the loader has already rebased.
   const uint32_t expect = (build->kind == Kind::Imm32)
                              ? build->expectValue
                              : (uint32_t)(uintptr_t)resolve(exe_base, build->expectValue);

   // Verify both sites before writing either.  Half of this applied would leave
   // SetSpawnDelay and SetSpawnDelayTeam disagreeing about the same team's delay,
   // which is worse than not applying at all.
   for (const Site& site : build->site) {
      const uint8_t* p = (const uint8_t*)resolve(exe_base, site.va);
      if (memcmp(p, site.opcode, site.opcodeLen) != 0 ||
          memcmp(p + site.opcodeLen, &expect, sizeof(expect)) != 0) {
         install_log("[MPSpawnDelay] site mismatch at %s (%08X), feature disabled\n",
                     site.what, (uint32_t)site.va);
         return;
      }
   }

   g_delayStorage = delay;

   // imm32: the new constant.  disp32: the address of the float above, so the
   // shared 15.0f literal every other caller depends on is left untouched.
   uint32_t replacement;
   if (build->kind == Kind::Imm32)
      memcpy(&replacement, &delay, sizeof(replacement));
   else
      replacement = (uint32_t)(uintptr_t)&g_delayStorage;

   for (const Site& site : build->site) {
      uint8_t* p = (uint8_t*)resolve(exe_base, site.va);
      memcpy(p + site.opcodeLen, &replacement, sizeof(replacement));
   }

   install_log("[MPSpawnDelay] multiplayer respawn delay set to %.2fs (stock %.2fs)\n",
               delay, kStockDelay);
}
