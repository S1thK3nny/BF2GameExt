#include "pch.h"
#include "tentacle_limit.hpp"
#include "tentacle_timing.hpp"
#include "tentacle_timing_sites.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <detours.h>

// =============================================================================
// Tentacle limit increase: 4 -> 9 tentacles per soldier class
//
// Ported from RJP1992's origin/dinput-hook branch (PatcherDLL/src/tentacle_patch.cpp),
// re-derived against all three shipped executables and the Phantom dev build.
// Corrections made during the port are marked "PORT:" below.
//
// -----------------------------------------------------------------------------
// What the engine does
//
// EntitySoldierClass packs three tentacle properties into one bitfield word
// (+0x8BC on modtools, +0x6C8 on Steam/GOG):
//
//     bits  7- 9   NumTentacles       0-7    against arrays dimensioned for 4
//     bits 10-13   BonesPerTentacle   0-15   against a fixed 4x5 stack array
//     bits 14-15   TentacleCollType   0-3
//
// EntitySoldier's constructor extracts those three and passes them to
// TentacleSimulator's constructor; the simulator is a 0x268-byte block out of
// a dedicated MemoryPool ("TentacleSimulator", item size pushed as an immediate
// at three sites).  Its layout, verified byte-for-byte off the Steam
// constructor at 0x00655770 and confirmed against modtools UpdatePositions:
//
//     0x000  PblVector3 tPos[4][6]          simulated bone positions
//     0x120  PblVector3 oldPos[4][6]        previous positions (Verlet)
//     0x240  PblVector3 oldVelocity
//     0x24C  float      mInternalTimer
//     0x250  float      mTimeSinceLastUpdate    net extrapolation only
//     0x254  float      mTimerOffset            net extrapolation only
//     0x258  int        mNumTentacles
//     0x25C  int        mBonesPerTentacle
//     0x260  int        mCollType
//     0x264  bool       mFirstUpdate
//                                              sizeof == 0x268
//
// DoTentacles is pure orchestration -- timing, bone lookup, first-frame seeding
// -- and then calls UpdatePositions / EnforceCollisions / UpdatePose, all three
// of which read their extent from mNumTentacles and mBonesPerTentacle.  So the
// simulation math never has to be reimplemented: hook the constructor and
// DoTentacles, keep the real arrays in an extended block past the stock 0x268,
// and drive the stock sub-functions in groups of four, copying each group into
// and back out of the addresses they expect.
//
// -----------------------------------------------------------------------------
// Bitfield widening
//
// NumTentacles needs a fourth bit to reach 9.  It is taken from
// BonesPerTentacle, whose useful range is 0-5 and which therefore only needs
// three.  TentacleCollType is untouched, so no site outside this file changes
// meaning and every edit is length-neutral (a mask immediate or a shift count):
//
//     bits  7-10   NumTentacles       0-15
//     bits 11-13   BonesPerTentacle   0-7
//     bits 14-15   TentacleCollType   0-3   (unchanged)
//
// A sweep of every reference to the class bitfield displacement in all three
// .text sections finds 21 sites per build that touch these two fields, and all
// 21 fall inside the five regions patched below (class copy, the two
// SetProperty writes, EntitySoldier's constructor, and the spawn-screen display
// soldier).  Nothing else decodes them.
//
// -----------------------------------------------------------------------------
// PORT corrections against RJP's version
//
//   1. oldVelocity.  UpdatePositions derives the frame's acceleration from
//      (velocity - mOldVelocity)/dt and then overwrites mOldVelocity with
//      velocity.  Calling it once per batch left batches 2 and 3 with a zero
//      delta, so tentacles 5-9 never felt the carrier move -- gravity and the
//      constraints only.  The value is now snapshotted, restored before each
//      batch, and committed once afterwards.
//   2. Clamping.  Widening the field to four bits lets an ODF ask for up to 15
//      tentacles, which overruns both the 9-slot heap arrays and the 45-entry
//      bone pointer array on the stack.  The hooked constructor clamps.
//   3. Null pose. The stock DoTentacles checks the pose after selecting timing;
//      the replacement dereferenced it unconditionally.
//   4. All-or-nothing patching.  Every site is verified before any byte is
//      written, and a failed Detours commit rolls the bytes back.  The previous
//      version could leave the executable half-patched -- pool grown but masks
//      stock, or the masks themselves half-swapped.
//   5. Uninstall reverts the byte patches too.  Detaching the hooks while the
//      widened masks stayed in place would have handed the stock DoTentacles up
//      to 15 tentacles against its 4-slot arrays.
//   6. mFirstUpdate is written as the byte the engine declares, not an int.
//   7. Install-time logging goes to BF2GameExt.log through the CRT, never
//      through the engine's RedWarning logger.  dllmain holds every exe section
//      at PAGE_READWRITE until all installers have run, so calling engine code
//      from an installer executes non-executable .text: an instant EXEC access
//      violation on Steam and GOG, where DEP is on.  (Cost a launch-blocking
//      crash on both retail builds during this port; modtools has no DEP and
//      tolerated it silently, which is exactly why it slipped through.)
//
// -----------------------------------------------------------------------------
// Timing and output ownership
//
// Keep all three native timing paths, including supplied dt == 0 for a pose-only
// render and network extrapolation. The original UpdateTimer remains in use.
// Each of the three render callers owns an expanded matrix buffer in a wrapper
// frame. It stays alive until that caller finishes drawing, including nested
// reflection/addon calls; no output matrices are shared between those frames.
// UpdatePose writes its stock names into a private batch pose, and we publish
// the returned matrix pointers under the actual names in the original pose.
// The engine's bone-name table remains read-only throughout.
//
// One modtools-only engine defect this incidentally papers over: in
// BattlefrontII.Debug.FullScreen.1080.exe the stock constructor's zeroing loop
// and DoTentacles' first-frame seeding address oldPos at this+0x260 rather than
// this+0x120, and DoTentacles reads mFirstUpdate at this+0x4A4 where the
// constructor writes it at this+0x264.  EnforceCollisions (mCollType at +0x260)
// and UpdatePositions (oldPos at tPos+0x120) are correct, so the simulation
// itself is fine, but the stock seeding pass writes past the 0x268 pool block
// and over mCollType.  Steam, GOG and Phantom are all correct.  Both offending
// functions are replaced here.
// =============================================================================

