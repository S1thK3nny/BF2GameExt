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

// Does the main module hold this list's identifying bytes? The id is a fixed
// 8-byte sequence at a per-build offset, so a build is recognized by where the
// bytes sit, not by the executable's name -- renamed or relocated copies still
// identify, repacked ones do not.
static bool matches_list(const exe_patch_list& exe_list, const uintptr_t exe_base,
                         const slim_vector<section_info>& sections)
{
   if (exe_list.id_address_is_file_offset) {
      const char* id_address = resolve_file_address(exe_list.id_address, sections);

      return id_address and memeq(id_address, sizeof(exe_list.expected_id),
                                  &exe_list.expected_id, sizeof(exe_list.expected_id));
   }

   return memeq(resolve(exe_base, exe_list.id_address), sizeof(exe_list.expected_id),
                &exe_list.expected_id, sizeof(exe_list.expected_id));
}

// One data directory of a mapped 32-bit module, or null if it is absent.
static const IMAGE_DATA_DIRECTORY* module_data_directory(const uintptr_t module_base, int index)
{
   const char* const base = (const char*)module_base;

   const IMAGE_DOS_HEADER& dos_header = *(const IMAGE_DOS_HEADER*)base;
   if (dos_header.e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

   const IMAGE_NT_HEADERS32& nt_headers = *(const IMAGE_NT_HEADERS32*)(base + dos_header.e_lfanew);
   if (nt_headers.Signature != IMAGE_NT_SIGNATURE) return nullptr;
   if (nt_headers.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return nullptr;
   if (nt_headers.OptionalHeader.NumberOfRvaAndSizes <= (DWORD)index) return nullptr;

   const IMAGE_DATA_DIRECTORY& dir = nt_headers.OptionalHeader.DataDirectory[index];
   if (not dir.VirtualAddress or not dir.Size) return nullptr;

   return &dir;
}

// Walk a module's import directory looking for one DLL by name.
static bool module_imports(const uintptr_t module_base, const char* dll_name)
{
   const char* const base = (const char*)module_base;

   const IMAGE_DATA_DIRECTORY* import_dir =
      module_data_directory(module_base, IMAGE_DIRECTORY_ENTRY_IMPORT);
   if (not import_dir) return false;

   for (const IMAGE_IMPORT_DESCRIPTOR* import =
           (const IMAGE_IMPORT_DESCRIPTOR*)(base + import_dir->VirtualAddress);
        import->Name; ++import) {
      if (_stricmp(base + import->Name, dll_name) == 0) return true;
   }

   return false;
}

// Is this the original 2006 v1.1 retail executable? It still circulates: some
// GOG installs ship it, and so does the SWBFSpy multiplayer exe. It is not the
// 2017 recompile our Steam and GOG tables describe but a separate compile, so
// nothing in the project applies to it and the user needs telling that plainly
// rather than getting the generic "couldn't identify" failure.
//
// Every copy seen (the 2006 Steam depot, a GOG install, SWBFSpy) is its own
// link of one compile, so the fixed-offset id bytes the patch lists use differ
// between them. What they share is the CodeView record naming the build config,
// `...\Build\PC Final LTCG\Battlefront2.pdb`. No supported build was linked from
// that config: modtools is `PC Release` / `PC Modtools Release`, the 2017
// exes `PC GOG Release` / `PC GOG XPLAY Release`. The record sits in .rdata, so
// it is mapped and readable in place.
static bool is_retail_2006(const uintptr_t module_base)
{
   const char* const base = (const char*)module_base;

   const IMAGE_DATA_DIRECTORY* debug_dir =
      module_data_directory(module_base, IMAGE_DIRECTORY_ENTRY_DEBUG);
   if (not debug_dir) return false;

   const IMAGE_DEBUG_DIRECTORY* entries =
      (const IMAGE_DEBUG_DIRECTORY*)(base + debug_dir->VirtualAddress);
   const size_t count = debug_dir->Size / sizeof(IMAGE_DEBUG_DIRECTORY);

   // RSDS record: 'RSDS', GUID, age, then the NUL-terminated PDB path.
   constexpr size_t rsds_path_offset = 4 + 16 + 4;

   for (size_t i = 0; i < count; ++i) {
      const IMAGE_DEBUG_DIRECTORY& entry = entries[i];

      if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
      if (not entry.AddressOfRawData or entry.SizeOfData <= rsds_path_offset) continue;

      const char* const record = base + entry.AddressOfRawData;
      if (memcmp(record, "RSDS", 4) != 0) continue;

      const char* const pdb_path = record + rsds_path_offset;
      const size_t path_room = entry.SizeOfData - rsds_path_offset;

      // Unterminated within the record means it isn't one we can trust.
      if (strnlen(pdb_path, path_room) == path_room) continue;

      if (strstr(pdb_path, "\\PC Final LTCG\\")) return true;
   }

   return false;
}

exe_identity identify_exe(const uintptr_t exe_base, const slim_vector<section_info>& sections)
{
   for (const exe_patch_list& exe_list : patch_lists) {
      if (matches_list(exe_list, exe_base, sections)) return exe_identity::supported;
   }

   if (is_retail_2006(exe_base)) return exe_identity::retail_2006;

   // No fingerprint matched, so decide how loudly to fail. An unrecognized BF2
   // build is a real problem the user needs told about (a pre-patched exe, say);
   // a launcher or tool that merely loaded our proxy is not our business at all.
   // Every BF2 build links the Bink video decoder and no launcher does, which
   // makes binkw32.dll the discriminator -- and unlike the executable's name, it
   // survives the renamed copies people actually run.
   return module_imports(exe_base, "binkw32.dll") ? exe_identity::unsupported
                                                  : exe_identity::foreign;
}

// Width of a patch's compare/write, in bytes.
static size_t patch_value_size(const patch& patch)
{
   // Checked first on purpose: a pointer is four bytes whatever else is set, and
   // truncating one would corrupt the site rather than fail to match.
   if (patch.flags.values_are_va) return sizeof(uint32_t);
   if (patch.flags.values_are_8bit) return 1;
   if (patch.flags.values_are_16bit) return 2;
   return sizeof(uint32_t);
}

// Verify a patch's site holds its expected original value (does not write).
static bool verify_patch(const patch& patch, const uintptr_t exe_base,
                         const slim_vector<section_info>& sections)
{
   char* patch_address = patch.flags.file_offset
                            ? resolve_file_address(patch.address, sections)
                            : (char*)resolve(exe_base, patch.address);

   if (not patch_address) return false;

   const bool expected_is_pointer = patch.flags.expected_is_va || patch.flags.values_are_va;

   const uint32_t expected_value = expected_is_pointer
                                      ? (uint32_t)(uintptr_t)resolve(exe_base, patch.expected_value)
                                      : patch.expected_value;

   const size_t cmp_size = patch_value_size(patch);

   return memeq(patch_address, cmp_size, &expected_value, cmp_size);
}

// The value a patch actually writes. A patch with a replacement_base is
// late-bound: its buffer is allocated by the set's prepare(), so the base is
// only known now, and replacement_value is an offset into it.
static uint32_t patch_replacement(const patch& patch, const uintptr_t exe_base)
{
   if (patch.replacement_base) return *patch.replacement_base + patch.replacement_value;

   // An unrelocated pointer has to be rebased the same way the loader rebased
   // everything else in the image, or the engine calls an address that is only
   // correct when the exe loads where it asked to.
   if (patch.flags.values_are_va)
      return (uint32_t)(uintptr_t)resolve(exe_base, patch.replacement_value);

   return patch.replacement_value;
}

// Write a patch's replacement value (caller must have verify_patch'd it first).
static void write_patch(const patch& patch, const uintptr_t exe_base,
                        const slim_vector<section_info>& sections)
{
   char* patch_address = patch.flags.file_offset
                            ? resolve_file_address(patch.address, sections)
                            : (char*)resolve(exe_base, patch.address);

   const uint32_t replacement_value = patch_replacement(patch, exe_base);
   const size_t cmp_size = patch_value_size(patch);

   memcpy(patch_address, &replacement_value, cmp_size);
}

// patch_set → INI section+key mapping now lives in ini_registry.hpp

bool apply_patches(const uintptr_t exe_base, const slim_vector<section_info>& sections,
                   const char* ini_path)
{
   // Start a fresh log, then append so runtime installers can append through
   // their own handles without the next patch-set message overwriting them.
   {
      cfile fresh_log{"BF2GameExt.log", "w"};
      if (not fresh_log) return false;
   }
   cfile log{"BF2GameExt.log", "a"};

   if (not log) return false;

   ini_config cfg{ini_path};

   // Name the host process. When identification fails this is the single most
   // useful line in the log -- it says which executable we were actually loaded
   // into, which is not always the one the user thinks they launched.
   char module_path[MAX_PATH];
   if (GetModuleFileNameA((HMODULE)exe_base, module_path, sizeof(module_path)))
      log.printf("Host executable: %s\n", module_path);

   for (const exe_patch_list& exe_list : patch_lists) {
      log.printf("Checking executable against patch list: %s\n", exe_list.name);

      if (not matches_list(exe_list, exe_base, sections)) continue;

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

         // Coupled runtime storage must be installed before raising any limit.
         // Every numeric site is already verified, so a failed callback leaves
         // the whole set at its stock values. The callback owns rollback of any
         // hooks or redirects it attempted before returning false.
         if (set.install && !set.install(exe_base)) {
            log.printf("Skipping patch set (runtime support not installed): %s\n", set.name);
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

   if (is_retail_2006(exe_base)) {
      log.printf("This is the original 2006 v1.1 executable (PC Final LTCG), a different compile "
                 "from the 2017 Steam/GOG builds. It is not supported.\n");
   }

   log.printf("Couldn't identify executable. Unable to patch.\n");

   return false;
}
