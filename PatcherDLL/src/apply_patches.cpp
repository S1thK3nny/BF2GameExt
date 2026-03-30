#include "pch.h"

#include "apply_patches.hpp"
#include "cfile.hpp"
#include "ini_config.hpp"
#include "patch_table.hpp"

#include <string.h>

static const uintptr_t unrelocated_executable_base = 0x400000;

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

static auto resolve_address(uintptr_t virtual_address, const uintptr_t relocated_executable_base) -> char*
{
   return (char*)((virtual_address - unrelocated_executable_base) + relocated_executable_base);
}

static bool apply_patch(const patch& patch, const uintptr_t relocated_executable_base,
                        const slim_vector<section_info>& sections)
{
   char* patch_address = patch.flags.file_offset
                            ? resolve_file_address(patch.address, sections)
                            : resolve_address(patch.address, relocated_executable_base);

   const uint32_t expected_value = patch.flags.expected_is_va
                                      ? patch.expected_value - unrelocated_executable_base + relocated_executable_base
                                      : patch.expected_value;

   const size_t cmp_size = patch.flags.values_are_8bit ? 1 : sizeof(expected_value);

   if (not memeq(patch_address, cmp_size, &expected_value, cmp_size)) {
      return false;
   }

   memcpy(patch_address, &patch.replacement_value, cmp_size);

   return true;
}

// Map patch_set names to INI section + key.
struct patch_ini_info {
   const char* section;
   const char* key;
};

static patch_ini_info patch_set_ini_info(const char* set_name)
{
   if (strcmp(set_name, "RedMemory Heap Extensions") == 0) return {"LimitIncreases", "HeapExtension"};
   if (strcmp(set_name, "SoundParameterized Layer Limit Extension") == 0) return {"LimitIncreases", "SoundLayerLimit"};
   if (strcmp(set_name, "DLC Mission Limit Extension") == 0) return {"LimitIncreases", "DLCMissionLimit"};
   if (strcmp(set_name, "Sound Limit Extension") == 0) return {"LimitIncreases", "SoundLimit"};
   if (strcmp(set_name, "Particle Cache Increase") == 0) return {"LimitIncreases", "ParticleCacheIncrease"};
   if (strcmp(set_name, "Object Limit Increase") == 0) return {"LimitIncreases", "ObjectLimitIncrease"};
   if (strcmp(set_name, "Combo Anims Increase") == 0) return {"LimitIncreases", "ComboAnimIncrease"};
   if (strcmp(set_name, "High-Res Animation Limit") == 0) return {"LimitIncreases", "HighResAnimLimit"};
   if (strcmp(set_name, "Network Timer Increase") == 0) return {"LimitIncreases", "NetworkTimerIncrease"};
   if (strcmp(set_name, "Chunk Push Fix") == 0) return {"Fixes", "ChunkPushFix"};
   if (strcmp(set_name, "Matrix/Item Pool Limit Extension") == 0) return {"LimitIncreases", "MatrixPoolIncrease"};
   return {nullptr, nullptr};
}

bool apply_patches(const uintptr_t relocated_executable_base, const slim_vector<section_info>& sections,
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
         if (not memeq(resolve_address(exe_list.id_address, relocated_executable_base),
                       sizeof(exe_list.expected_id), &exe_list.expected_id, sizeof(exe_list.expected_id))) {
            continue;
         }
      }

      log.printf("Identified executable as: %s\nApplying patches.\n", exe_list.name);

      for (const patch_set& set : exe_list.patches) {
         // Check INI toggle for this patch set (defaults to enabled)
         auto [ini_section, ini_key] = patch_set_ini_info(set.name);
         if (ini_section && ini_key && !cfg.get_bool(ini_section, ini_key, true)) {
            log.printf("Skipping patch set (disabled in INI): %s\n", set.name);
            continue;
         }

         log.printf("Applying patch set: %s\n", set.name);

         for (const patch& patch : set.patches) {
            if (not apply_patch(patch, relocated_executable_base, sections)) {
               log.printf(R"(Failed to apply patch
   address = %x
   expected_value = %x
   replacement_value = %x
   flags = {.file_offset = %i, .expected_is_va = %i}
)",
                          patch.address, patch.expected_value, patch.replacement_value,
                          (int)patch.flags.file_offset, (int)patch.flags.expected_is_va);

               return false;
            }
         }
      }

      // Initialize the sentinel value at the end of the relocated EntityEx::mIdMap.
      // Iterator functions read 1 past the values array and compare against an RTTI
      // hash global that sits right after the old table in BSS. The class name differs
      // per build due to different BSS layouts.
      if (cfg.get_bool("LimitIncreases", "ObjectLimitIncrease", true)) {
         const char* sentinel_class = "Entity"; // modtools default
         if (strcmp(exe_list.name, "BattlefrontII.exe Steam") == 0)
            sentinel_class = "EntityBuilding";
         else if (strcmp(exe_list.name, "BattlefrontII.exe GOG") == 0)
            sentinel_class = "EntityBuildingClass";
         init_object_limit_sentinel(sentinel_class);
         log.printf("Object limit sentinel initialized (PblHash(\"%s\"))\n", sentinel_class);
      }

      return true;
   }

   log.printf("Couldn't identify executable. Unable to patch.\n");

   return false;
}
