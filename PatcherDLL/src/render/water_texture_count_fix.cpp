#include "pch.h"
#include "water_texture_count_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstring>

// =============================================================================
// RedWater animated-texture frame count sanitizer.
//
// A world's Water() effect animates the surface through a sequence of textures:
//
//     NormalMapTextures("<prefix>", <count>, <speed>);
//     BumpMapTextures("<prefix>", <count>, <speed>);
//     SpecularMaskTextures("<prefix>", <count>, <speed>);
//
// RedWater::ReadConfig funnels all three (PblHash 0xEAC587AC / 0x11D65039 /
// 0xC0311180) into one handler, which takes <count> at face value and fills a
// fixed 50-entry static table with no bounds check:
//
//     CVTTSS2SI EAX,[ESI+0xc]        ; count, straight off the property
//     XOR  EDI,EDI
//     MOV  [<count>],EAX             ; <- our patch site (A3 imm32)
//     TEST EAX,EAX / JZ  done
//   loop:
//     ...sprintf("%s%d", prefix, EDI); look the texture up...
//     MOV  [EDI*4 + <array>],EAX     ; no bounds check
//     INC  EDI
//     CMP  EDI,[<count>]             ; bound is re-read from memory every pass
//     JC   loop
//
// The parsed property block (built by FUN_00727E30) is:
//
//     +0x00  PblHash
//     +0x04  argument count      (dword, sign-extended from a byte)
//     +0x08  arg0, +0x0C arg1, +0x10 arg2   (argCount dwords)
//     +8+4*argCount              the string data, copied straight after the args
//
// Two ways a world breaks this, both real:
//
//  1. A count above 50.  On both retail builds the three tables are laid out
//     back to back with each one's count immediately behind it (array + 0xC8 ==
//     count), so storing entry index 50 overwrites the loop bound with a
//     texture handle.  The bound is re-read every iteration, so the loop then
//     runs to that handle's value and scribbles dwords across .data and past
//     the end of the image until it hits an unmapped page.
//
//  2. The count argument missing entirely -- `NormalMapTextures("prefix_");`,
//     which is what several .fx round-trip tools emit.  Because the string is
//     copied directly after the last argument, argCount == 1 puts the prefix's
//     first four bytes exactly where the count is read from: "wate" reads as
//     7.2e22f.  Retail's SSE `CVTTSS2SI` returns the 32-bit integer indefinite
//     0x80000000 for that, which the `TEST EAX,EAX / JZ` treats as a live count
//     and the unsigned `JC` runs as two billion iterations.  Modtools converts
//     the same float through x87 `FISTP QWORD` and keeps the low dword, which
//     is 0 for the 64-bit indefinite, so it skips the loop and the level loads.
//     That -- not the table layout -- is why a broken .fx runs under modtools
//     and takes the retail exe down on load.
//
// Both are handled by sanitizing the count at the store.  The floor is 1, not
// 0: RedWater::pcAccumulateNormalMap picks the current frame with `DIV dword
// ptr [<count>]` (Steam 0x00720750 / 0x00720763) and nothing gates that call on
// the count, so writing 0 would trade the access violation for a divide error.
// One frame means a still normal map instead of an animated one.
//
// The `MOV [<count>],EAX` store is `A3 imm32`, exactly 5 bytes on all three
// builds; it is replaced with `E8 rel32` to a per-property stub.  The count
// global's address is read out of the displaced instruction's own imm32 operand
// at install time, so it needs no entry in the address registry.
//
// The stubs are register- and flag-transparent.  EAX carries the count in and
// the sanitized count out, and EFLAGS must survive: retail sets the flags it
// needs after the store (TEST EAX,EAX), but modtools sets them before it (TEST
// EAX,EAX / MOV / JBE) and relies on the MOV not disturbing them.  Sanitizing
// never turns a nonzero count into zero, so handing the original flags back is
// correct on both.
//
// Not handled, because it is not a crash: with exactly two arguments the count
// is real but the *speed* slot holds string data.  The engine multiplies it by
// elapsed time, overflows the same conversion, and ends up frozen on frame 0.
// The log line below is enough to point at the property.
// =============================================================================

