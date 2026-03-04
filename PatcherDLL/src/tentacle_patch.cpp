#include "pch.h"
#include "tentacle_patch.hpp"
#include "lua_hooks.hpp"

#include <detours.h>
#include <string.h>

// ============================================================================
// Tentacle bone limit increase: 4 → 9 tentacles
// ============================================================================
// Strategy: Hook Constructor + DoTentacles. The constructor initializes the
// extended struct. DoTentacles reimplements only the orchestration (timing,
// bone lookup, init) and batch-calls the original sub-functions
// (UpdatePositions, EnforceCollisions, UpdatePose) for groups of up to 4
// tentacles at a time, copying data to/from the legacy array positions.
// This avoids reimplementing any simulation math.

// ============================================================================
// Constants
// ============================================================================

static constexpr int MAX_TENTACLES      = 9;
static constexpr int MAX_BONES_PER_TENT = 5;
static constexpr int TENT_STRIDE        = 6;  // tPos/oldPos positions per tentacle (5 bones + tip)
static constexpr int BONE_PTR_STRIDE    = 5;  // bonePtrs entries per tentacle
static constexpr int BATCH_SIZE         = 4;  // max tentacles per sub-function call (legacy array fits 4)
static constexpr float MAX_DT           = 0.039f;

// Pose hash table: 128 keys + 128 values = 256 uint32s
static constexpr int HT_SIZE = 0x100;

// ============================================================================
// CRC-32/BZIP2 — bone name hashing
// ============================================================================

static uint32_t g_crc32_table[256];

static void init_crc32_table()
{
   for (int i = 0; i < 256; i++) {
      uint32_t crc = (uint32_t)i << 24;
      for (int j = 0; j < 8; j++)
         crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
      g_crc32_table[i] = crc;
   }
}

static uint32_t bone_hash(const char* str)
{
   uint32_t h = 0xFFFFFFFF;
   for (; *str; str++)
      h = g_crc32_table[((h >> 24) ^ (uint8_t)*str) & 0xFF] ^ (h << 8);
   return h ^ 0xFFFFFFFF;
}

// ============================================================================
// Bone hash table — 54 entries (9 tentacles * 6 bones max)
// ============================================================================

static constexpr int TOTAL_BONE_HASHES = MAX_TENTACLES * TENT_STRIDE; // 54
static uint32_t g_bone_hashes[TOTAL_BONE_HASHES];
static uint32_t g_hash_bone_string_1;

static bool init_bone_hashes(uintptr_t game_table_va)
{
   init_crc32_table();

   char buf[32];
   for (int i = 0; i < TOTAL_BONE_HASHES; i++) {
      sprintf_s(buf, "bone_string_%d", i + 1);
      g_bone_hashes[i] = bone_hash(buf);
   }
   g_hash_bone_string_1 = g_bone_hashes[0];

   // Verify against game's table
   uint32_t game_first = *(uint32_t*)game_table_va;
   if (g_bone_hashes[0] != game_first) {
      dbg_log("[Tentacle] Hash mismatch: computed %08x, game %08x\n", g_bone_hashes[0], game_first);
      return false;
   }
   return true;
}

// ============================================================================
// Types
// ============================================================================

struct PblVec3 { float x, y, z; };

struct PblMat4 {
   PblVec3 right;  float rw;
   PblVec3 up;     float uw;
   PblVec3 fwd;    float fw;
   PblVec3 trans;   float tw;
};