bool g_tentacleLimitEnabled = true;

namespace {

// install_log() is the ONLY logger that may run during install: dllmain holds the
// exe sections at PAGE_READWRITE (non-executable) until every installer has run,
// so calling the engine's own logger there executes non-executable .text and is
// an immediate EXEC access violation on the DEP-enabled retail builds. The fatal
// runtime context guard also uses this logger so it does not depend on a working
// engine log callback.
void install_log(const char* fmt, ...)
{
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

constexpr int kMaxTentacles    = 9;
constexpr int kMaxBones        = 5;
constexpr int kTentStride      = 6;    // tPos/oldPos slots per tentacle (5 bones + tip)
constexpr int kBonePtrStride   = 5;    // bonePtrs entries per tentacle
constexpr int kBatch           = 4;    // the stock arrays hold exactly this many
constexpr int kBoneTableSlots  = 20;   // the engine's static bone_string_ table (4 * 5)
constexpr int kPoseTableSize   = 0x100;
constexpr int kTotalBoneHashes = kMaxTentacles * kMaxBones; // 45 animated bones

constexpr uint32_t kStockPoolSize    = 0x268;
constexpr uint32_t kExtendedPoolSize = 0x778;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

struct PblVec3 { float x, y, z; };

// PblMatrix, 4x4 row-major; the translation row is at +0x30.
struct PblMat4 { float m[16]; };
static_assert(sizeof(PblMat4) == 0x40, "PblMatrix is 64 bytes");

// The extended simulator block.  Everything up to 0x268 is the stock layout and
// must stay exactly where it is -- the sub-functions we call still address it.
// tPos_legacy / oldPos_legacy are the stock arrays, used here as the scratch
// window each batch of four is copied through.
struct tentacle_sim {
   PblVec3 tPos_legacy[kBatch * kTentStride];   // 0x000
   PblVec3 oldPos_legacy[kBatch * kTentStride]; // 0x120
   PblVec3 oldVelocity;                         // 0x240
   float   mInternalTimer;                      // 0x24C
   float   mTimeSinceLastUpdate;                // 0x250
   float   mTimerOffset;                        // 0x254
   int     mNumTentacles;                       // 0x258
   int     mBonesPerTentacle;                   // 0x25C
   int     mCollType;                           // 0x260
   uint8_t mFirstUpdate;                        // 0x264
   uint8_t mPad[3];                             // 0x265
   // Ours, past the stock end of the object.
   PblVec3 tPos[kMaxTentacles * kTentStride];   // 0x268
   PblVec3 oldPos[kMaxTentacles * kTentStride]; // 0x4F0
};
static_assert(offsetof(tentacle_sim, oldPos_legacy)       == 0x120, "oldPos");
static_assert(offsetof(tentacle_sim, oldVelocity)         == 0x240, "oldVelocity");
static_assert(offsetof(tentacle_sim, mInternalTimer)      == 0x24C, "mInternalTimer");
static_assert(offsetof(tentacle_sim, mNumTentacles)       == 0x258, "mNumTentacles");
static_assert(offsetof(tentacle_sim, mBonesPerTentacle)   == 0x25C, "mBonesPerTentacle");
static_assert(offsetof(tentacle_sim, mCollType)           == 0x260, "mCollType");
static_assert(offsetof(tentacle_sim, mFirstUpdate)        == 0x264, "mFirstUpdate");
static_assert(offsetof(tentacle_sim, tPos)                == 0x268, "extended tPos");
static_assert(offsetof(tentacle_sim, oldPos)              == 0x4F0, "extended oldPos");
static_assert(sizeof(tentacle_sim) == kExtendedPoolSize, "extended block size");

// RedPose publishes matrix pointers; the storage must outlive DoTentacles and
// remain independent until its render caller has consumed the pose. A wrapper
// around each complete caller gives it exactly that lifetime. Nested callers
// link another frame without changing the outer frame's output.
struct pose_frame;
thread_local pose_frame* g_poseFrame = nullptr;

struct alignas(16) pose_frame {
   PblMat4 matrices[kMaxTentacles * kMaxBones];
   pose_frame* previous;
   bool claimed = false;

   pose_frame() : previous(g_poseFrame) { g_poseFrame = this; }
   ~pose_frame() { g_poseFrame = previous; }
   pose_frame(const pose_frame&) = delete;
   pose_frame& operator=(const pose_frame&) = delete;
};

struct batch_pose {
   uint32_t count = 0;
   uint32_t table[kPoseTableSize] = {};
};
static_assert(sizeof(batch_pose) == 0x404, "RedPose count and hash storage");

tentacle_timing::network_state g_timingNetwork{};

void fail_pose_context(const char* reason)
{
   install_log("[Tentacle] invalid pose context: %s", reason);
   FatalAppExitA(0, "BF2GameExt: invalid tentacle pose context. See BF2GameExt.log.");
}

// ---------------------------------------------------------------------------
// Bone name hashing -- CRC-32/BZIP2, the same one PblTEMPHash uses
// ---------------------------------------------------------------------------

uint32_t g_crcTable[256];
uint32_t g_boneHashes[kTotalBoneHashes];

void init_crc_table()
{
   for (int i = 0; i < 256; ++i) {
      uint32_t crc = (uint32_t)i << 24;
      for (int j = 0; j < 8; ++j)
         crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
      g_crcTable[i] = crc;
   }
}

uint32_t bone_hash(const char* str)
{
   uint32_t h = 0xFFFFFFFFu;
   for (; *str; ++str)
      h = g_crcTable[((h >> 24) ^ (uint8_t)*str) & 0xFF] ^ (h << 8);
   return h ^ 0xFFFFFFFFu;
}

// The engine's own table holds bone_string_1..bone_string_20.  Ours extends the
// same sequence to cover nine tentacles; entry 0 doubles as the presence guard
// DoTentacles tests before doing any work.
bool init_bone_hashes(const uint32_t* gameTable)
{
   init_crc_table();

   char name[32];
   for (int i = 0; i < kTotalBoneHashes; ++i) {
      sprintf_s(name, "bone_string_%d", i + 1);
      g_boneHashes[i] = bone_hash(name);
   }

   // Every entry the engine already has must come out identical, or our hash
   // is not the engine's hash and none of the extended lookups would resolve.
   for (int i = 0; i < kBoneTableSlots; ++i) {
      if (g_boneHashes[i] != gameTable[i]) {
         install_log("[Tentacle] NOT installed: bone hash %d mismatch "
                     "(computed %08X, engine %08X)", i + 1, g_boneHashes[i], gameTable[i]);
         return false;
      }
   }
   return true;
}

// ---------------------------------------------------------------------------
// PblHashTableCode::_Find, reimplemented
//
// Keys occupy the low half of the table and values the matching slot in the
// high half; the probe walks backwards and wraps.  The engine's version has no
// iteration cap and spins forever on a full table with a missing key, so this
// one is bounded -- it cannot do less work than the original, only stop sooner
// in the case the original never returns from.
// ---------------------------------------------------------------------------

void* ht_find(const uint32_t* table, int tableSize, uint32_t hash)
{
   if (hash == 0) return nullptr;

   const int half = tableSize >> 1;
   const int mask = half - 1;
   int idx = (int)(hash & (uint32_t)mask);

   for (int i = 0; i < half; ++i) {
      const uint32_t key = table[idx];
      if (key == hash) return (void*)(uintptr_t)table[half + idx];
      if (key == 0) return nullptr;
      idx = (idx - 1) & mask;
   }
   return nullptr;
}

uint32_t* ht_value_slot(uint32_t* table, uint32_t hash)
{
   if (hash == 0) return nullptr;
   constexpr int half = kPoseTableSize / 2;
   int idx = (int)(hash & (half - 1));
   for (int i = 0; i < half; ++i) {
      if (table[idx] == hash) return &table[half + idx];
      if (table[idx] == 0) return nullptr;
      idx = (idx - 1) & (half - 1);
   }
   return nullptr;
}

// ---------------------------------------------------------------------------
// Stock sub-functions, resolved at install time
// ---------------------------------------------------------------------------

using fn_ctor_t            = void*(__thiscall*)(void*, int, int, int);
using fn_doTentacles_t     = void(__thiscall*)(void*, void*, void*, void*, void*, float);
using fn_updatePositions_t = void(__thiscall*)(void*, float, void*, void*, void*, void**);
using fn_enforceColl_t     = void(__thiscall*)(void*, void*, void*);
using fn_updatePose_t      = void(__thiscall*)(void*, void*, void*, void**, void*);

fn_ctor_t            g_origCtor         = nullptr;
fn_doTentacles_t     g_origDoTentacles  = nullptr;
fn_updatePositions_t g_updatePositions  = nullptr;
fn_enforceColl_t     g_enforceColl      = nullptr;
fn_updatePose_t      g_updatePose       = nullptr;

using fn_soldierRender_t = void(__thiscall*)(void*, unsigned char, float, uint32_t);
using fn_selectionRender_t = void(__thiscall*)(void*, void*, void*);
using fn_addonRender_t = void(__thiscall*)(void*, void*, void*, void*, void*, void*, uint32_t);
fn_soldierRender_t g_origSoldierRender = nullptr;
fn_selectionRender_t g_origSelectionRender = nullptr;
fn_addonRender_t g_origAddonRender = nullptr;

void __fastcall hooked_soldier_render(void* self, void*, unsigned char pass, float fade, uint32_t flags)
{
   pose_frame frame;
   g_origSoldierRender(self, pass, fade, flags);
}

void __fastcall hooked_selection_render(void* self, void*, void* matrix, void* color)
{
   pose_frame frame;
   g_origSelectionRender(self, matrix, color);
}

void __fastcall hooked_addon_render(void* self, void*, void* simulator, void* velocity,
                                    void* pose, void* matrix, void* color, uint32_t flags)
{
   pose_frame frame;
   g_origAddonRender(self, simulator, velocity, pose, matrix, color, flags);
}

// Steam/GOG LTCG: UpdatePositions takes dt in XMM1 and drops it from the stack,
// so it is __thiscall(velocity, pose, parentMat, bonePtrs) + XMM1, RET 0x10 --
// against modtools' five stack arguments and RET 0x14.  This adapts the
// modtools shape to it so the caller below stays build-independent.  ECX (this)
// passes straight through.
void* g_rawUpdatePositions = nullptr;

__declspec(naked) void shim_update_positions_retail()
{
   __asm {
      movss xmm1, [esp + 4]        // dt off our stack into XMM1
      push  ebp
      mov   ebp, esp
      push  dword ptr [ebp + 24]   // bonePtrs
      push  dword ptr [ebp + 20]   // parentMatrix
      push  dword ptr [ebp + 16]   // pose
      push  dword ptr [ebp + 12]   // velocity
      mov   eax, dword ptr [g_rawUpdatePositions]
      call  eax                    // RET 0x10 cleans the four we pushed
      pop   ebp
      ret   0x14                   // clean our own five
   }
}

// ---------------------------------------------------------------------------
// Batching
// ---------------------------------------------------------------------------

void batch_copy_in(tentacle_sim* self, int first, int count)
{
   const size_t bytes = (size_t)count * kTentStride * sizeof(PblVec3);
   std::memcpy(self->tPos_legacy,   &self->tPos[first * kTentStride],   bytes);
   std::memcpy(self->oldPos_legacy, &self->oldPos[first * kTentStride], bytes);
}

void batch_copy_out(tentacle_sim* self, int first, int count)
{
   const size_t bytes = (size_t)count * kTentStride * sizeof(PblVec3);
   std::memcpy(&self->tPos[first * kTentStride],   self->tPos_legacy,   bytes);
   std::memcpy(&self->oldPos[first * kTentStride], self->oldPos_legacy, bytes);
}

// ---------------------------------------------------------------------------
// Hooked constructor
//
// The stock constructor zeroes tPos/oldPos using the members it has not
// assigned yet, so on a fresh pool block it loops over whatever the pool's fill
// pattern happens to decode as.  Zeroing the whole block first makes that moot
// and gives the extended arrays a defined starting state.
// ---------------------------------------------------------------------------

void* __fastcall hooked_ctor(tentacle_sim* self, void* /*edx*/,
                             int numTentacles, int bonesPerTentacle, int collType)
{
   std::memset(self, 0, sizeof(tentacle_sim));

   // The widened bitfield accepts 0-15 tentacles and 0-7 bones; the arrays here
   // hold 9 and 5.  An ODF typo must not reach past them.
   if (numTentacles < 0) numTentacles = 0;
   if (numTentacles > kMaxTentacles) numTentacles = kMaxTentacles;
   if (bonesPerTentacle < 0) bonesPerTentacle = 0;
   if (bonesPerTentacle > kMaxBones) bonesPerTentacle = kMaxBones;

   self->mNumTentacles     = numTentacles;
   self->mBonesPerTentacle = bonesPerTentacle;
   self->mCollType         = collType;
   self->mFirstUpdate      = 1;

   return self; // the caller keeps EAX as the simulator pointer
}

// ---------------------------------------------------------------------------
// Hooked DoTentacles -- orchestration only, no simulation math
// ---------------------------------------------------------------------------

void __fastcall hooked_do_tentacles(tentacle_sim* self, void* /*edx*/,
                                    void* pose, void* parentMatrix, void* velocity,
                                    void* targetMatrices, float dtIn)
{
   const float dt = tentacle_timing::select_elapsed_time(
      self->mInternalTimer, self->mTimeSinceLastUpdate, self->mTimerOffset,
      dtIn, pose != nullptr, g_timingNetwork);
   if (!pose) return;

   int numT = self->mNumTentacles;
   int bpt  = self->mBonesPerTentacle;
   if (numT > kMaxTentacles) numT = kMaxTentacles;
   if (bpt  > kMaxBones)     bpt  = kMaxBones;
   if (numT <= 0 || bpt <= 0) return;

   // RedPose: [0] is the entry count, followed by keys and matching values.
   uint32_t* poseTable = (uint32_t*)((uintptr_t)pose + 4);

   // ---- nothing to do unless this pose actually has tentacle bones ----------
   if (!ht_find(poseTable, kPoseTableSize, g_boneHashes[0])) return;

   // ---- bone lookup for every tentacle -------------------------------------
   void* bonePtrs[kMaxTentacles * kBonePtrStride] = {};
   uint32_t* poseSlots[kMaxTentacles * kMaxBones] = {};
   for (int t = 0; t < numT; ++t) {
      for (int b = 0; b < bpt; ++b) {
         uint32_t* slot = ht_value_slot(poseTable, g_boneHashes[bpt * t + b]);
         if (!slot || !*slot) {
            fail_pose_context("missing bone in the requested tentacle rig");
            return;
         }
         poseSlots[bpt * t + b] = slot;
         bonePtrs[t * kBonePtrStride + b] = (void*)(uintptr_t)*slot;
      }
   }

   PblMat4* output = (PblMat4*)targetMatrices;
   if (output && numT > kBatch) {
      if (!g_poseFrame || g_poseFrame->claimed) {
         fail_pose_context("extended output has no independent render frame");
         return;
      }
      g_poseFrame->claimed = true;
      output = g_poseFrame->matrices;
   }

   // ---- first frame: seed both position buffers from the supplied pose ------
   if (self->mFirstUpdate) {
      for (int t = 0; t < numT; ++t) {
         for (int b = 0; b < bpt; ++b) {
            const PblMat4* bone = (const PblMat4*)bonePtrs[t * kBonePtrStride + b];
            if (!bone) continue;
            const PblVec3 trans = *(const PblVec3*)&bone->m[12]; // matrix +0x30
            const int slot = t * kTentStride + b;
            self->tPos[slot]   = trans;
            self->oldPos[slot] = trans;
         }
      }
      self->mFirstUpdate = 0;
   }

   const int savedNumT = self->mNumTentacles;

   // ---- integrate + collide -------------------------------------------------
   if (dt > 0.0f) {
      // UpdatePositions consumes mOldVelocity and then overwrites it, so every
      // batch has to see the same pre-frame value and only the first batch's
      // result is kept (they all write the identical current velocity anyway).
      const PblVec3 velocityAtEntry = self->oldVelocity;
      PblVec3       velocityAtExit  = velocityAtEntry;
      bool          haveExit        = false;

      for (int first = 0; first < numT; first += kBatch) {
         const int count = (numT - first < kBatch) ? (numT - first) : kBatch;

         batch_copy_in(self, first, count);
         self->mNumTentacles = count;
         self->oldVelocity   = velocityAtEntry;

         g_updatePositions(self, dt, velocity, pose, parentMatrix,
                           &bonePtrs[first * kBonePtrStride]);

         if (!haveExit) { velocityAtExit = self->oldVelocity; haveExit = true; }
         batch_copy_out(self, first, count);
      }
      self->oldVelocity = velocityAtExit;

      for (int first = 0; first < numT; first += kBatch) {
         const int count = (numT - first < kBatch) ? (numT - first) : kBatch;

         batch_copy_in(self, first, count);
         self->mNumTentacles = count;
         g_enforceColl(self, pose, parentMatrix);
         batch_copy_out(self, first, count);
      }
   }

   // The native math only uses the pose for its final Remove/Store operations.
   // Let it publish stock names into a private table, then replace the existing
   // original pose values under the real names. Source pointers were collected
   // before any replacement, preserving each chain's native matrix inputs.
   if (output) {
      for (int first = 0; first < numT; first += kBatch) {
         const int count = (numT - first < kBatch) ? (numT - first) : kBatch;
         batch_pose result;
         batch_copy_in(self, first, count);
         self->mNumTentacles = count;
         PblMat4* target = output + first * bpt;
         g_updatePose(self, &result, parentMatrix, &bonePtrs[first * kBonePtrStride], target);
         for (int i = 0; i < count * bpt; ++i) {
            void* matrix = ht_find(result.table, kPoseTableSize, g_boneHashes[i]);
            if (matrix != target + i) {
               self->mNumTentacles = savedNumT;
               fail_pose_context("native UpdatePose did not publish its output matrix");
               return;
            }
            *poseSlots[first * bpt + i] = (uint32_t)(uintptr_t)matrix;
         }
         batch_copy_out(self, first, count);
      }
   }

   self->mNumTentacles = savedNumT;
}

// ---------------------------------------------------------------------------
// Per-build patch sites
//
// Addresses are unrelocated (imagebase 0x400000).  Every one was checked
// byte-for-byte against the shipped executables; `stock` is what must be there
// before anything is written.
// ---------------------------------------------------------------------------

struct byte_patch {
   uintptr_t va;
   uint8_t   width;   // 1 or 4
   uint32_t  stock;
   uint32_t  patched;
};

struct build_patches {
   bool       dtInXmm1;          // Steam/GOG UpdatePositions convention
   uintptr_t  warnThreshold;     // `CMP EAX,4` in the "Too many tentacles!" check; 0 where retail compiled it out
   uintptr_t  poolSize[3];       // the three `PUSH 0x268` immediates
   byte_patch bitfield[13];
};

// clang-format off
const build_patches kModtools = {
   /*dtInXmm1*/ false,
   /*warn*/     0x00541CD5,
   /*pool*/     { 0x006745CD, 0x00A17181, 0x005347CA },
   {
      // EntitySoldierClass field-by-field copy
      { 0x0053F136, 4, 0x00000380, 0x00000780 }, // NumTentacles mask
      { 0x0053F120, 4, 0x00003C00, 0x00003800 }, // BonesPerTentacle mask
      // EntitySoldierClass::SetProperty stores
      { 0x00541D13, 4, 0x00000380, 0x00000780 }, // NumTentacles mask
      { 0x0053FC2E, 1, 0x0A,       0x0B       }, // BonesPerTentacle shift
      { 0x0053FC35, 4, 0x00003C00, 0x00003800 }, // BonesPerTentacle mask
      // EntitySoldier constructor extraction
      { 0x005347AD, 4, 0x00000380, 0x00000780 }, // presence TEST mask
      { 0x005347F5, 1, 0x0A,       0x0B       }, // BonesPerTentacle shift
      { 0x005347F8, 1, 0x0F,       0x07       }, // BonesPerTentacle AND
      { 0x005347FF, 1, 0x07,       0x0F       }, // NumTentacles AND
      // Spawn-screen display soldier
      { 0x006745A4, 4, 0x00000380, 0x00000780 },
      { 0x006745F8, 1, 0x0A,       0x0B       },
      { 0x006745FB, 1, 0x0F,       0x07       },
      { 0x00674602, 1, 0x07,       0x0F       },
   },
};

// GOG is Steam plus 0x10A0 through the tentacle simulator itself, but every site
// below sits before the shift and shares Steam's address; verified in both.
const build_patches kRetail = {
   /*dtInXmm1*/ true,
   /*warn*/     0,
   /*pool*/     { 0x004DF909, 0x004069D1, 0x0048DA5E },
   {
      { 0x004F6004, 4, 0x00000380, 0x00000780 },
      { 0x004F5FEF, 4, 0x00003C00, 0x00003800 },
      { 0x004FA2EE, 4, 0x00000380, 0x00000780 },
      { 0x004F84C3, 1, 0x0A,       0x0B       },
      { 0x004F84CB, 4, 0x00003C00, 0x00003800 },
      { 0x004DF8ED, 4, 0x00000380, 0x00000780 },
      { 0x004DF935, 1, 0x0A,       0x0B       },
      { 0x004DF939, 1, 0x0F,       0x07       },
      { 0x004DF940, 1, 0x07,       0x0F       },
      { 0x0048DA35, 4, 0x00000380, 0x00000780 },
      { 0x0048DA8A, 1, 0x0A,       0x0B       },
      { 0x0048DA8E, 1, 0x0F,       0x07       },
      { 0x0048DA95, 1, 0x07,       0x0F       },
   },
};
// clang-format on

constexpr int kBitfieldSites = (int)(sizeof(kModtools.bitfield) / sizeof(kModtools.bitfield[0]));

// ---------------------------------------------------------------------------
// Applying and reverting
// ---------------------------------------------------------------------------

struct applied_site {
   void*    addr;
   uint8_t  width;
   uint32_t stock;
};

applied_site g_applied[kBitfieldSites + 3 + 1] = {};
int          g_appliedCount = 0;
bool         g_installed    = false;

uint32_t read_site(const void* p, uint8_t width)
{
   return (width == 1) ? (uint32_t) * (const uint8_t*)p : *(const uint32_t*)p;
}

void write_site(void* p, uint8_t width, uint32_t value)
{
   if (width == 1) *(uint8_t*)p = (uint8_t)value;
   else            *(uint32_t*)p = value;
}

// .text and .rdata are RW for the duration of the install window, so the
// forward direction needs no VirtualProtect; the revert can run afterwards and
// does.
void record_and_write(void* p, uint8_t width, uint32_t stock, uint32_t patched)
{
   g_applied[g_appliedCount++] = { p, width, stock };
   write_site(p, width, patched);
}

void revert_all()
{
   for (int i = g_appliedCount - 1; i >= 0; --i) {
      applied_site& s = g_applied[i];
      if (!s.addr) continue;
      if (s.width == 1) {
         const uint8_t v = (uint8_t)s.stock;
         protected_write(s.addr, &v, 1);
      } else {
         protected_write(s.addr, &s.stock, 4);
      }
      s.addr = nullptr;
   }
   g_appliedCount = 0;
}

const build_patches* patches_for_build()
{
   switch (g_build) {
   case GameBuild::Modtools: return &kModtools;
   case GameBuild::Steam:
   case GameBuild::GOG:      return &kRetail;
   default:                  return nullptr;
   }
}

struct build_functions {
   uintptr_t ctor, doTentacles, updatePositions, enforceCollisions, updatePose, boneTable;
};

struct render_sites {
   uintptr_t soldier, selection, addon;
   uintptr_t calls[3];
   uintptr_t doTarget;
};

const render_sites* renders_for_build()
{
   // All three are thiscall, with 3, 2 and 6 stack arguments respectively.
   static constexpr render_sites modtools = {
      0x00535D90, 0x00674890, 0x0056FE80,
      { 0x00536F50, 0x00674E32, 0x0056FFD4 }, 0x0040F2A9
   };
   static constexpr render_sites steam = {
      0x004E23D0, 0x0048DC90, 0x00443CA0,
      { 0x004E3644, 0x0048E2A6, 0x00443E07 }, 0x006558F0
   };
   static constexpr render_sites gog = {
      0x004E23D0, 0x0048DC90, 0x00443C80,
      { 0x004E3644, 0x0048E2A6, 0x00443DE7 }, 0x00656990
   };
   switch (g_build) {
   case GameBuild::Modtools: return &modtools;
   case GameBuild::Steam: return &steam;
   case GameBuild::GOG: return &gog;
   default: return nullptr;
   }
}

bool verify_render_sites(uintptr_t exe_base, const render_sites& sites, const build_functions& fn)
{
   constexpr uint8_t debugCtor[] = { 0x83, 0xEC, 0x14, 0x8B, 0xC1 };
   constexpr uint8_t retailCtor[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };
   const bool debug = g_build == GameBuild::Modtools;
   if (std::memcmp(resolve(exe_base, fn.ctor), debug ? debugCtor : retailCtor,
                   debug ? sizeof(debugCtor) : sizeof(retailCtor)) != 0) {
      install_log("[Tentacle] NOT installed: constructor entry mismatch");
      return false;
   }
   constexpr uint8_t frame[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
   const uintptr_t entries[] = { sites.soldier, sites.selection, sites.addon };
   for (uintptr_t va : entries) {
      if (std::memcmp(resolve(exe_base, va), frame, sizeof(frame)) != 0) {
         install_log("[Tentacle] NOT installed: render entry mismatch at %08X", (unsigned)va);
         return false;
      }
   }
   for (uintptr_t va : sites.calls) {
      const uint8_t* call = (const uint8_t*)resolve(exe_base, va);
      int32_t displacement;
      std::memcpy(&displacement, call + 1, sizeof(displacement));
      if (*call != 0xE8 || call + 5 + displacement != resolve(exe_base, sites.doTarget)) {
         install_log("[Tentacle] NOT installed: render call mismatch at %08X", (unsigned)va);
         return false;
      }
   }
   if (g_build == GameBuild::Modtools) {
      const uint8_t* thunk = (const uint8_t*)resolve(exe_base, sites.doTarget);
      int32_t displacement;
      std::memcpy(&displacement, thunk + 1, sizeof(displacement));
      if (*thunk != 0xE9 || thunk + 5 + displacement != resolve(exe_base, fn.doTentacles)) {
         install_log("[Tentacle] NOT installed: DoTentacles thunk mismatch");
         return false;
      }
   }
   return true;
}

bool functions_for_build(build_functions& out)
{
   switch (g_build) {
   case GameBuild::Modtools: {
      using namespace game_addrs::modtools;
      out = { tentacle_ctor, tentacle_do_tentacles, tentacle_update_positions,
              tentacle_enforce_collisions, tentacle_update_pose, tentacle_bone_hashes };
      return true;
   }
   case GameBuild::Steam: {
      using namespace game_addrs::steam;
      out = { tentacle_ctor, tentacle_do_tentacles, tentacle_update_positions,
              tentacle_enforce_collisions, tentacle_update_pose, tentacle_bone_hashes };
      return true;
   }
   case GameBuild::GOG: {
      using namespace game_addrs::gog;
      out = { tentacle_ctor, tentacle_do_tentacles, tentacle_update_positions,
              tentacle_enforce_collisions, tentacle_update_pose, tentacle_bone_hashes };
      return true;
   }
   default:
      return false;
   }
}

} // namespace

// =============================================================================
// Install / uninstall
// =============================================================================

void tentacle_limit_install(uintptr_t exe_base)
{
   if (!g_tentacleLimitEnabled) return;
   if (g_installed) return;

   const build_patches* p = patches_for_build();
   const render_sites* renders = renders_for_build();
   build_functions fn{};
   if (!p || !renders || !functions_for_build(fn)) return;
   if (!verify_render_sites(exe_base, *renders, fn)) return;
   if (!tentacle_timing::resolve_network_state(exe_base, g_build, g_timingNetwork)) {
      install_log("[Tentacle] NOT installed: native timing signature mismatch");
      return;
   }

   // ---- the engine's bone table, and proof our hash matches its hash --------
   const auto* boneTable = (const uint32_t*)resolve(exe_base, fn.boneTable);
   if (!init_bone_hashes(boneTable)) return;

   // ---- verify every site before writing any of them ------------------------
   for (int i = 0; i < kBitfieldSites; ++i) {
      const byte_patch& bp = p->bitfield[i];
      const void* at = resolve(exe_base, bp.va);
      if (read_site(at, bp.width) != bp.stock) {
         install_log("[Tentacle] NOT installed: bitfield site %d at 0x%08X reads %08X, "
                     "expected %08X", i, (unsigned)bp.va, read_site(at, bp.width), bp.stock);
         return;
      }
   }
   for (int i = 0; i < 3; ++i) {
      const void* at = resolve(exe_base, p->poolSize[i]);
      if (*(const uint32_t*)at != kStockPoolSize) {
         install_log("[Tentacle] NOT installed: pool size site %d at 0x%08X reads 0x%X, "
                     "expected 0x%X", i, (unsigned)p->poolSize[i], *(const uint32_t*)at,
                     kStockPoolSize);
         return;
      }
   }
   if (p->warnThreshold) {
      const void* at = resolve(exe_base, p->warnThreshold);
      if (*(const uint8_t*)at != kBatch) {
         install_log("[Tentacle] NOT installed: warn threshold at 0x%08X reads %02X, "
                     "expected %02X", (unsigned)p->warnThreshold, *(const uint8_t*)at, kBatch);
         return;
      }
   }

   // ---- hooks ---------------------------------------------------------------
   g_origCtor        = (fn_ctor_t)resolve(exe_base, fn.ctor);
   g_origDoTentacles = (fn_doTentacles_t)resolve(exe_base, fn.doTentacles);
   g_enforceColl     = (fn_enforceColl_t)resolve(exe_base, fn.enforceCollisions);
   g_updatePose      = (fn_updatePose_t)resolve(exe_base, fn.updatePose);
   g_origSoldierRender = (fn_soldierRender_t)resolve(exe_base, renders->soldier);
   g_origSelectionRender = (fn_selectionRender_t)resolve(exe_base, renders->selection);
   g_origAddonRender = (fn_addonRender_t)resolve(exe_base, renders->addon);

   if (p->dtInXmm1) {
      g_rawUpdatePositions = resolve(exe_base, fn.updatePositions);
      g_updatePositions    = (fn_updatePositions_t)(void*)shim_update_positions_retail;
   } else {
      g_updatePositions    = (fn_updatePositions_t)resolve(exe_base, fn.updatePositions);
   }

   LONG rc = DetourTransactionBegin();
   if (rc != NO_ERROR) {
      install_log("[Tentacle] NOT installed: Detours begin failed (%ld)", rc);
      return;
   }
   rc = DetourUpdateThread(GetCurrentThread());
   if (rc == NO_ERROR) rc = DetourAttach(&(PVOID&)g_origCtor, hooked_ctor);
   if (rc == NO_ERROR) rc = DetourAttach(&(PVOID&)g_origDoTentacles, hooked_do_tentacles);
   if (rc == NO_ERROR) rc = DetourAttach(&(PVOID&)g_origSoldierRender, hooked_soldier_render);
   if (rc == NO_ERROR) rc = DetourAttach(&(PVOID&)g_origSelectionRender, hooked_selection_render);
   if (rc == NO_ERROR) rc = DetourAttach(&(PVOID&)g_origAddonRender, hooked_addon_render);
   if (rc != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[Tentacle] NOT installed: Detours attach failed (%ld)", rc);
      return;
   }

   // Every signature and hook has passed before the dependent sizes change.
   for (int i = 0; i < kBitfieldSites; ++i) {
      const byte_patch& bp = p->bitfield[i];
      record_and_write(resolve(exe_base, bp.va), bp.width, bp.stock, bp.patched);
   }
   for (int i = 0; i < 3; ++i)
      record_and_write(resolve(exe_base, p->poolSize[i]), 4, kStockPoolSize, kExtendedPoolSize);
   if (p->warnThreshold)
      record_and_write(resolve(exe_base, p->warnThreshold), 1, kBatch, kMaxTentacles);
   rc = DetourTransactionCommit();

   if (rc != NO_ERROR) {
      // The widened masks are only safe while our replacements are in place.
      install_log("[Tentacle] NOT installed: Detours commit failed (%ld); bytes reverted", rc);
      revert_all();
      return;
   }

   g_installed = true;
   install_log("[Tentacle] installed (limit=%d, native timing, caller-owned poses, read-only bone names)", kMaxTentacles);
}

void tentacle_limit_uninstall()
{
   if (!g_installed) return;

   LONG rc = DetourTransactionBegin();
   if (rc != NO_ERROR) return;
   rc = DetourUpdateThread(GetCurrentThread());
   if (rc == NO_ERROR) rc = DetourDetach(&(PVOID&)g_origCtor, hooked_ctor);
   if (rc == NO_ERROR) rc = DetourDetach(&(PVOID&)g_origDoTentacles, hooked_do_tentacles);
   if (rc == NO_ERROR) rc = DetourDetach(&(PVOID&)g_origSoldierRender, hooked_soldier_render);
   if (rc == NO_ERROR) rc = DetourDetach(&(PVOID&)g_origSelectionRender, hooked_selection_render);
   if (rc == NO_ERROR) rc = DetourDetach(&(PVOID&)g_origAddonRender, hooked_addon_render);
   if (rc != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[Tentacle] uninstall failed; active patch retained (%ld)", rc);
      return;
   }
   rc = DetourTransactionCommit();
   if (rc != NO_ERROR) {
      install_log("[Tentacle] uninstall commit failed; active patch retained (%ld)", rc);
      return;
   }

   // Must follow the detach: the stock DoTentacles against widened masks would
   // take up to 15 tentacles into arrays that hold 4.
   revert_all();

   g_installed = false;
}