// Entries the engine's static tables actually hold.  Identical on all three
// builds: array base + 0xC8 is the next global in every case.
static constexpr uint32_t kMaxWaterFrames = 50;

// The count argument is arg1, so anything below two arguments means the slot at
// +0xC is really the prefix string.
static constexpr int32_t kMinArgCount = 2;

enum WaterTexArray { kNormalMap = 0, kBumpMap = 1, kSpecularMask = 2, kWaterArrayCount = 3 };

static const char* const kPropName[kWaterArrayCount] = {
   "NormalMapTextures", "BumpMapTextures", "SpecularMaskTextures"
};

// Resolved from each patch site's displaced `MOV [<count>],EAX` operand.
static uint32_t* s_countNormal = nullptr;
static uint32_t* s_countBump   = nullptr;
static uint32_t* s_countSpec   = nullptr;

// One line per property per session.  ReadConfig runs on every level load, and
// a world that is broken is broken every time.
static bool s_warned[kWaterArrayCount] = {};

// Not static: the only callers are the naked stubs below, and an inline-asm
// reference is not always enough to keep a static function alive.
//
// `block` is the parsed property block (ESI at the patch site), `raw` is what
// the engine's own float-to-int conversion produced.  Returns the count the
// engine should actually use.
extern "C" uint32_t __cdecl water_count_sanitize(int idx, uint32_t raw, const uint32_t* block)
{
   const char* name = (idx >= 0 && idx < kWaterArrayCount) ? kPropName[idx] : "?";
   const bool  first = (idx >= 0 && idx < kWaterArrayCount) && !s_warned[idx];

   // Signed: FUN_00727E30 sign-extends the count byte, so a corrupt chunk can
   // present a negative argument count.
   const int32_t argCount = (int32_t)block[1];

   if (argCount < kMinArgCount) {
      if (first) {
         s_warned[idx] = true;
         warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
                      "[WaterTextureCountFix] Water() %s was given %d argument(s); it needs "
                      "(prefix, frameCount, speed). The frame count slot is holding the prefix "
                      "text, which crashes the retail exe on load. Using 1 frame -- fix the "
                      "world's .fx, e.g. %s(\"prefix_\", 16, 8.0).\n",
                      name, argCount, name);
      }
      return 1;
   }

   if (raw == 0) {
      if (first) {
         s_warned[idx] = true;
         warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
                      "[WaterTextureCountFix] Water() %s asked for 0 frames; the water renderer "
                      "divides by that. Using 1.\n", name);
      }
      return 1;
   }

   if (raw > kMaxWaterFrames) {
      if (first) {
         s_warned[idx] = true;
         warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
                      "[WaterTextureCountFix] Water() %s asked for %u frames; the engine only "
                      "stores %u. Clamped -- lower the count in the world's .fx.\n",
                      name, raw, kMaxWaterFrames);
      }
      return kMaxWaterFrames;
   }

   return raw;
}

// Each stub replaces one 5-byte `MOV [<count>],EAX`.  pushad/pushfd make the
// C call transparent; the sanitized result is written over the saved EAX slot
// (pushad order puts it at [esp+0x1C]) so popad delivers it to the engine.

__declspec(naked) static void stub_normalmap()
{
   __asm {
      pushfd
      pushad
      push esi                        // parsed property block
      push eax                        // raw count
      push 0                          // kNormalMap
      call water_count_sanitize
      add  esp, 12
      mov  [esp + 0x1C], eax          // saved EAX inside the pushad frame
      popad
      mov  ecx, [s_countNormal]
      mov  dword ptr [ecx], eax
      popfd
      ret
   }
}

__declspec(naked) static void stub_bumpmap()
{
   __asm {
      pushfd
      pushad
      push esi
      push eax
      push 1                          // kBumpMap
      call water_count_sanitize
      add  esp, 12
      mov  [esp + 0x1C], eax
      popad
      mov  ecx, [s_countBump]
      mov  dword ptr [ecx], eax
      popfd
      ret
   }
}