// Extended TentacleSimulator struct (0x778 bytes)
struct TentSim {
   PblVec3 tPos_legacy[24];        // 0x000 — scratch for batching
   PblVec3 oldPos_legacy[24];      // 0x120 — scratch for batching
   PblVec3 oldVelocity;            // 0x240 (12 bytes → ends at 0x24C)
   float   mInternalTimer;         // 0x24C
   float   mTimeSinceLastUpdate;   // 0x250
   float   mTimerOffset;           // 0x254
   int     mNumTentacles;          // 0x258
   int     mBonesPerTentacle;      // 0x25C
   int     mCollType;              // 0x260
   int     mFirstUpdate;           // 0x264
   PblVec3 tPos[54];               // 0x268
   PblVec3 oldPos[54];             // 0x4F0
};
static_assert(offsetof(TentSim, oldVelocity) == 0x240);
static_assert(offsetof(TentSim, mInternalTimer) == 0x24C);
static_assert(offsetof(TentSim, mNumTentacles) == 0x258);
static_assert(offsetof(TentSim, mFirstUpdate) == 0x264);
static_assert(offsetof(TentSim, tPos) == 0x268);
static_assert(offsetof(TentSim, oldPos) == 0x4F0);
static_assert(sizeof(TentSim) == 0x778);

// ============================================================================
// Pose hash table — _Find (linear probe, matches game's PblHashTableCode)
// ============================================================================

static void* ht_find(uint32_t* table, int tableSize, uint32_t hash)
{
   int half = tableSize >> 1;          // 128
   int mask = half - 1;                // 127
   int idx = hash & mask;
   for (int i = 0; i < half; i++) {
      uint32_t key = table[idx];
      if (key == 0) return nullptr;    // empty slot
      if (key == hash)
         return (void*)(uintptr_t)table[half + idx]; // value
      idx = (idx - 1) & mask;          // probe backward
   }
   return nullptr;
}

// ============================================================================
// Original sub-function pointers (filled at install time)
// ============================================================================

// All __thiscall: ECX=this, params on stack, callee cleans
using fn_UpdatePositions   = void(__thiscall*)(void* self, float dt, void* velocity, void* pose, void* parentMat, void** bonePtrs);
using fn_EnforceCollisions = void(__thiscall*)(void* self, void* pose, void* parentMat);
using fn_UpdatePose        = void(__thiscall*)(void* self, void* pose, void* parentMat, void** bonePtrs, void* targetMats);

static fn_UpdatePositions   game_UpdatePositions   = nullptr;
static fn_EnforceCollisions game_EnforceCollisions = nullptr;
static fn_UpdatePose        game_UpdatePose        = nullptr;

// Steam/GOG LTCG hybrid: UpdatePositions takes dt in XMM1 instead of on the stack.
// RET 0x10 (4 stack params) instead of modtools' RET 0x14 (5 stack params).
// This naked shim adapts standard __thiscall(self, dt, vel, pose, pMat, bPtrs)
// to Steam convention: __thiscall(self, vel, pose, pMat, bPtrs) + XMM1=dt.
static void* g_raw_UpdatePositions = nullptr;

static __declspec(naked) void shim_UpdatePositions_steam()
{
   __asm {
      movss xmm1, [esp+4]       // dt from stack → XMM1
      push  ebp
      mov   ebp, esp
      push  dword ptr [ebp+24]  // bonePtrs
      push  dword ptr [ebp+20]  // parentMat
      push  dword ptr [ebp+16]  // pose (unused but passed through)
      push  dword ptr [ebp+12]  // velocity
      mov   eax, dword ptr [g_raw_UpdatePositions]
      call  eax                  // real func does RET 0x10, cleans our 4 pushes
      pop   ebp
      ret   0x14                 // clean caller's 5 params (20 bytes)
   }
}

// Game's static bone hash table — needed for UpdatePose hash swapping per batch.
// UpdatePose indexes (&bone_string_1)[bpt*tentIdx+boneIdx] to get hash keys for
// _Remove/_Store on the pose. In batch mode tentIdx resets to 0, so we must swap
// the game table entries to match the current batch's actual bone hashes.
static uint32_t* g_game_bone_table = nullptr;
static bool      g_bone_table_writable = false;

static void ensure_bone_table_writable()
{
   if (!g_bone_table_writable && g_game_bone_table) {
      DWORD old_prot;
      VirtualProtect(g_game_bone_table, 20 * sizeof(uint32_t), PAGE_READWRITE, &old_prot);
      g_bone_table_writable = true;
   }
}

