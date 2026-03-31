#pragma once

#include <stdint.h>

#include "util/slim_vector.hpp"

#define PATCH_COUNT 13
#define EXE_COUNT 3

struct patch_flags {
   /// @brief Address represents a file offset instead of a virtual an unrelocated virtual address.
   bool file_offset : 1 = false;

   /// expected_value is an unrelocated virtual address (what would be displayed in tools like Ghidra/IDA)
   bool expected_is_va : 1 = false;

   /// Compare and write only the low byte of expected_value/replacement_value (for imm8 patches)
   bool values_are_8bit : 1 = false;
};

struct patch {
   uintptr_t address = 0;
   uint32_t expected_value = 0;
   uint32_t replacement_value = 0;
   patch_flags flags = {};
};

struct patch_set {
   const char* name = "";
   slim_vector<patch> patches;
};

struct exe_patch_list {
   const char* name = "";

   bool id_address_is_file_offset = false;

   uintptr_t id_address = 0;
   uint64_t expected_id = 0;

   const patch_set patches[PATCH_COUNT];
};

extern const exe_patch_list patch_lists[EXE_COUNT];

// Renderer cache storage — redirected from s_caches[15] by binary patches.
// Used by particle_renderer_patch.cpp to set the overflow hook's array pointer.
extern char g_sCaches_storage[];

// Initialize the sentinel value at the end of the relocated EntityEx::mIdMap hash table.
// The RTTI class name differs per build (different BSS layouts after mIdMap).
void init_object_limit_sentinel(const char* rtti_class_name);

// Relocated EntityEx::mIdMap buffer (N=2048).
// Layout: [uint32 count] [uint32 keys[2048]] [uint32 values[2048]]
// values[i] are EntityEx* pointers (0 = empty slot).
extern char EntityEx_mIdMap_new[];
static constexpr uint32_t ENTITY_MAP_BUCKETS_NEW = 2048;