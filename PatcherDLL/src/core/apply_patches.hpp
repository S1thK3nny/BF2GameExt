#pragma once

#include "util/slim_vector.hpp"

#include <stdint.h>

struct section_info {
   char* memory_start = nullptr;

   uint32_t file_start = 0;
   uint32_t file_end = 0;
};

// What process are we actually in?
//
// Our dinput8.dll proxy lives in GameData, and Windows resolves dinput8.dll from
// the application directory of whatever process asks for it. So any program
// started out of that folder that touches DirectInput -- directly or through an
// injected overlay -- loads us as well. The Battlefront II Mod Loader is the one
// that comes up in practice. Those processes have to be left completely alone,
// which means answering this question before doing anything with a side effect.
enum class exe_identity {
   supported,   // matched one of the patch lists
   unsupported, // looks like a BF2 build we have no table for (pre-patched exe, odd variant)
   foreign,     // not the game at all
};

// Byte-fingerprints the main module against the patch lists. Reads only; writes
// nothing, opens nothing. Call before unprotecting sections or opening the log.
exe_identity identify_exe(const uintptr_t relocated_executable_base,
                          const slim_vector<section_info>& sections);

bool apply_patches(const uintptr_t relocated_executable_base, const slim_vector<section_info>& sections,
                   const char* ini_path = nullptr);