// ============================================================================
// Batch helper — copies tPos/oldPos to/from legacy area and calls a sub-func
// ============================================================================

template<typename Fn>
static void batch_call_sub(
   TentSim* self, int totalTentacles,
   Fn callFn)
{
   int saved_numT = self->mNumTentacles;

   for (int batchStart = 0; batchStart < totalTentacles; batchStart += BATCH_SIZE) {
      int batchCount = totalTentacles - batchStart;
      if (batchCount > BATCH_SIZE) batchCount = BATCH_SIZE;

      int posCount = batchCount * TENT_STRIDE; // positions to copy

      // Copy extended → legacy
      memcpy(self->tPos_legacy,   &self->tPos[batchStart * TENT_STRIDE],   posCount * sizeof(PblVec3));
      memcpy(self->oldPos_legacy, &self->oldPos[batchStart * TENT_STRIDE], posCount * sizeof(PblVec3));

      self->mNumTentacles = batchCount;

      callFn(self, batchStart, batchCount);

      // Copy legacy → extended (results)
      memcpy(&self->tPos[batchStart * TENT_STRIDE],   self->tPos_legacy,   posCount * sizeof(PblVec3));
      memcpy(&self->oldPos[batchStart * TENT_STRIDE], self->oldPos_legacy, posCount * sizeof(PblVec3));
   }

   self->mNumTentacles = saved_numT;
}

// ============================================================================
// Hooked Constructor
// ============================================================================

using fn_Ctor = void(__thiscall*)(void*, int, int, int);
static fn_Ctor original_Ctor = nullptr;

static TentSim* __fastcall hooked_Ctor(TentSim* self, void* /*edx*/, int numT, int bonesPerT, int collType)
{
   memset(self, 0, sizeof(TentSim));
   self->mNumTentacles = numT;
   self->mBonesPerTentacle = bonesPerT;
   self->mCollType = collType;
   self->mFirstUpdate = 1;
   return self;  // Caller stores EAX as the TentacleSimulator pointer
}

// ============================================================================
// Hooked DoTentacles — orchestration only, no simulation math
// ============================================================================

using fn_DoTentacles = void(__thiscall*)(void*, void*, void*, void*, void*, float);
static fn_DoTentacles original_DoTentacles = nullptr;