__declspec(naked) static void stub_specularmask()
{
   __asm {
      pushfd
      pushad
      push esi
      push eax
      push 2                          // kSpecularMask
      call water_count_sanitize
      add  esp, 12
      mov  [esp + 0x1C], eax
      popad
      mov  ecx, [s_countSpec]
      mov  dword ptr [ecx], eax
      popfd
      ret
   }
}

// Bytes bracketing the store, verified before patching so this silently no-ops
// on any build whose codegen differs.
//   retail   CVTTSS2SI EAX,[ESI+0xc] ; XOR EDI,EDI | A3 imm32 | TEST EAX,EAX
//   modtools XOR EDI,EDI ; TEST EAX,EAX            | A3 imm32 | JBE +0x3e
static constexpr uint8_t kPrefixRetail[]   = {0xF3, 0x0F, 0x2C, 0x46, 0x0C, 0x33, 0xFF};
static constexpr uint8_t kSuffixRetail[]   = {0x85, 0xC0};
static constexpr uint8_t kPrefixModtools[] = {0x33, 0xFF, 0x85, 0xC0};
static constexpr uint8_t kSuffixModtools[] = {0x76, 0x3E};

static constexpr size_t kSiteLen = 5;  // A3 imm32 == E8 rel32

static uint8_t* g_site[kWaterArrayCount]               = {};
static uint8_t  g_siteOrig[kWaterArrayCount][kSiteLen] = {};

void water_texture_count_fix_install(uintptr_t exe_base)
{
   const uint8_t* prefix;
   size_t         prefixLen;
   const uint8_t* suffix;

   switch (g_build) {
   case GameBuild::Modtools:
      prefix = kPrefixModtools; prefixLen = sizeof(kPrefixModtools); suffix = kSuffixModtools;
      break;
   case GameBuild::Steam:
   case GameBuild::GOG:
      prefix = kPrefixRetail;   prefixLen = sizeof(kPrefixRetail);   suffix = kSuffixRetail;
      break;
   default:
      return;
   }

   struct SiteDesc {
      uintptr_t   addr;
      void      (*stub)();
      uint32_t**  countSlot;
   };
   const SiteDesc sites[kWaterArrayCount] = {
      {g_addr->water_normalmap_count_store,    stub_normalmap,    &s_countNormal},
      {g_addr->water_bumpmap_count_store,      stub_bumpmap,      &s_countBump  },
      {g_addr->water_specularmask_count_store, stub_specularmask, &s_countSpec  },
   };

   for (int i = 0; i < kWaterArrayCount; ++i) {
      if (sites[i].addr == 0) continue;

      uint8_t* site = (uint8_t*)resolve(exe_base, sites[i].addr);

      if (site[0] != 0xA3 ||
          std::memcmp(site - prefixLen, prefix, prefixLen) != 0 ||
          std::memcmp(site + kSiteLen, suffix, 2) != 0) {
         get_gamelog()("[WaterTextureCountFix] unexpected bytes at the %s count store, skipping\n",
                       kPropName[i]);
         continue;
      }

      // The displaced instruction's operand is the count global, already
      // relocated by the loader.
      uint32_t* countPtr = *(uint32_t**)(site + 1);
      if (!countPtr) continue;
      *sites[i].countSlot = countPtr;

      std::memcpy(g_siteOrig[i], site, kSiteLen);
      site[0] = 0xE8;
      *(int32_t*)(site + 1) = (int32_t)((uint8_t*)sites[i].stub - (site + kSiteLen));
      g_site[i] = site;
   }
}

void water_texture_count_fix_uninstall()
{
   // Sections are re-protected by the time this runs, so the restore cannot be
   // a plain write.
   for (int i = 0; i < kWaterArrayCount; ++i) {
      if (!g_site[i]) continue;
      protected_write(g_site[i], g_siteOrig[i], kSiteLen);
      g_site[i] = nullptr;
   }
}
