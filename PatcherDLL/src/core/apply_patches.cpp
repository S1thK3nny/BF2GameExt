#include "pch.h"

#include "apply_patches.hpp"
#include "resolve.hpp"
#include "game_build.hpp"
#include "util/cfile.hpp"
#include "util/ini_config.hpp"
#include "util/ini_registry.hpp"
#include "patch_table.hpp"

#include <string.h>

// Map an identified patch-list name to the runtime build enum used by the hooks.
static GameBuild build_from_name(const char* name)
{
   if (strstr(name, "modtools")) return GameBuild::Modtools;
   if (strstr(name, "Steam")) return GameBuild::Steam;
   if (strstr(name, "GoG") || strstr(name, "GOG")) return GameBuild::GOG;
   return GameBuild::Unknown;
}

static bool memeq(const void* left, size_t left_size, const void* right, size_t right_size)
{
   if (left_size != right_size) return false;

   return memcmp(left, right, left_size) == 0;
}

static auto resolve_file_address(uintptr_t offset, const slim_vector<section_info>& sections) -> char*
{
   for (const section_info& section : sections) {
      if (offset >= section.file_start and offset < section.file_end) {
         return section.memory_start + (offset - section.file_start);
      }
   }

   return nullptr;
}

// Verify a patch's site holds its expected original value (does not write).
static bool verify_patch(const patch& patch, const uintptr_t exe_base,
                         const slim_vector<section_info>& sections)
{
   char* patch_address = patch.flags.file_offset
                            ? resolve_file_address(patch.address, sections)
                            : (char*)resolve(exe_base, patch.address);

   if (not patch_address) return false;

   const uint32_t expected_value = patch.flags.expected_is_va
                                      ? (uint32_t)(uintptr_t)resolve(exe_base, patch.expected_value)
                                      : patch.expected_value;

   const size_t cmp_size = patch.flags.values_are_8bit ? 1 : sizeof(expected_value);

   return memeq(patch_address, cmp_size, &expected_value, cmp_size);
}

// The value a patch actually writes. A patch with a replacement_base is
// late-bound: its buffer is allocated by the set's prepare(), so the base is
// only known now, and replacement_value is an offset into it.
static uint32_t patch_replacement(const patch& patch)
{
   if (patch.replacement_base) return *patch.replacement_base + patch.replacement_value;

   return patch.replacement_value;
}

// Write a patch's replacement value (caller must have verify_patch'd it first).
static void write_patch(const patch& patch, const uintptr_t exe_base,
                        const slim_vector<section_info>& sections)
{
   char* patch_address = patch.flags.file_offset
                            ? resolve_file_address(patch.address, sections)
                            : (char*)resolve(exe_base, patch.address);

   const uint32_t replacement_value = patch_replacement(patch);
   const size_t cmp_size = patch.flags.values_are_8bit ? 1 : sizeof(replacement_value);

   memcpy(patch_address, &replacement_value, cmp_size);
}

// patch_set → INI section+key mapping now lives in ini_registry.hpp

bool apply_patches(const uintptr_t exe_base, const slim_vector<section_info>& sections,
                   const char* ini_path)
{
   cfile log{"BF2GameExt.log", "w"};

   if (not log) return false;

   ini_config cfg{ini_path};

   for (const exe_patch_list& exe_list : patch_lists) {
      log.printf("Checking executable against patch list: %s\n", exe_list.name);

      if (exe_list.id_address_is_file_offset) {
         const char* id_address = resolve_file_address(exe_list.id_address, sections);

         if (not id_address or not memeq(id_address, sizeof(exe_list.expected_id),
                                         &exe_list.expected_id, sizeof(exe_list.expected_id))) {
            continue;
         }
      }
      else {
         if (not memeq(resolve(exe_base, exe_list.id_address),
                       sizeof(exe_list.expected_id), &exe_list.expected_id, sizeof(exe_list.expected_id))) {
            continue;
         }
      }

      log.printf("Identified executable as: %s\nApplying patches.\n", exe_list.name);

      // Select the runtime address table for the detour-based hooks installed later.
      const GameBuild build = build_from_name(exe_list.name);
      game_build_select(build);

      for (const patch_set& set : exe_list.patches) {
         // exe_patch_list::patches is a fixed PATCH_COUNT array; lists with fewer
         // sets leave default-constructed tail slots — skip them.
         if (!set.name[0]) continue;

         // Check INI toggle for this patch set (defaults to enabled)
         auto [ini_section, ini_key] = ini_lookup_patch_set(set.name);
         if (ini_section && ini_key && !cfg.get_bool(ini_section, ini_key, true)) {
            log.printf("Skipping patch set (disabled in INI): %s\n", set.name);
            continue;
         }

         // Allocate any late-bound relocation buffers this set needs. Deferred
         // until after the INI check on purpose: a buffer named directly by the
         // table has to be a DLL global, and a DLL global costs its address space
         // at load whether the set is enabled or not.
         if (set.prepare && !set.prepare()) {
            log.printf("Skipping patch set (prepare failed, likely out of address space): %s\n",
                       set.name);
            continue;
         }

         // Verify every site in the set first. If any expected value mismatches
         // (wrong build/version, or an address we haven't mapped), skip the whole
         // set rather than leaving it half-applied — other sets still apply.
         const patch* bad = nullptr;
         for (const patch& patch : set.patches) {
            if (not verify_patch(patch, exe_base, sections)) { bad = &patch; break; }
         }

         if (bad) {
            log.printf("Skipping patch set (site mismatch @ %x, expected %x): %s\n",
                       bad->address, bad->expected_value, set.name);
            continue;
         }

         log.printf("Applying patch set: %s\n", set.name);
         for (const patch& patch : set.patches) {
            write_patch(patch, exe_base, sections);
         }
      }

      // Initialize the sentinel value at the end of the relocated EntityEx::mIdMap.
      // Iterator functions read 1 past the values array and compare against an RTTI
      // hash global that sits right after the old table in BSS. The class name differs
      // per build due to different BSS layouts.
      if (cfg.get_bool("LimitIncreases", "ObjectLimitIncrease", true)) {
         // Keyed off the build enum, not the list name: the GOG list is spelled
         // "…exe GoG" while this compared against "…exe GOG", so GOG silently
         // fell through to the modtools sentinel and the relocated mIdMap
         // iterator had the wrong end-of-iteration value.
         const char* sentinel_class = "Entity"; // modtools default
         if (build == GameBuild::Steam)
            sentinel_class = "EntityBuilding";
         else if (build == GameBuild::GOG)
            sentinel_class = "EntityBuildingClass";
         init_object_limit_sentinel(sentinel_class);
         log.printf("Object limit sentinel initialized (PblHash(\"%s\"))\n", sentinel_class);
      }

      return true;
   }

   log.printf("Couldn't identify executable. Unable to patch.\n");

   return false;
}