static void __fastcall hooked_DoTentacles(
   TentSim* self, void* /*edx*/,
   void* pose_raw, void* parentMatrix, void* velocity, void* targetMatrices, float dt_unused)
{
   // Cast pose for hash table access
   // RedPose: *(int*)(pose+0) = numEntries, (uint32_t*)(pose+4) = table data
   uint32_t* poseTable = (uint32_t*)((uintptr_t)pose_raw + 4);

   int numT = self->mNumTentacles;
   int bpt  = self->mBonesPerTentacle;
   if (numT <= 0 || bpt <= 0) return;

   // --- 1. Timing (same as original offline path) ---
   float dt = self->mInternalTimer;
   if (dt > MAX_DT) dt = MAX_DT;
   self->mInternalTimer = 0.0f;
   self->mTimeSinceLastUpdate = dt;

   // --- 2. Guard: bone_string_1 must exist in pose ---
   if (!ht_find(poseTable, HT_SIZE, g_hash_bone_string_1))
      return;

   // --- 3. Bone lookup: fill bonePtrs for ALL tentacles ---
   void* bonePtrs[MAX_TENTACLES * BONE_PTR_STRIDE]; // 45 max
   memset(bonePtrs, 0, sizeof(bonePtrs));

   for (int t = 0; t < numT; t++) {
      for (int b = 0; b < bpt; b++) {
         uint32_t key = g_bone_hashes[bpt * t + b];
         bonePtrs[t * BONE_PTR_STRIDE + b] = ht_find(poseTable, HT_SIZE, key);
      }
   }

   // --- 4. mFirstUpdate: init all tPos/oldPos from bone matrices ---
   if (self->mFirstUpdate) {
      for (int t = 0; t < numT; t++) {
         for (int b = 0; b < bpt; b++) {
            PblMat4* bone = (PblMat4*)bonePtrs[t * BONE_PTR_STRIDE + b];
            if (bone) {
               int idx = t * TENT_STRIDE + b;
               self->tPos[idx] = bone->trans;
               self->oldPos[idx] = bone->trans;
            }
         }
      }
      self->mFirstUpdate = 0;
   }

   // --- 5. Simulation: batch-call original sub-functions ---
   if (dt > 0.0f) {
      // UpdatePositions
      batch_call_sub(self, numT, [&](TentSim* s, int batchStart, int batchCount) {
         void** batchBonePtrs = &bonePtrs[batchStart * BONE_PTR_STRIDE];
         game_UpdatePositions(s, dt, velocity, pose_raw, parentMatrix, batchBonePtrs);
      });

      // EnforceCollisions
      batch_call_sub(self, numT, [&](TentSim* s, int batchStart, int /*batchCount*/) {
         game_EnforceCollisions(s, pose_raw, parentMatrix);
      });
   }

   // UpdatePose (always runs, even if dt==0)
   // UpdatePose accesses the game's static bone hash table by index
   // [bpt*tentIdx+boneIdx] for _Remove/_Store on the pose hash table.
   // Since tentIdx resets to 0 per batch, we must swap the game table
   // entries to contain the correct hashes for each batch.
   //
   // targetMatrices is allocated on the CALLER's stack, sized for only 4 tentacles.
   // For batches beyond the first, we use a static overflow buffer to avoid
   // writing past the caller's array. (We can't widen the caller's SUB ESP
   // because those functions use ESP-relative addressing after AND ESP alignment.)
   static PblMat4 s_targetMats_overflow[MAX_TENTACLES * MAX_BONES_PER_TENT]; // 9*5 = 45 matrices
   ensure_bone_table_writable();
   batch_call_sub(self, numT, [&](TentSim* s, int batchStart, int batchCount) {
      int numHashEntries = bpt * batchCount;
      uint32_t saved_hashes[MAX_BONES_PER_TENT * BATCH_SIZE]; // 20 max
      memcpy(saved_hashes, g_game_bone_table, numHashEntries * sizeof(uint32_t));
      memcpy(g_game_bone_table, &g_bone_hashes[bpt * batchStart], numHashEntries * sizeof(uint32_t));

      void** batchBonePtrs = &bonePtrs[batchStart * BONE_PTR_STRIDE];
      // First batch writes to caller's targetMatrices (fits in their stack frame).
      // Subsequent batches use our static buffer to avoid stack overflow.
      void* batchTargetMats = (batchStart == 0)
         ? targetMatrices
         : (void*)&s_targetMats_overflow[batchStart * bpt];
      game_UpdatePose(s, pose_raw, parentMatrix, batchBonePtrs, batchTargetMats);

      memcpy(g_game_bone_table, saved_hashes, numHashEntries * sizeof(uint32_t));
   });
}

// ============================================================================
// Per-build address table
// ============================================================================

struct tentacle_addrs {
   uintptr_t id_file_offset;
   uint64_t  id_expected;

   // Detours hook targets (file offsets)
   uintptr_t fn_constructor;
   uintptr_t fn_doTentacles;

   // Sub-function addresses (file offsets) — called directly, not hooked
   uintptr_t fn_updatePositions;
   uintptr_t fn_enforceCollisions;
   uintptr_t fn_updatePose;

   // Bone hash table VA (for verification)
   uintptr_t bone_hash_table_va;

   // Steam/GOG LTCG: UpdatePositions takes dt in XMM1 instead of stack
   bool xmm1_dt;

   // SetProperty tentacle limit (0 if not present for this build)
   uintptr_t setProperty_limit;

   // Memory pool size patches: 0x268 → 0x778
   uintptr_t memPool1, memPool2, memPool3;

