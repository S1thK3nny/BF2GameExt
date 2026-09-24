#include "pch.h"
#include "light_projected_texture_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <cstring>

// =============================================================================
// See the header for what this fixes.  The engine detail:
//
// EntityLightClass::SetProperty, ProjectedTexture branch (PblHash 0x99E6DBFE).
// The name and the frame count come in as one property, the array is sized from
// the count, and then:
//
//   modtools (0x0051C770 + ...), loop index in EDI:
//     loop:
//       8B 86 E4 00 00 00   MOV  EAX,[ESI+0xE4]     ; <- _uiNumFrames, THE BUG
//       50                  PUSH EAX                ; the %d argument
//       53                  PUSH EBX                ; arg index 0
//       ...                 CALL PblConfig::Data::GetStringArg
//       50                  PUSH EAX                ; the %s argument
//       68 ...              PUSH "%s%d"
//       51                  PUSH ECX                ; destination buffer
//       ...                 CALL sprintf
//       ...                 CALL PblHash
//       89 14 B9            MOV  [ECX + EDI*4],EDX  ; EDI is the real index
//       8B 86 E4 00 00 00   MOV  EAX,[ESI+0xE4]
//       47                  INC  EDI
//       3B F8               CMP  EDI,EAX
//       72 B2               JC   loop
//
//   Steam / GOG (0x004CE690, byte-identical on both), loop index in ESI:
//     loop:
//       FF B7 C4 00 00 00   PUSH dword ptr [EDI+0xC4]   ; <- _uiNumFrames, THE BUG
//       8D 44 24 34         LEA  EAX,[ESP+0x34]
//       50                  PUSH EAX                    ; the %s argument
//       ...                 PUSH "%s%d" / dest / CALL sprintf
//       ...                 CALL PblHash
//       89 04 B1            MOV  [ECX + ESI*4],EAX      ; ESI is the real index
//       46                  INC  ESI
//       3B B7 C4 00 00 00   CMP  ESI,[EDI+0xC4]
//       72 BA               JC   loop
//
// Both buggy instructions are six bytes and both are the first instruction of
// the loop body (the backward jump lands exactly on them), so the replacement
// has to be six bytes and has to stay a valid instruction boundary:
//
//   modtools  8B 86 E4 00 00 00  ->  8B C7 90 90 90 90   MOV EAX,EDI + NOPs
//   retail    FF B7 C4 00 00 00  ->  56 90 90 90 90 90   PUSH ESI   + NOPs
//
// The count is re-read from memory for the loop condition further down, so
// clobbering EAX (modtools) costs nothing.
// =============================================================================

namespace {

constexpr size_t kSiteLen = 6;

// Bytes we require at the site before touching it, and what replaces them.
constexpr uint8_t kOrigModtools[kSiteLen] = {0x8B, 0x86, 0xE4, 0x00, 0x00, 0x00};
constexpr uint8_t kFixModtools[kSiteLen]  = {0x8B, 0xC7, 0x90, 0x90, 0x90, 0x90};

constexpr uint8_t kOrigRetail[kSiteLen] = {0xFF, 0xB7, 0xC4, 0x00, 0x00, 0x00};
constexpr uint8_t kFixRetail[kSiteLen]  = {0x56, 0x90, 0x90, 0x90, 0x90, 0x90};

// The instruction that follows, checked as well so a wrong address cannot match
// on the six bytes alone: PUSH EAX on modtools, LEA EAX,[ESP+0x34] on retail.
constexpr uint8_t kNextModtools[] = {0x50, 0x53};
constexpr uint8_t kNextRetail[]   = {0x8D, 0x44, 0x24, 0x34, 0x50};

uint8_t* g_site = nullptr;
uint8_t  g_siteOrig[kSiteLen] = {};

} // namespace

void light_projected_texture_fix_install(uintptr_t exe_base)
{
   if (g_build == GameBuild::Unknown) return;
   if (!g_addr->light_projected_texture_frame) return;

   const uint8_t* orig;
   const uint8_t* fix;
   const uint8_t* next;
   size_t         nextLen;

   switch (g_build) {
   case GameBuild::Modtools:
      orig = kOrigModtools; fix = kFixModtools;
      next = kNextModtools; nextLen = sizeof(kNextModtools);
      break;
   case GameBuild::Steam:
   case GameBuild::GOG:
      orig = kOrigRetail;   fix = kFixRetail;
      next = kNextRetail;   nextLen = sizeof(kNextRetail);
      break;
   default:
      return;
   }

   uint8_t* site = (uint8_t*)resolve(exe_base, g_addr->light_projected_texture_frame);

   if (std::memcmp(site, orig, kSiteLen) != 0 ||
       std::memcmp(site + kSiteLen, next, nextLen) != 0) {
      install_log("[LightProjectedTextureFix] unexpected bytes at the frame-name site, skipping");
      return;
   }

   std::memcpy(g_siteOrig, site, kSiteLen);
   std::memcpy(site, fix, kSiteLen);
   g_site = site;
}

void light_projected_texture_fix_uninstall()
{
   // Sections are re-protected by the time this runs, so the restore cannot be
   // a plain write.
   if (!g_site) return;
   protected_write(g_site, g_siteOrig, kSiteLen);
   g_site = nullptr;
}
