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

// Continue a hash over more bytes. There is no finalisation step, so for any
// non-empty a, pbl_hash_append(pbl_hash(a), b) == pbl_hash(a + b): a name the
// engine stored only as a hash can still be extended with a suffix.
constexpr uint32_t pbl_hash_append(uint32_t h, const char* s)
{
   for (; s && *s; ++s)
      h = (h ^ ((uint32_t)(int32_t)(signed char)*s | 0x20u)) * 0x01000193u;
   return h;
}

// Values from Hash.exe.
static_assert(pbl_hash("EntityBuilding") == 0x460DE15Eu);
static_assert(pbl_hash("HeldOrdnanceEffectBone") == 0x1892110Du);
static_assert(pbl_hash("hp_fire") == 0xFDD110D2u);
static_assert(pbl_hash("a\xE9" "b") == 0xF4E78CC7u); // non-ASCII: sign extension
static_assert(pbl_hash("") == 0 && pbl_hash(nullptr) == 0);
static_assert(pbl_hash_append(pbl_hash("hp_"), "fire") == pbl_hash("hp_fire"));

// =============================================================================
// PblTEMPHash - the hardpoint and bone name hash (model hardpoints, pose joints,
// FirePointName, tentacle bones).
//
// CRC-32/BZIP2 (poly 0x04C11DB7, MSB first, init and xorout 0xFFFFFFFF) over the
// name with A-Z LOWERCASED first; every other byte goes in unchanged. Read off
// all builds (table-driven there, bit-by-bit here):
//     modtools 0x007E1C10: CMP CL,0x5A / JA / CMP CL,0x41 / JB / ADD CL,0x20
//     Steam    0x00726D80, GOG 0x00727E50:
//         LEA ECX,[EBX-0x41] / CMP CL,0x19 / JA / ADD BL,0x20
// A null or empty string hashes to 0, as on the engine (NOT of the -1 seed).
// =============================================================================

constexpr uint32_t pbl_temp_hash(const char* s)
{
   uint32_t h = 0xFFFFFFFFu;
   for (; s && *s; ++s) {
      uint8_t c = (uint8_t)*s;
      if (c >= 'A' && c <= 'Z') c += 0x20;
      h ^= (uint32_t)c << 24;
      for (int bit = 0; bit < 8; ++bit)
         h = (h << 1) ^ ((h & 0x80000000u) ? 0x04C11DB7u : 0u);
   }
   return ~h;
}

static_assert(pbl_temp_hash("123456789") == 0xFC891918u);  // CRC-32/BZIP2 check value
static_assert(pbl_temp_hash("hp_weapons") == 0x2B960099u); // Weapon::Render immediate, Steam 0x006793AA
static_assert(pbl_temp_hash("HP_Weapons") == pbl_temp_hash("hp_weapons"));
static_assert(pbl_temp_hash("") == 0 && pbl_temp_hash(nullptr) == 0);