   // Bitfield widening: numTentacles 3→4 bits, bpt shifted, collType unchanged
   // Layout: numT bits 7-10, bpt bits 11-13, collType bits 14-15 (no bit 16)
   static const int NUM_BF_PATCHES = 13;
   struct bf_patch { uintptr_t file_offset; uint8_t size; uint32_t old_val, new_val; };
   bf_patch bitfield[NUM_BF_PATCHES];
};

// clang-format off
static const tentacle_addrs MODTOOLS = {
   .id_file_offset = 0x62b59c,
   .id_expected    = 0x746163696c707041,

   .fn_constructor      = 0x16d090,
   .fn_doTentacles      = 0x16f4e0,
   .fn_updatePositions  = 0x16e420,
   .fn_enforceCollisions = 0x16f020,
   .fn_updatePose       = 0x16dc80,

   .bone_hash_table_va = 0xa442f0,
   .xmm1_dt = false,
   .setProperty_limit  = 0x141cd5,

   .memPool1 = 0x2745cd,
   .memPool2 = 0x617181,
   .memPool3 = 0x1347ca,

   .bitfield = {
      // EntitySoldierClass copy ctor
      {0x13F136, 4, 0x00000380, 0x00000780},   // numT mask
      {0x13F120, 4, 0x00003C00, 0x00003800},   // bpt mask (3 bits: 11-13)
      // SetProperty writes
      {0x141D13, 4, 0x00000380, 0x00000780},   // numT mask
      {0x13FC2E, 1, 0x0A, 0x0B},               // bpt shift 10→11
      {0x13FC35, 4, 0x00003C00, 0x00003800},   // bpt mask (3 bits: 11-13)
      // EntitySoldier ctor — bitfield extraction
      {0x1347AD, 4, 0x00000380, 0x00000780},   // TEST numT mask
      {0x1347F5, 1, 0x0A, 0x0B},               // bpt shift 10→11
      {0x1347F8, 1, 0x0F, 0x07},               // bpt AND 0xF→0x7
      {0x1347FF, 1, 0x07, 0x0F},               // numT AND 7→0xF
      // DisplaySoldier::Setup — bitfield extraction
      {0x2745A4, 4, 0x00000380, 0x00000780},   // TEST numT mask
      {0x2745F8, 1, 0x0A, 0x0B},               // bpt shift 10→11
      {0x2745FB, 1, 0x0F, 0x07},               // bpt AND 0xF→0x7
      {0x274602, 1, 0x07, 0x0F},               // numT AND 7→0xF
   },
};

static const tentacle_addrs STEAM = {
   .id_file_offset = 0x39f834,
   .id_expected    = 0x746163696c707041,

   .fn_constructor      = 0x255770,
   .fn_doTentacles      = 0x2558f0,
   .fn_updatePositions  = 0x256270,
   .fn_enforceCollisions = 0x2569c0,
   .fn_updatePose       = 0x255b60,

   .bone_hash_table_va = 0x78b630,
   .xmm1_dt = true,
   .setProperty_limit  = 0,

   .memPool1 = 0x0df909,
   .memPool2 = 0x0069d1,
   .memPool3 = 0x08da5e,

   .bitfield = {
      // EntitySoldierClass copy ctor
      {0x0f6004, 4, 0x00000380, 0x00000780},   // numT mask
      {0x0f5fef, 4, 0x00003C00, 0x00003800},   // bpt mask (3 bits: 11-13)
      // SetProperty writes
      {0x0fa2ee, 4, 0x00000380, 0x00000780},   // numT mask
      {0x0f84c3, 1, 0x0A, 0x0B},               // bpt shift 10→11
      {0x0f84cb, 4, 0x00003C00, 0x00003800},   // bpt mask (3 bits: 11-13)
      // EntitySoldier ctor — bitfield extraction
      {0x0df8ed, 4, 0x00000380, 0x00000780},   // TEST numT mask
      {0x0df935, 1, 0x0A, 0x0B},               // bpt shift 10→11
      {0x0df939, 1, 0x0F, 0x07},               // bpt AND 0xF→0x7
      {0x0df940, 1, 0x07, 0x0F},               // numT AND 7→0xF
      // SoldierElement::vfunction17 — bitfield extraction
      {0x08da35, 4, 0x00000380, 0x00000780},   // TEST numT mask
      {0x08da8a, 1, 0x0A, 0x0B},               // bpt shift 10→11
      {0x08da8e, 1, 0x0F, 0x07},               // bpt AND 0xF→0x7
      {0x08da95, 1, 0x07, 0x0F},               // numT AND 7→0xF
   },
};

