#pragma once

#include <stdint.h>

// =============================================================================
// PblHash - the engine's name hash (PblHash::calcHash / _MakeHash).
//
// FNV-1a over each byte SIGN-extended and OR'd with 0x20 (case folding; '_'
// lands on 0x7F). A null or empty string hashes to 0, not to the FNV basis.
// Both details are the engine's own, read off all builds:
//     modtools 0x007E1B70, Steam 0x00726E50:
//         TEST EDX,EDX / JZ ret0 ; MOV CL,[EDX] / TEST CL,CL / JZ ret0
//         MOVSX ECX,CL / OR ECX,0x20 / XOR EAX,ECX / IMUL EAX,EAX,0x1000193
// Ground truth for new values: E:\BF2_Modtools\ToolsFL\bin\Hash.exe.
//
// Computed here rather than by calling the engine because most callers run
// from installers, while the exe's sections are not executable.
// =============================================================================

// Hash at most maxLen bytes, stopping early at a NUL.
constexpr uint32_t pbl_hash(const char* s, uint32_t maxLen)
{
   if (!s || !*s || maxLen == 0) return 0;
   uint32_t h = 0x811C9DC5u;
   for (uint32_t i = 0; i < maxLen && s[i]; ++i)
      h = (h ^ ((uint32_t)(int32_t)(signed char)s[i] | 0x20u)) * 0x01000193u;
   return h;
}

constexpr uint32_t pbl_hash(const char* s)
{
   return pbl_hash(s, UINT32_MAX);
}

// Values from Hash.exe.
static_assert(pbl_hash("EntityBuilding") == 0x460DE15Eu);
static_assert(pbl_hash("HeldOrdnanceEffectBone") == 0x1892110Du);
static_assert(pbl_hash("hp_fire") == 0xFDD110D2u);
static_assert(pbl_hash("a\xE9" "b") == 0xF4E78CC7u); // non-ASCII: sign extension
static_assert(pbl_hash("") == 0 && pbl_hash(nullptr) == 0);
