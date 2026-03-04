#include "pch.h"
#include "bone_transform.hpp"
#include "lua_hooks.hpp"

#include <detours.h>

// =============================================================================
// TransformBone ODF property
// =============================================================================
// Hook EntitySoldierClass::SetProperty to intercept a custom "TransformBone"
// property. When encountered, enables procedural transform animation on the
// named bone by calling the game's SetJointTransformAnimationFlag.
//
// Requirement: AnimationName must appear before TransformBone in the ODF so
// that mSpecialSkeleton is populated when we read it.

// ---------------------------------------------------------------------------
// CRC-32/BZIP2 — bone name hashing (same as game engine uses for joint lookup)
// ---------------------------------------------------------------------------

static uint32_t g_crc32_table[256];
static bool     g_crc32_ready = false;

static void init_crc32_table()
{
   if (g_crc32_ready) return;
   for (int i = 0; i < 256; i++) {
      uint32_t crc = (uint32_t)i << 24;
      for (int j = 0; j < 8; j++)
         crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
      g_crc32_table[i] = crc;
   }
   g_crc32_ready = true;
}

static uint32_t bone_hash(const char* str)
{
   uint32_t h = 0xFFFFFFFF;
   for (; *str; str++)
      h = g_crc32_table[((h >> 24) ^ (uint8_t)*str) & 0xFF] ^ (h << 8);
   return h ^ 0xFFFFFFFF;
}

// ---------------------------------------------------------------------------
// FNV-1a hash — matches game's PblHash for property name lookup
// ---------------------------------------------------------------------------

static uint32_t pbl_hash(const char* str)
{
   uint32_t h = 0x811c9dc5u;
   for (; *str; ++str)
      h = (h ^ ((uint8_t)*str | 0x20)) * 0x01000193u;
   return h;
}

// Property hash for "TransformBone"
static const uint32_t HASH_TRANSFORM_BONE = pbl_hash("TransformBone");

// ---------------------------------------------------------------------------
// Game function typedefs
// ---------------------------------------------------------------------------

// EntitySoldierClass::SetProperty — __thiscall (ECX=this, propHash+value on stack, RET 8)
using fn_SetProperty = void(__thiscall*)(void*, uint32_t, const char*);
static fn_SetProperty original_SetProperty = nullptr;

// PblHashTableCode::_Find — __cdecl (table, size=0x80, hash) → returns jointIndex+1 (0 = not found)
using fn_HashFind = int(__cdecl*)(void*, int, uint32_t);
static fn_HashFind game_HashFind = nullptr;

// SetJointTransformAnimationFlag — __thiscall (ECX=skeleton, jointIndex, enableFlag)
using fn_SetJointFlag = void(__thiscall*)(void*, int, bool);
static fn_SetJointFlag game_SetJointFlag = nullptr;

// Build-specific offset for mSpecialSkeleton within EntitySoldierClass
static unsigned s_skeletonOffset = 0;

// ---------------------------------------------------------------------------
// SetProperty hook
// ---------------------------------------------------------------------------

static void __fastcall hooked_SetProperty(void* thisPtr, void* /*edx*/, uint32_t propHash, const char* value)
{
   // Let the original handle the property first
   original_SetProperty(thisPtr, propHash, value);

   if (propHash != HASH_TRANSFORM_BONE)
      return;

   // Read mSpecialSkeleton from EntitySoldierClass
   void* skeleton = *(void**)((char*)thisPtr + s_skeletonOffset);
   if (!skeleton) {
      dbg_log("[BoneTransform] WARNING: TransformBone=\"%s\" but mSpecialSkeleton is null "
              "(AnimationName must appear before TransformBone in ODF)\n", value);
      return;
   }

   // Hash the bone name and look up in skeleton's joint hash table (inline at skeleton+0x0C)
   uint32_t boneHash = bone_hash(value);
   void* hashTable = (char*)skeleton + 0x0C;  // m_kHashCRC2Joint is inline, not a pointer
   int result = game_HashFind(hashTable, 0x80, boneHash);
   int jointIndex = result - 1;

   if (jointIndex < 0) {
      dbg_log("[BoneTransform] WARNING: bone \"%s\" (hash 0x%08X) not found in skeleton\n",
              value, boneHash);
      return;
   }

   game_SetJointFlag(skeleton, jointIndex, true);
   dbg_log_verbose("[BoneTransform] Enabled transform on bone \"%s\" (joint %d)\n", value, jointIndex);
}

// ---------------------------------------------------------------------------
// Per-build addresses
// ---------------------------------------------------------------------------

struct BoneTransformAddrs {
   uintptr_t set_property;               // EntitySoldierClass::SetProperty
   uintptr_t hash_find;                  // PblHashTableCode::_Find
   uintptr_t set_joint_flag;             // SetJointTransformAnimationFlag
   unsigned  skeleton_offset;            // mSpecialSkeleton offset in EntitySoldierClass
};

static constexpr BoneTransformAddrs MODTOOLS_ADDRS = {
   .set_property    = 0x53fa20,
   .hash_find       = 0x7e1a40,
   .set_joint_flag  = 0x847d40,
   .skeleton_offset = 0x1200,
};

static constexpr BoneTransformAddrs STEAM_ADDRS = {
   .set_property    = 0x4f82e0,
   .hash_find       = 0x726e00,
   .set_joint_flag  = 0x72ee20,
   .skeleton_offset = 0x100C,
};

static constexpr BoneTransformAddrs GOG_ADDRS = {
   .set_property    = 0x4f82e0,  // EntitySoldierClass::SetProperty — same VA as Steam
   .hash_find       = 0x727ed0,  // PblHashTableCode::_Find — __cdecl
   .set_joint_flag  = 0x72fef0,  // SetJointTransformAnimationFlag — __thiscall, RET 8
   .skeleton_offset = 0x100C,    // mSpecialSkeleton offset — confirmed via vfunction8 disasm
};

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void bone_transform_install(uintptr_t exe_base)
{
   const BoneTransformAddrs* addrs = nullptr;
   switch (g_exeType) {
      case ExeType::MODTOOLS: addrs = &MODTOOLS_ADDRS; break;
      case ExeType::STEAM:    addrs = &STEAM_ADDRS;    break;
      case ExeType::GOG:      addrs = &GOG_ADDRS;      break;
      default:
         dbg_log("[BoneTransform] Skipping — unsupported build\n");
         return;
   }

   init_crc32_table();

   auto resolve = [=](uintptr_t addr) -> uintptr_t {
      return addr - 0x400000u + exe_base;
   };

   s_skeletonOffset = addrs->skeleton_offset;
   game_HashFind    = (fn_HashFind)resolve(addrs->hash_find);
   game_SetJointFlag = (fn_SetJointFlag)resolve(addrs->set_joint_flag);

   original_SetProperty = (fn_SetProperty)resolve(addrs->set_property);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   LONG result = DetourTransactionCommit();

   if (result == NO_ERROR) {
      dbg_log("[BoneTransform] Hook installed successfully\n");
   } else {
      dbg_log("[BoneTransform] ERROR: Detours commit failed (%ld)\n", result);
   }
}

void bone_transform_uninstall()
{
   if (!original_SetProperty) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   DetourTransactionCommit();

   dbg_log("[BoneTransform] Hook removed\n");
}