static const tentacle_addrs GOG = {
   .id_file_offset = 0x3a0698,
   .id_expected    = 0x746163696c707041,

   .fn_constructor      = 0x256810,
   .fn_doTentacles      = 0x256990,
   .fn_updatePositions  = 0x257310,
   .fn_enforceCollisions = 0x257a60,
   .fn_updatePose       = 0x256c00,

   .bone_hash_table_va = 0x78c5d0,
   .xmm1_dt = true,
   .setProperty_limit  = 0,

   .memPool1 = 0x0df909,
   .memPool2 = 0x0069d1,
   .memPool3 = 0x08da5e,

   .bitfield = {
      // EntitySoldierClass copy ctor
      {0x0f6004, 4, 0x00000380, 0x00000780},   // numT mask
      {0x0f5fef, 4, 0x00003C00, 0x00003800},   // bpt mask (3 bits: 11-13)
      // SetProperty writes
      {0x0fa2ee, 4, 0x00000380, 0x00000780},   // numT mask
      {0x0f84c3, 1, 0x0A, 0x0B},               // bpt shift 10→11
      {0x0f84cb, 4, 0x00003C00, 0x00003800},   // bpt mask (3 bits: 11-13)
      // EntitySoldier ctor — bitfield extraction
      {0x0df8ed, 4, 0x00000380, 0x00000780},   // TEST numT mask
      {0x0df935, 1, 0x0A, 0x0B},               // bpt shift 10→11
      {0x0df939, 1, 0x0F, 0x07},               // bpt AND 0xF→0x7
      {0x0df940, 1, 0x07, 0x0F},               // numT AND 7→0xF
      // SoldierElement::vfunction17 — bitfield extraction
      {0x08da35, 4, 0x00000380, 0x00000780},   // TEST numT mask
      {0x08da8a, 1, 0x0A, 0x0B},               // bpt shift 10→11
      {0x08da8e, 1, 0x0F, 0x07},               // bpt AND 0xF→0x7
      {0x08da95, 1, 0x07, 0x0F},               // numT AND 7→0xF
   },
};
// clang-format on

// ============================================================================
// Binary patch application
// ============================================================================

static bool apply_binary_patches(uintptr_t exe_base, const tentacle_addrs& a)
{
   int total = 0;
   bool ok = true;

   // SetProperty limit: 4 → 9
   if (a.setProperty_limit) {
      uint8_t* p = (uint8_t*)(a.setProperty_limit + exe_base);
      if (*p == 0x04) { *p = 0x09; total++; }
      else { dbg_log("[Tentacle] SetProperty limit: expected 04, found %02x\n", *p); }
   }

   // Memory pool: 0x268 → 0x778
   uintptr_t pools[] = { a.memPool1, a.memPool2, a.memPool3 };
   for (int i = 0; i < 3; i++) {
      if (!pools[i]) continue;
      uint32_t* p = (uint32_t*)(pools[i] + exe_base);
      if (*p == 0x268) { *p = 0x778; total++; }
      else { dbg_log("[Tentacle] Pool %d: expected 268, found %x\n", i, *p); ok = false; }
   }

   // NOTE: Render stack patches (SUB ESP widening) were REMOVED. Those functions
   // use ESP-relative addressing after AND ESP alignment, so changing SUB ESP
   // shifts all local variable offsets and crashes. Our hooked DoTentacles handles
   // >4 tentacles via batching + a static overflow buffer for targetMatrices.

   // Bitfield widening
   for (int i = 0; i < tentacle_addrs::NUM_BF_PATCHES; i++) {
      const auto& bp = a.bitfield[i];
      uintptr_t addr = bp.file_offset + exe_base;
      if (bp.size == 1) {
         uint8_t* p = (uint8_t*)addr;
         if (*p == (uint8_t)bp.old_val) { *p = (uint8_t)bp.new_val; total++; }
         else dbg_log("[Tentacle] BF[%d]: expected %02x, found %02x\n", i, bp.old_val, *p);
      } else {
         uint32_t* p = (uint32_t*)addr;
         if (*p == bp.old_val) { *p = bp.new_val; total++; }
         else { dbg_log("[Tentacle] BF[%d]: expected %08x, found %08x\n", i, bp.old_val, *p); ok = false; }
      }
   }

   dbg_log("[Tentacle] Binary patches: %d applied (ok=%d)\n", total, (int)ok);
   return ok;
}

// ============================================================================
// Detours install/uninstall
// ============================================================================

static bool g_installed = false;

static bool install_hooks(uintptr_t exe_base, const tentacle_addrs& a)
{
   auto resolve = [=](uintptr_t off) { return off + exe_base; };

   original_Ctor         = (fn_Ctor)resolve(a.fn_constructor);
   original_DoTentacles  = (fn_DoTentacles)resolve(a.fn_doTentacles);
   game_EnforceCollisions = (fn_EnforceCollisions)resolve(a.fn_enforceCollisions);
   game_UpdatePose       = (fn_UpdatePose)resolve(a.fn_updatePose);

   if (a.xmm1_dt) {
      // Steam/GOG LTCG: dt passed in XMM1, use naked shim
      g_raw_UpdatePositions = (void*)resolve(a.fn_updatePositions);
      game_UpdatePositions  = (fn_UpdatePositions)(void*)shim_UpdatePositions_steam;
   } else {
      game_UpdatePositions  = (fn_UpdatePositions)resolve(a.fn_updatePositions);
   }

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_Ctor, hooked_Ctor);
   DetourAttach(&(PVOID&)original_DoTentacles, hooked_DoTentacles);
   LONG result = DetourTransactionCommit();

   if (result != NO_ERROR) {
      dbg_log("[Tentacle] Detours commit FAILED (%ld)\n", result);
      return false;
   }

   g_installed = true;
   dbg_log("[Tentacle] Hooks installed\n");
   return true;
}

// ============================================================================
// Public API
// ============================================================================

bool patch_tentacle_limit(uintptr_t exe_base)
{
   dbg_log("[Tentacle] Tentacle bone limit patch (batch approach)\n");

   static const tentacle_addrs* builds[] = { &MODTOOLS, &STEAM, &GOG };
   for (const tentacle_addrs* b : builds) {
      char* id = (char*)(b->id_file_offset + exe_base);
      if (memcmp(id, &b->id_expected, sizeof(b->id_expected)) != 0)
         continue;

      uintptr_t ht_va = b->bone_hash_table_va - 0x400000 + exe_base;
      if (!init_bone_hashes(ht_va)) return false;
      g_game_bone_table = (uint32_t*)ht_va;
      dbg_log("[Tentacle] Bone hashes initialized (%d entries, verified)\n", TOTAL_BONE_HASHES);

      if (!apply_binary_patches(exe_base, *b)) return false;
      if (!install_hooks(exe_base, *b)) return false;
      return true;
   }

   dbg_log("[Tentacle] No matching build, skipping\n");
   return true;
}

void unpatch_tentacle_limit()
{
   if (!g_installed) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_Ctor)        DetourDetach(&(PVOID&)original_Ctor, hooked_Ctor);
   if (original_DoTentacles) DetourDetach(&(PVOID&)original_DoTentacles, hooked_DoTentacles);
   DetourTransactionCommit();

   g_installed = false;
}
