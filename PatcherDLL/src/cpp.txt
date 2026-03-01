#include "pch.h"

#include "tentacle_patch.hpp"
#include "cfile.hpp"

// ============================================================================
// CRC-32/BZIP2 — used for bone name hashes in the TentacleSimulator table
// Poly 0x04C11DB7, MSB-first, init 0xFFFFFFFF, final XOR 0xFFFFFFFF
// ============================================================================

static uint32_t g_crc32_table[256];

static void init_crc32_table()
{
   for (int i = 0; i < 256; i++) {
      uint32_t crc = (uint32_t)i << 24;
      for (int j = 0; j < 8; j++) {
         if (crc & 0x80000000)
            crc = (crc << 1) ^ 0x04C11DB7;
         else
            crc <<= 1;
      }
      g_crc32_table[i] = crc;
   }
}

static uint32_t bone_hash(const char* str)
{
   uint32_t hash = 0xFFFFFFFF;
   while (*str) {
      hash = g_crc32_table[((hash >> 24) ^ (uint8_t)*str) & 0xFF] ^ (hash << 8);
      str++;
   }
   return hash ^ 0xFFFFFFFF;
}

// ============================================================================
// Expanded bone hash table — 45 entries (9 tentacles * 5 bones)
// Original table at game VA (modtools: 0xa442f0) has 20 entries.
// ============================================================================

static uint32_t g_bone_hashes[45];

static bool init_bone_hashes(uintptr_t original_table_addr, cfile& log)
{
   init_crc32_table();

   // Compute CRC-32/BZIP2 hashes for bone_string_1 through bone_string_45
   char name_buf[32];
   for (int i = 0; i < 45; i++) {
      sprintf_s(name_buf, "bone_string_%d", i + 1);
      g_bone_hashes[i] = bone_hash(name_buf);
   }

   // Verify first hash matches the game's table to confirm algorithm is correct
   uint32_t game_first_hash = *(uint32_t*)original_table_addr;
   if (g_bone_hashes[0] != game_first_hash) {
      log.printf("Tentacle: hash mismatch: computed %08x, game has %08x (at %08x)\n",
                 g_bone_hashes[0], game_first_hash, original_table_addr);
      return false;
   }

   return true;
}

// ============================================================================
// Code cave page — executable memory for trampolines
// ============================================================================

static uint8_t* g_cave_page = nullptr;
static size_t g_cave_offset = 0;

static uint8_t* alloc_cave(size_t size)
{
   if (!g_cave_page) {
      g_cave_page = (uint8_t*)VirtualAlloc(
         nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
      if (!g_cave_page) return nullptr;
   }

   uint8_t* ptr = g_cave_page + g_cave_offset;
   g_cave_offset += size;
   return ptr;
}

// Write a JMP rel32 at `from`, targeting `to`, padding with NOP to fill `overwrite_size`
static void write_jmp(uintptr_t from, uintptr_t to, size_t overwrite_size)
{
   uint8_t* p = (uint8_t*)from;
   p[0] = 0xE9;
   *(int32_t*)(p + 1) = (int32_t)(to - (from + 5));
   for (size_t i = 5; i < overwrite_size; i++)
      p[i] = 0x90;
}

// ============================================================================
// Struct offset scanning — safe displacement-only patching
//
// In x86, struct field accesses via [reg + disp32] encode the displacement
// after a ModRM byte with mod=10 (bits 7:6). We verify this context to avoid
// false-patching stack offsets (which use ESP/EBP-based addressing).
// ============================================================================

static bool is_struct_displacement(uintptr_t disp_addr)
{
   uint8_t modrm = *(uint8_t*)(disp_addr - 1);
   uint8_t mod = (modrm >> 6) & 3;
   uint8_t rm = modrm & 7;

   // mod=10 means 32-bit displacement follows
   // r/m != 4 (ESP/SIB) and r/m != 5 (EBP) rules out stack/frame accesses
   return (mod == 2) && (rm != 4) && (rm != 5);
}

static int scan_and_patch(uintptr_t func_start, size_t func_size,
                          uint32_t old_val, uint32_t new_val, cfile& log)
{
   int count = 0;
   uintptr_t end = func_start + func_size - 3;

   for (uintptr_t addr = func_start; addr <= end; addr++) {
      if (*(uint32_t*)addr == old_val && is_struct_displacement(addr)) {
         *(uint32_t*)addr = new_val;
         count++;
      }
   }

   return count;
}

// ============================================================================
// Struct offset tables — frozen-prefix architecture
// ============================================================================

// Struct field offset mappings: old offset → new offset
// With the frozen-prefix architecture, scalar fields stay at original offsets
// in the frozen prefix (0x000-0x267). Only oldPos needs redirecting relative
// to the new array base (this+0x268): oldPos moves from 0x120 to 0x288.
struct offset_mapping {
   uint32_t old_val;
   uint32_t new_val;
};

static const offset_mapping OLDPOS_REDIRECT[] = {
   {0x00000120, 0x00000288}, // oldPos array start (relative to array base)
};

// Negative offsets used in UpdatePositions and UpdatePose to access
// tPos entries relative to an oldPos pointer. The distance between
// oldPos and tPos changes by 0x168 (new oldPos start - old oldPos start).
static const offset_mapping NEGATIVE_OFFSETS[] = {
   {0xFFFFFEDC, 0xFFFFFD74}, // -(0x124) → -(0x28C)
   {0xFFFFFEE0, 0xFFFFFD78}, // -(0x120) → -(0x288)
   {0xFFFFFEE4, 0xFFFFFD7C}, // -(0x11C) → -(0x284)
};

// Interior oldPos offsets that appear as hardcoded addresses in UpdatePositions.
// These are specific points within the oldPos array, NOT field boundaries.
// With frozen-prefix: Loop 1 base is NOT redirected (LEA is already disp32,
// patched in-place). The offset is absolute from `this`:
//   old: this + 0x130 = oldPos + 0x10
//   new: this + 0x500 = oldPos_ext + 0x10 (= 0x4F0 + 0x10)
static const offset_mapping OLDPOS_INTERIOR_OFFSETS[] = {
   {0x00000130, 0x00000500}, // oldPos + 0x10 (UpdatePositions Loop 1 base)
};

// Emit CALL rel32 to a target address. Returns bytes written (always 5).
static size_t emit_call(uint8_t* buf, uintptr_t target)
{
   buf[0] = 0xE8;
   *(int32_t*)(buf + 1) = (int32_t)(target - (uintptr_t)(buf + 5));
   return 5;
}

// ============================================================================
// Per-build address tables
// ============================================================================

// Modtools build addresses (file offsets, add exe_base for runtime address)
struct build_addrs {
   // Executable identification
   uintptr_t id_file_offset;
   uint64_t  id_expected;

   // TentacleSimulator function file offsets and sizes
   uintptr_t fn_constructor;      size_t sz_constructor;
   uintptr_t fn_updateTimer;      size_t sz_updateTimer;
   uintptr_t fn_enforceSphere;    size_t sz_enforceSphere;
   uintptr_t fn_enforceBox;       size_t sz_enforceBox;
   uintptr_t fn_enforceCylinder;  size_t sz_enforceCylinder;
   uintptr_t fn_enforceCollisions; size_t sz_enforceCollisions;
   uintptr_t fn_updatePose;       size_t sz_updatePose;
   uintptr_t fn_updatePositions;  size_t sz_updatePositions;
   uintptr_t fn_doTentacles;      size_t sz_doTentacles;

   // Bone hash table VA in the original binary
   uintptr_t bone_hash_table_va;

   // Bone hash table pointer references (file offsets of the 4-byte VA values)
   uintptr_t hash_ref[3];

   // Stack frame SUB ESP imm32 value addresses (file offsets)
   // These are EBP-framed functions where only the SUB ESP value needs patching
   uintptr_t updatePose_sub_esp;        uint32_t updatePose_old_esp;       uint32_t updatePose_new_esp;
   uintptr_t updatePositions_sub_esp;   uint32_t updatePositions_old_esp;  uint32_t updatePositions_new_esp;
   uintptr_t enforceCollisions_sub_esp; uint32_t enforceCollisions_old_esp; uint32_t enforceCollisions_new_esp;

   // DoTentacles local bone pointer array redirect
   // 4 LEA instructions that load ESP+0x20 (array base) into registers
   // Each entry: {file_offset_of_lea, overwrite_size, target_register_encoding}
   struct lea_redirect {
      uintptr_t file_offset;     // start of LEA instruction
      size_t overwrite_size;     // LEA + following instruction bytes to overwrite with JMP
      uint8_t mov_reg_opcode;    // B8=EAX, B9=ECX, BA=EDX, BB=EBX, BD=EBP
      uint8_t restore_bytes[8];  // bytes of the instruction(s) after LEA that get overwritten
      size_t restore_size;       // how many restore_bytes to emit in cave
      uintptr_t return_offset;   // file offset to JMP back to
      uint32_t addr_delta;       // offset from g_bone_hashes base (0 for ESP+0x20, 4 for ESP+0x24)
   };
   lea_redirect doTentacles_leas[4];

   // SetProperty tentacle limit
   uintptr_t setProperty_limit; // file offset of the byte to patch (4 → 9)

   // sMemoryPool element size (PUSH 0x268 → 0x778)
   uintptr_t memPool1; // file offset (Allocate call site 1)
   uintptr_t memPool2; // file offset (pool init)
   uintptr_t memPool3; // file offset (Allocate call site 2, 0 if none)

   // Render stack sizes
   uintptr_t renderSoldier;   uint32_t renderSoldier_old;   uint32_t renderSoldier_new;
   uintptr_t renderAddon1;    uint32_t renderAddon1_old;    uint32_t renderAddon1_new;
   uintptr_t renderAddon2;    uint32_t renderAddon2_old;    uint32_t renderAddon2_new;

   // Bitfield widening: expand numTentacles from 3 bits (max 7) to 4 bits (max 15)
   // Shifts bonesPerTentacle (bits 10-13 → 11-14) and collisionType (bits 14-15 → 15-16)
   struct bf_patch {
      uintptr_t file_offset;
      uint8_t size;       // 1 or 4 bytes
      uint32_t old_val;
      uint32_t new_val;
   };
   static const int NUM_BF_PATCHES = 17;
   bf_patch bitfield[NUM_BF_PATCHES];

   // UpdatePositions: oldPos accesses via tPos-relative pointer (EAX = ESI+0x10)
   // These use displacements 0x110/0x114/0x118 which resolve to oldPos entries.
   // Can't go in the global OLDPOS_INTERIOR_OFFSETS table because 0x110-0x118
   // overlap with valid tPos absolute offsets (tPos[22..23]) in other functions.
   // File offsets point to the 4-byte displacement within each instruction.
   static const int NUM_TPOS_REL_PATCHES = 6;
   struct disp_patch {
      uintptr_t file_offset;  // file offset of the 4-byte displacement
      uint32_t old_val;
      uint32_t new_val;
   };
   disp_patch updatePositions_tpos_rel[NUM_TPOS_REL_PATCHES];

   // Per-function array base redirects — frozen-prefix architecture.
   // Each redirect overwrites the instruction(s) that set up the array iteration
   // register, replacing them with a JMP to a code cave that does:
   //   LEA dest, [src + new_disp]  (6 bytes)
   //   <restore_bytes>             (variable)
   //   JMP back                    (5 bytes)
   // This makes array access use this+0x268 (extension area) instead of this+0.
   static const int NUM_ARRAY_REDIRECTS = 9;
   struct array_base_redirect {
      uintptr_t file_offset;     // file offset of instruction(s) to overwrite
      size_t    overwrite_size;  // bytes to overwrite (>= 5 for JMP)
      uint8_t   lea_modrm;      // ModRM byte for LEA dest,[src+disp32]
      uint32_t  new_disp;        // displacement (0x268 + original_offset)
      uint8_t   restore_bytes[12]; // instruction bytes to emit after LEA in cave
      size_t    restore_size;     // how many restore_bytes to emit
      uintptr_t return_offset;    // file offset to JMP back to
      uintptr_t loopback_rel32;   // file offset of Jcc rel32 that loops back into our overwrite (0 = none)
   };
   array_base_redirect array_redirects[NUM_ARRAY_REDIRECTS];

   // Constructor pre-zero: zeroes mNumTentacles/mBonesPerTentacle before the
   // constructor's zeroing loop reads them. Without this, uninitialized pool
   // memory can cause the zeroing loop to run billions of iterations,
   // corrupting the heap.
   struct ctor_prezero {
      uintptr_t overwrite_offset; // file offset of instruction to overwrite with JMP
      size_t    overwrite_size;   // bytes to overwrite (>= 5)
      uintptr_t return_offset;    // file offset to JMP back to after cave
      uint8_t   cave_bytes[32];   // replay + zero 0x258 + zero 0x25C
      size_t    cave_size;        // bytes in cave_bytes (JMP appended by patching code)
   };
   ctor_prezero ctor_fix;

   // DoTentacles mFirstUpdate init: oldPos displacement patches (Steam/GOG only).
   // The mFirstUpdate block uses pre-increment + smaller displacements (0x114, 0x11C)
   // rather than the 0x120 that scan_and_patch catches. These must be patched manually
   // after the array base redirect changes the iteration base from this to this+0x268.
   static const int NUM_DOTENTACLES_OLDPOS_PATCHES = 2;
   disp_patch doTentacles_oldpos_disp[NUM_DOTENTACLES_OLDPOS_PATCHES];
};

// clang-format off
static const build_addrs MODTOOLS = {
   .id_file_offset = 0x62b59c,
   .id_expected    = 0x746163696c707041,

   // Function file offsets and sizes (VA - 0x400000)
   .fn_constructor      = 0x16d090, .sz_constructor      = 0x56d177 - 0x56d090 + 3,  // through RET
   .fn_updateTimer      = 0x16d1c0, .sz_updateTimer      = 0x56d25c - 0x56d1c0 + 3,
   .fn_enforceSphere    = 0x16d290, .sz_enforceSphere    = 0x56d3d9 - 0x56d290 + 3,
   .fn_enforceBox       = 0x16d430, .sz_enforceBox       = 0x56d7b1 - 0x56d430 + 3,
   .fn_enforceCylinder  = 0x16d8a0, .sz_enforceCylinder  = 0x56dbab - 0x56d8a0 + 3,
   .fn_enforceCollisions = 0x16f020, .sz_enforceCollisions = 0x56f3df - 0x56f020 + 1,
   .fn_updatePose       = 0x16dc80, .sz_updatePose       = 0x900, // generous upper bound
   .fn_updatePositions  = 0x16e420, .sz_updatePositions  = 0x56edac - 0x56e420 + 3, // through RET
   .fn_doTentacles      = 0x16f4e0, .sz_doTentacles      = 0x56f7ab - 0x56f4e0 + 3,

   .bone_hash_table_va = 0xa442f0,

   // Bone hash table pointer references in code (file offsets of the disp32 operand)
   // DoTentacles: MOV EAX,[ECX*4 + 0xa442f0] at VA 0x56f63b → disp at 0x56f63e
   // UpdatePose: MOV EAX,[EAX*4 + 0xa442f0] at VA 0x56e1d4 → disp at 0x56e1d7
   // UpdatePose: MOV EAX,[EDX*4 + 0xa442f0] at VA 0x56e207 → disp at 0x56e20a
   .hash_ref = {
      0x16f63e, // DoTentacles
      0x16e1d7, // UpdatePose (1st)
      0x16e20a, // UpdatePose (2nd)
   },

   // Stack frame patches (imm32 values inside SUB ESP instructions)
   // UpdatePose: SUB ESP,0x284 at VA 0x56dc86 → imm32 at 0x56dc88
   .updatePose_sub_esp        = 0x16dc88, .updatePose_old_esp       = 0x284, .updatePose_new_esp       = 0x78c,
   // UpdatePositions: SUB ESP,0x214 at VA 0x56e426 → imm32 at 0x56e428
   .updatePositions_sub_esp   = 0x16e428, .updatePositions_old_esp  = 0x214, .updatePositions_new_esp  = 0x63c,
   // EnforceCollisions: SUB ESP,0x104 at VA 0x56f026 → imm32 at 0x56f028
   .enforceCollisions_sub_esp = 0x16f028, .enforceCollisions_old_esp = 0x104, .enforceCollisions_new_esp = 0x30c,

   // DoTentacles LEA redirects — redirect local bone_ptrs array to static DLL buffer
   // Original: LEA reg,[ESP+0x20] (4 bytes) — redirect to MOV reg, &g_bone_hashes_ptrs
   .doTentacles_leas = {
      // LEA EBP,[ESP+0x20] at VA 0x56f614 (4B) + MOV [ESP+0x10],EBP at 0x56f618 (4B) = 8 bytes
      {0x16f614, 8, 0xBD, {0x89, 0x6C, 0x24, 0x10}, 4, 0x16f61c, 0},
      // LEA EDX,[ESP+0x20] at VA 0x56f6a7 (4B) + MOV ECX,ESI at 0x56f6ab (2B) = 6 bytes
      {0x16f6a7, 6, 0xBA, {0x8B, 0xCE}, 2, 0x16f6ad, 0},
      // LEA ECX,[ESP+0x20] at VA 0x56f775 (4B) + PUSH ECX at 0x56f779 (1B) = 5 bytes
      {0x16f775, 5, 0xB9, {0x51}, 1, 0x16f77a, 0},
      // LEA EDX,[ESP+0x24] at VA 0x56f796 (4B) + PUSH EDX at 0x56f79a (1B) = 5 bytes
      // NOTE: ESP+0x24 not ESP+0x20 because a PUSH preceded this LEA; same array base
      {0x16f796, 5, 0xBA, {0x52}, 1, 0x16f79b, 0},
   },

   // EntitySoldierClass::SetProperty tentacle limit: CMP EAX,4 → CMP EAX,9
   // CMP EAX,imm8 (83 F8 04) at VA 0x541cd3, imm byte at 0x541cd5
   .setProperty_limit = 0x141cd5,

   // sMemoryPool element size: 0x268 → 0x778 (PUSH imm32, patching the imm32 at opcode+1)
   // Site 1: PUSH 0x268 at VA 0x6745cc in DisplaySoldier::Setup (Allocate call)
   // Site 2: PUSH 0x268 at VA 0xa17180 in pool init
   // Site 3: PUSH 0x268 at VA 0x5347c9 in EntitySoldier ctor (Allocate call)
   .memPool1 = 0x2745cd,
   .memPool2 = 0x617181,
   .memPool3 = 0x1347ca,

   // Render stack sizes (SUB ESP,imm32 — patching the imm32 at opcode+2)
   // EntitySoldierClass::Render: SUB ESP at VA 0x535d96, imm32 at 0x535d98
   .renderSoldier = 0x135d98, .renderSoldier_old = 0xb84, .renderSoldier_new = 0x228c,
   // AnimatedAddon::Render: SUB ESP at VA 0x56fe86, imm32 at 0x56fe88
   .renderAddon1  = 0x16fe88, .renderAddon1_old  = 0xa24, .renderAddon1_new  = 0x1e6c,
   // AnimatedAddon::Render 2nd site: SUB ESP at VA 0x674896, imm32 at 0x674898
   .renderAddon2  = 0x274898, .renderAddon2_old  = 0xa34, .renderAddon2_new  = 0x1e9c,

   // Bitfield widening patches — numTentacles: 3-bit → 4-bit (max 7 → 15)
   // New layout: numTentacles bits 7-10 (0x780), bonesPerTentacle bits 11-14 (0x7800), collisionType bits 15-16 (0x18000)
   .bitfield = {
      // --- EntitySoldierClass default ctor (VA 0x53DF90) ---
      // AND EDX, 0xFFFF0056 → AND EDX, 0xFFFE0056 (clear bit 16 too for widened collisionType)
      {0x13E0FA, 4, 0xFFFF0056, 0xFFFE0056},

      // --- EntitySoldierClass copy ctor (VA 0x53EE20) ---
      // numTentacles copy mask: AND ECX, 0x380 → 0x780
      {0x13F136, 4, 0x00000380, 0x00000780},
      // bonesPerTentacle copy mask: AND EAX, 0x3C00 → 0x7800
      {0x13F120, 4, 0x00003C00, 0x00007800},
      // collisionType copy mask: AND EAX, 0xC000 → 0x18000
      {0x13F14B, 4, 0x0000C000, 0x00018000},

      // --- SetProperty numTentacles write (VA 0x541D12) ---
      // AND EAX, 0x380 → 0x780
      {0x141D13, 4, 0x00000380, 0x00000780},

      // --- SetProperty bonesPerTentacle write (VA 0x53FC26) ---
      // SHL EAX, 0xA → 0xB
      {0x13FC2E, 1, 0x0A, 0x0B},
      // AND EAX, 0x3C00 → 0x7800
      {0x13FC35, 4, 0x00003C00, 0x00007800},

      // --- SetProperty collisionType write (VA 0x5420CA) ---
      // SHL EAX, 0xE → 0xF
      {0x1420D9, 1, 0x0E, 0x0F},
      // AND EAX, 0xC000 → 0x18000
      {0x1420E0, 4, 0x0000C000, 0x00018000},

      // --- EntitySoldier ctor (VA 0x5339D0) — bitfield extraction ---
      // TEST [EAX+0x8BC], 0x380 → 0x780 (numTentacles zero-check)
      {0x1347AD, 4, 0x00000380, 0x00000780},
      // SHR ECX, 0xE → 0xF (collisionType)
      {0x1347EC, 1, 0x0E, 0x0F},
      // SHR ECX, 0xA → 0xB (bonesPerTentacle)
      {0x1347F5, 1, 0x0A, 0x0B},
      // AND EDX, 0x7 → 0xF (numTentacles mask)
      {0x1347FF, 1, 0x07, 0x0F},

      // --- DisplaySoldier::Setup (VA 0x674580) — bitfield extraction ---
      // TEST [EAX+0x8BC], 0x380 → 0x780 (numTentacles zero-check)
      {0x2745A4, 4, 0x00000380, 0x00000780},
      // SHR ECX, 0xE → 0xF (collisionType)
      {0x2745EF, 1, 0x0E, 0x0F},
      // SHR ECX, 0xA → 0xB (bonesPerTentacle)
      {0x2745F8, 1, 0x0A, 0x0B},
      // AND EDX, 0x7 → 0xF (numTentacles mask)
      {0x274602, 1, 0x07, 0x0F},
   },

   // UpdatePositions verlet loop: oldPos via tPos-relative pointer (EAX = ESI+0x10)
   // Old: [EAX+0x110/0x114/0x118] = [ESI+0x120/0x124/0x128] (oldPos at 0x120)
   // New: [EAX+0x278/0x27C/0x280] = [ESI+0x288/0x28C/0x290] (oldPos at 0x288)
   // File offsets point to the disp32 operand (instruction start + 2)
   .updatePositions_tpos_rel = {
      {0x16eab0, 0x110, 0x278}, // FSUB [EAX+0x110] at VA 0x56eaae
      {0x16eac5, 0x114, 0x27C}, // FSUB [EAX+0x114] at VA 0x56eac3
      {0x16eada, 0x118, 0x280}, // FSUB [EAX+0x118] at VA 0x56ead8
      {0x16eb5d, 0x110, 0x278}, // MOV [EAX+0x110],ECX at VA 0x56eb5b
      {0x16eb6a, 0x114, 0x27C}, // MOV [EAX+0x114],EDI at VA 0x56eb68
      {0x16eb77, 0x118, 0x280}, // MOV [EAX+0x118],ECX at VA 0x56eb75
   },

   // Array base redirects (frozen-prefix: redirect array iteration to this+0x268)
   // [0]=Constructor, [1]=UpdatePose, [2]=EnforceSphere,
   // [3]=EnforceBox, [4]=EnforceCylinder, [5]=UpdatePositions Loop2
   .array_redirects = {
      // [0] Constructor: MOV [ESP+0xC],EAX(4); PUSH EDI(1) → LEA EDX,[EAX+0x268]; store+push
      //     Cave stores redirected base (EDX) to stack slot instead of raw this (EAX).
      //     EDX is scratch (overwritten at 0x56D0C9 before first use).
      {0x16D0BC, 5, 0x90, 0x268, {0x89,0x54,0x24,0x0C,0x57}, 5, 0x16D0C1},
      // [1] UpdatePose: LEA EDX,[ECX+0xC](3); MOV [ESP+0x40],EAX(4) → LEA EDX,[ECX+0x274]
      {0x16DCB7, 7, 0x91, 0x274, {0x89,0x44,0x24,0x40}, 4, 0x16DCBE},
      // [2] EnforceSphere: LEA EDX,[ECX+0xC](3); PUSH EDI(1); MOV [ESP+0x10],EDX(4)
      {0x16D2AA, 8, 0x91, 0x274, {0x57,0x89,0x54,0x24,0x10}, 5, 0x16D2B2},
      // [3] EnforceBox: LEA EBP,[ECX+0x14](3); MOV EAX,[ECX+0x25C](6)
      //     Loop-back JL at VA 0x56D7A1 targets 0x56D454 (MOV, inside overwrite) — fixup needed
      {0x16D451, 9, 0xA9, 0x27C, {0x8B,0x81,0x5C,0x02,0x00,0x00}, 6, 0x16D45A, 0x16D7A3},
      // [4] EnforceCylinder: LEA EBP,[ECX+0x14](3); MOV EAX,[ECX+0x25C](6)
      //     Loop-back JL at VA 0x56DB9B targets 0x56D8C4 (MOV, inside overwrite) — fixup needed
      {0x16D8C1, 9, 0xA9, 0x27C, {0x8B,0x81,0x5C,0x02,0x00,0x00}, 6, 0x16D8CA, 0x16DB9D},
      // [5] UpdatePositions Loop2: LEA EAX,[ESI+0x10](3); MOV [ESP+0x34],EAX(4)
      {0x16EA16, 7, 0x86, 0x278, {0x89,0x44,0x24,0x34}, 4, 0x16EA1D},
      // [6] UpdatePositions Loop3 (root bone fixup): MOV EAX,[EBP+0x18](3); MOV ECX,ESI(2)
      //     ECX = this → LEA ECX,[ESI+0x268]. Loop-back target at 0x56EBE1 untouched.
      {0x16EBDC, 5, 0x8E, 0x268, {0x8B,0x45,0x18}, 3, 0x16EBE1},
      // [7] UpdatePositions Loop4 (distance constraint): LEA EAX,[ESI+0x14](3); MOV [ESP+0x38],EAX(4)
      //     EAX = this+0x14 → LEA EAX,[ESI+0x27C]
      {0x16EC25, 7, 0x86, 0x27C, {0x89,0x44,0x24,0x38}, 4, 0x16EC2C},
      // [8] DoTentacles mFirstUpdate init (leas[1] returns to VA 0x56F6AD):
      //     MOV [ESP+0x1C],EDX(4); MOV [ESP+0x18],ECX(4) = 8 bytes.
      //     Insert LEA ECX,[ESI+0x268] before replaying both saves.
      //     ECX (from leas[1] cave) is ESI=this; after LEA it becomes this+0x268.
      //     MOV [ESP+0x18],ECX then saves the redirected value for outer loop reload.
      {0x16F6AD, 8, 0x8E, 0x268, {0x89,0x54,0x24,0x1C,0x89,0x4C,0x24,0x18}, 8, 0x16F6B5},
   },

   // Constructor pre-zero: overwrite MOV [EAX+0x240],ECX (6B) at VA 0x56D0A3.
   // ECX is already 0 (from XOR ECX,ECX at 0x56D095).
   // Cave: replay MOV + zero mNumTentacles + zero mBonesPerTentacle, return to CMP.
   .ctor_fix = {
      0x16D0A3, 6, 0x16D0A9,
      {
         0x89,0x88,0x40,0x02,0x00,0x00,  // MOV [EAX+0x240],ECX  (replay)
         0x89,0x88,0x58,0x02,0x00,0x00,  // MOV [EAX+0x258],ECX  (zero mNumTentacles)
         0x89,0x88,0x5C,0x02,0x00,0x00,  // MOV [EAX+0x25C],ECX  (zero mBonesPerTentacle)
      },
      18
   },

   // Not needed for modtools — scan_and_patch catches the 0x120 displacement in DoTentacles
   .doTentacles_oldpos_disp = {{0,0,0},{0,0,0}},
};

static const build_addrs STEAM = {
   .id_file_offset = 0x39f834,
   .id_expected    = 0x746163696c707041,

   // Function file offsets and sizes (VA - 0x400000)
   .fn_constructor      = 0x255770, .sz_constructor      = 0x655836 - 0x655770 + 3,
   .fn_updateTimer      = 0x255840, .sz_updateTimer      = 0x6558e5 - 0x655840 + 1,
   .fn_enforceSphere    = 0x256d00, .sz_enforceSphere    = 0x656e5e - 0x656d00 + 3,
   .fn_enforceBox       = 0x256e70, .sz_enforceBox       = 0x657135 - 0x656e70 + 3,
   .fn_enforceCylinder  = 0x257140, .sz_enforceCylinder  = 0x6573fa - 0x657140 + 3,
   .fn_enforceCollisions = 0x2569c0, .sz_enforceCollisions = 0x656cfd - 0x6569c0 + 3,
   .fn_updatePose       = 0x255b60, .sz_updatePose       = 0x656268 - 0x655b60 + 3,
   .fn_updatePositions  = 0x256270, .sz_updatePositions  = 0x6569b5 - 0x656270 + 3,
   .fn_doTentacles      = 0x2558f0, .sz_doTentacles      = 0x655b55 - 0x6558f0 + 3,

   .bone_hash_table_va = 0x78b630,

   // Bone hash table references: PUSH [EAX*4 + 0x78b630] — disp32 operand offsets
   .hash_ref = {
      0x255a3e, // DoTentacles
      0x2561be, // UpdatePose (1st)
      0x2561f3, // UpdatePose (2nd)
   },

   // Stack frame patches — all expanded to 3× original (same ratio as modtools)
   .updatePose_sub_esp        = 0x255b68, .updatePose_old_esp       = 0x1c8, .updatePose_new_esp       = 0x558,
   .updatePositions_sub_esp   = 0x256278, .updatePositions_old_esp  = 0x1c8, .updatePositions_new_esp  = 0x558,
   .enforceCollisions_sub_esp = 0x2569cb, .enforceCollisions_old_esp = 0xbc, .enforceCollisions_new_esp = 0x23c,

   // DoTentacles LEA redirects — EBP-relative LEA reg,[EBP-0x64] (3-byte encoding)
   .doTentacles_leas = {
      // LEA EAX,[EBP-0x64] (3B) + MOV [EBP-0xC],EAX (3B) = 6 bytes; EB 03 follows intact
      {0x255a15, 6, 0xB8, {0x89, 0x45, 0xF4}, 3, 0x255a1b, 0},
      // LEA ECX,[EBP-0x64] (3B) + MOV [EBP-0x8],EDI (3B) + MOV [EBP-0x10],ECX (3B) = 9 bytes
      {0x255a97, 9, 0xB9, {0x89, 0x7D, 0xF8, 0x89, 0x4D, 0xF0}, 6, 0x255aa0, 0},
      // LEA EAX,[EBP-0x64] (3B) + PUSH EAX (1B) + PUSH ESI (1B) = 5 bytes
      {0x255b1f, 5, 0xB8, {0x50, 0x56}, 2, 0x255b24, 0},
      // LEA EAX,[EBP-0x64] (3B) + PUSH EAX (1B) + PUSH ECX (1B) = 5 bytes
      {0x255b40, 5, 0xB8, {0x50, 0x51}, 2, 0x255b45, 0},
   },

   // No CMP EAX,4 limit in Steam build
   .setProperty_limit = 0,

   // sMemoryPool element size: 0x268 → 0x778
   .memPool1 = 0x0df909, // EntitySoldier ctor (PUSH 0x268)
   .memPool2 = 0x0069d1, // Static pool init (PUSH 0x268)
   .memPool3 = 0x08da5e, // SoldierElement::vfunction17 (PUSH 0x268)

   // Render stack sizes (3× expansion)
   .renderSoldier = 0x0e23ed, .renderSoldier_old = 0xbac, .renderSoldier_new = 0x230c,
   .renderAddon1  = 0x043ca8, .renderAddon1_old  = 0xa18, .renderAddon1_new  = 0x1e48,
   .renderAddon2  = 0x08dcad, .renderAddon2_old  = 0xa4c, .renderAddon2_new  = 0x1eec,

   // Bitfield widening patches
   .bitfield = {
      // --- EntitySoldierClass default ctor (VA 0x4F5000) ---
      {0x0f51a5, 4, 0xFFFF0056, 0xFFFE0056},
      // --- EntitySoldierClass copy ctor (VA 0x4F5CC0) ---
      {0x0f6004, 4, 0x00000380, 0x00000780},
      {0x0f5fef, 4, 0x00003C00, 0x00007800},
      {0x0f601a, 4, 0x0000C000, 0x00018000},
      // --- SetProperty numTentacles write ---
      {0x0fa2ee, 4, 0x00000380, 0x00000780},
      // --- SetProperty bonesPerTentacle write ---
      {0x0f84c3, 1, 0x0A, 0x0B},
      {0x0f84cb, 4, 0x00003C00, 0x00007800},
      // --- SetProperty collisionType write ---
      {0x0fa613, 1, 0x0E, 0x0F},
      {0x0fa61b, 4, 0x0000C000, 0x00018000},
      // --- EntitySoldier ctor (VA 0x4DE850) — bitfield extraction ---
      {0x0df8ed, 4, 0x00000380, 0x00000780},
      {0x0df92f, 1, 0x0E, 0x0F},
      {0x0df935, 1, 0x0A, 0x0B},
      {0x0df940, 1, 0x07, 0x0F},
      // --- SoldierElement::vfunction17 (VA 0x48DA10) — bitfield extraction ---
      {0x08da35, 4, 0x00000380, 0x00000780},
      {0x08da84, 1, 0x0E, 0x0F},
      {0x08da8a, 1, 0x0A, 0x0B},
      {0x08da95, 1, 0x07, 0x0F},
   },

   // UpdatePositions: 4 hardcoded patches (0x120 disps handled by scan_and_patch)
   .updatePositions_tpos_rel = {
      {0x2566fc, 0x11C, 0x284},
      {0x25674c, 0x124, 0x28C},
      {0x25677e, 0x11C, 0x284},
      {0x256784, 0x124, 0x28C},
      {0, 0, 0},
      {0, 0, 0},
   },

   // Array base redirects (frozen-prefix: redirect array iteration to this+0x268)
   .array_redirects = {
      // [0] Constructor: MOV EBX,ECX(2); XOR EDX,EDX(2); CMP [ECX+0x25C],EDX(6) → LEA EBX,[ECX+0x268]
      {0x2557A1, 10, 0x99, 0x268, {0x33,0xD2,0x39,0x91,0x5C,0x02,0x00,0x00}, 8, 0x2557AB},
      // [1] UpdatePose: LEA EDI,[EAX+0xC](3); MOV [ESP+0x3C],EDI(4) → LEA EDI,[EAX+0x274]
      {0x255B93, 7, 0xB8, 0x274, {0x89,0x7C,0x24,0x3C}, 4, 0x255B9A},
      // [2] EnforceSphere: LEA ESI,[ECX+0xC](3); MOV [ESP+0x14],ESI(4) → LEA ESI,[ECX+0x274]
      {0x256D2E, 7, 0xB1, 0x274, {0x89,0x74,0x24,0x14}, 4, 0x256D35},
      // [3] EnforceBox: LEA EAX,[EDI+0x14](3); MOV [ESP+0xC],EAX(4) → LEA EAX,[EDI+0x27C]
      {0x256E9E, 7, 0x87, 0x27C, {0x89,0x44,0x24,0x0C}, 4, 0x256EA5},
      // [4] EnforceCylinder: LEA ESI,[EAX+0x14](3); MOV [ESP+0x20],ESI(4) → LEA ESI,[EAX+0x27C]
      {0x257175, 7, 0xB0, 0x27C, {0x89,0x74,0x24,0x20}, 4, 0x25717C},
      // [5] UpdatePositions Loop2: LEA EAX,[EDI+0x10](3); MOV [ESP+0x1C],EDX(4) → LEA EAX,[EDI+0x278]
      {0x2566A8, 7, 0x87, 0x278, {0x89,0x54,0x24,0x1C}, 4, 0x2566AF},
      // [6] UpdatePositions Loop3 (root bone fixup): MOV ESI,[EBP+0x14](3); MOV ECX,EDI(2); MOV EDI,EDI(2)
      //     ECX = this → LEA ECX,[EDI+0x268]. Return to loop-back target at 0x6567D0.
      {0x2567C9, 7, 0x8F, 0x268, {0x8B,0x75,0x14}, 3, 0x2567D0},
      // [7] UpdatePositions Loop4 (distance constraint): XOR EAX,EAX(2); LEA EDX,[EDI+0x14](3); MOV [ESP+0x14],EAX(4)
      //     EDX = this+0x14 → LEA EDX,[EDI+0x27C]
      {0x256803, 9, 0x97, 0x27C, {0x33,0xC0,0x89,0x44,0x24,0x14}, 6, 0x25680C},
      // [8] DoTentacles mFirstUpdate init (leas[1] returns to VA 0x655AA0):
      //     MOV EDX,EDI(2); XOR ESI,ESI(2); CMP [EDI+0x25C],ESI(6) = 10 bytes.
      //     Redirect EDX = LEA EDX,[EDI+0x268], then overwrite [EBP-0x8] (saved by leas[1]
      //     with un-redirected EDI) with the redirected EDX for outer loop reload.
      //     Restore: MOV [EBP-8],EDX(3) + XOR ESI,ESI(2) + CMP [EDI+0x25C],ESI(6) = 11 bytes.
      {0x255AA0, 10, 0x97, 0x268,
       {0x89,0x55,0xF8, 0x33,0xF6, 0x39,0xB7,0x5C,0x02,0x00,0x00}, 11, 0x255AAA},
   },

   // Constructor pre-zero: overwrite MOV [ECX+0x240],0 (10B) at VA 0x65578A.
   // No zero register available at this point, so uses MOV [ECX+disp32],imm32 form.
   // Cave: replay MOV + zero mNumTentacles + zero mBonesPerTentacle, return to PUSH EDI.
   .ctor_fix = {
      0x25578A, 10, 0x255794,
      {
         0xC7,0x81,0x40,0x02,0x00,0x00,0x00,0x00,0x00,0x00,  // MOV [ECX+0x240],0  (replay)
         0xC7,0x81,0x58,0x02,0x00,0x00,0x00,0x00,0x00,0x00,  // MOV [ECX+0x258],0  (zero mNumTentacles)
         0xC7,0x81,0x5C,0x02,0x00,0x00,0x00,0x00,0x00,0x00,  // MOV [ECX+0x25C],0  (zero mBonesPerTentacle)
      },
      30
   },

   // DoTentacles mFirstUpdate: oldPos displacements 0x114/0x11C (pre-increment coding style).
   // scan_and_patch only catches 0x120; these must be patched manually.
   // After redirect (EDX = this+0x268, pre-inc +0xC), [EDX+0x114] must become [EDX+0x27C].
   .doTentacles_oldpos_disp = {
      {0x255AC2, 0x114, 0x27C}, // MOVQ [EDX+0x114] → [EDX+0x27C] (oldPos.x,.y)
      {0x255ACB, 0x11C, 0x284}, // MOV [EDX+0x11C] → [EDX+0x284] (oldPos.z)
   },
};

static const build_addrs GOG = {
   .id_file_offset = 0x3a0698,
   .id_expected    = 0x746163696c707041,

   // All TentacleSimulator functions at Steam VA + 0x10A0
   .fn_constructor      = 0x256810, .sz_constructor      = 0x6568d6 - 0x656810 + 3,
   .fn_updateTimer      = 0x2568e0, .sz_updateTimer      = 0x656985 - 0x6568e0 + 1,
   .fn_enforceSphere    = 0x257da0, .sz_enforceSphere    = 0x657efe - 0x657da0 + 3,
   .fn_enforceBox       = 0x257f10, .sz_enforceBox       = 0x6581d5 - 0x657f10 + 3,
   .fn_enforceCylinder  = 0x2581e0, .sz_enforceCylinder  = 0x65849a - 0x6581e0 + 3,
   .fn_enforceCollisions = 0x257a60, .sz_enforceCollisions = 0x657d9d - 0x657a60 + 3,
   .fn_updatePose       = 0x256c00, .sz_updatePose       = 0x657308 - 0x656c00 + 3,
   .fn_updatePositions  = 0x257310, .sz_updatePositions  = 0x657a55 - 0x657310 + 3,
   .fn_doTentacles      = 0x256990, .sz_doTentacles      = 0x656bf5 - 0x656990 + 3,

   .bone_hash_table_va = 0x78c5d0,

   .hash_ref = {
      0x256ade, // DoTentacles
      0x25725e, // UpdatePose (1st)
      0x257293, // UpdatePose (2nd)
   },

   // Stack frames (same old values as Steam, same 3× expansion)
   .updatePose_sub_esp        = 0x256c08, .updatePose_old_esp       = 0x1c8, .updatePose_new_esp       = 0x558,
   .updatePositions_sub_esp   = 0x257318, .updatePositions_old_esp  = 0x1c8, .updatePositions_new_esp  = 0x558,
   .enforceCollisions_sub_esp = 0x257a6b, .enforceCollisions_old_esp = 0xbc, .enforceCollisions_new_esp = 0x23c,

   // DoTentacles LEA redirects — byte-identical to Steam, shifted +0x10A0
   .doTentacles_leas = {
      {0x256ab5, 6, 0xB8, {0x89, 0x45, 0xF4}, 3, 0x256abb, 0},
      {0x256b37, 9, 0xB9, {0x89, 0x7D, 0xF8, 0x89, 0x4D, 0xF0}, 6, 0x256b40, 0},
      {0x256bbf, 5, 0xB8, {0x50, 0x56}, 2, 0x256bc4, 0},
      {0x256be0, 5, 0xB8, {0x50, 0x51}, 2, 0x256be5, 0},
   },

   .setProperty_limit = 0,

   // MemPool sites same as Steam (peripheral patches share file offsets)
   .memPool1 = 0x0df909,
   .memPool2 = 0x0069d1,
   .memPool3 = 0x08da5e,

   // Render stacks (renderAddon1 at different VA: GOG 0x443C80 vs Steam 0x443CA0)
   .renderSoldier = 0x0e23ed, .renderSoldier_old = 0xbac, .renderSoldier_new = 0x230c,
   .renderAddon1  = 0x043c88, .renderAddon1_old  = 0xa18, .renderAddon1_new  = 0x1e48,
   .renderAddon2  = 0x08dcad, .renderAddon2_old  = 0xa4c, .renderAddon2_new  = 0x1eec,

   // Bitfield patches — identical file offsets to Steam
   .bitfield = {
      {0x0f51a5, 4, 0xFFFF0056, 0xFFFE0056},
      {0x0f6004, 4, 0x00000380, 0x00000780},
      {0x0f5fef, 4, 0x00003C00, 0x00007800},
      {0x0f601a, 4, 0x0000C000, 0x00018000},
      {0x0fa2ee, 4, 0x00000380, 0x00000780},
      {0x0f84c3, 1, 0x0A, 0x0B},
      {0x0f84cb, 4, 0x00003C00, 0x00007800},
      {0x0fa613, 1, 0x0E, 0x0F},
      {0x0fa61b, 4, 0x0000C000, 0x00018000},
      {0x0df8ed, 4, 0x00000380, 0x00000780},
      {0x0df92f, 1, 0x0E, 0x0F},
      {0x0df935, 1, 0x0A, 0x0B},
      {0x0df940, 1, 0x07, 0x0F},
      {0x08da35, 4, 0x00000380, 0x00000780},
      {0x08da84, 1, 0x0E, 0x0F},
      {0x08da8a, 1, 0x0A, 0x0B},
      {0x08da95, 1, 0x07, 0x0F},
   },

   // tPos-relative: same offsets within function, shifted +0x10A0 from Steam
   .updatePositions_tpos_rel = {
      {0x25779c, 0x11C, 0x284},
      {0x2577ec, 0x124, 0x28C},
      {0x25781e, 0x11C, 0x284},
      {0x257824, 0x124, 0x28C},
      {0, 0, 0},
      {0, 0, 0},
   },

   // Array base redirects (Steam + 0x10A0, byte-identical code)
   .array_redirects = {
      // [0] Constructor
      {0x256841, 10, 0x99, 0x268, {0x33,0xD2,0x39,0x91,0x5C,0x02,0x00,0x00}, 8, 0x25684B},
      // [1] UpdatePose
      {0x256C33, 7, 0xB8, 0x274, {0x89,0x7C,0x24,0x3C}, 4, 0x256C3A},
      // [2] EnforceSphere
      {0x257DCE, 7, 0xB1, 0x274, {0x89,0x74,0x24,0x14}, 4, 0x257DD5},
      // [3] EnforceBox
      {0x257F3E, 7, 0x87, 0x27C, {0x89,0x44,0x24,0x0C}, 4, 0x257F45},
      // [4] EnforceCylinder
      {0x258215, 7, 0xB0, 0x27C, {0x89,0x74,0x24,0x20}, 4, 0x25821C},
      // [5] UpdatePositions Loop2
      {0x257748, 7, 0x87, 0x278, {0x89,0x54,0x24,0x1C}, 4, 0x25774F},
      // [6] UpdatePositions Loop3 (root bone fixup) — Steam+0x10A0
      {0x257869, 7, 0x8F, 0x268, {0x8B,0x75,0x14}, 3, 0x257870},
      // [7] UpdatePositions Loop4 (distance constraint) — Steam+0x10A0
      {0x2578A3, 9, 0x97, 0x27C, {0x33,0xC0,0x89,0x44,0x24,0x14}, 6, 0x2578AC},
      // [8] DoTentacles mFirstUpdate init — Steam+0x10A0
      {0x256B40, 10, 0x97, 0x268,
       {0x89,0x55,0xF8, 0x33,0xF6, 0x39,0xB7,0x5C,0x02,0x00,0x00}, 11, 0x256B4A},
   },

   // Constructor pre-zero — Steam+0x10A0, byte-identical cave bytes
   .ctor_fix = {
      0x25682A, 10, 0x256834,
      {
         0xC7,0x81,0x40,0x02,0x00,0x00,0x00,0x00,0x00,0x00,
         0xC7,0x81,0x58,0x02,0x00,0x00,0x00,0x00,0x00,0x00,
         0xC7,0x81,0x5C,0x02,0x00,0x00,0x00,0x00,0x00,0x00,
      },
      30
   },

   // DoTentacles mFirstUpdate: oldPos displacements — Steam+0x10A0
   .doTentacles_oldpos_disp = {
      {0x256B62, 0x114, 0x27C},
      {0x256B6B, 0x11C, 0x284},
   },
};
// clang-format on

// ============================================================================
// Static buffer for DoTentacles bone pointer array
// Original: PblMatrix* local_50[20] on stack (4 tentacles * 5 bones)
// Expanded: 45 entries (9 tentacles * 5 bones)
// Safe because game rendering is single-threaded.
// ============================================================================

static void* g_bone_ptrs[45];

// ============================================================================
// Main patching function
// ============================================================================

static bool apply_tentacle_patches(uintptr_t exe_base, const build_addrs& addrs, cfile& log)
{
   int total_patches = 0;
   bool ok = true;

   // --- 0. Constructor pre-zero: prevent wild zeroing loop from uninitialized pool memory ---
   // The constructor reads mNumTentacles/mBonesPerTentacle BEFORE assigning them.
   // If pool memory isn't zeroed, these can be garbage, causing the zeroing loop
   // to iterate billions of times and corrupt the heap.
   {
      const auto& cf = addrs.ctor_fix;
      size_t cave_size = cf.cave_size + 5; // cave_bytes + JMP
      uint8_t* cave = alloc_cave(cave_size);
      if (!cave) {
         log.printf("Tentacle: ctor pre-zero cave alloc FAILED\n");
         return false;
      }

      memcpy(cave, cf.cave_bytes, cf.cave_size);

      // JMP back to return_offset
      size_t p = cf.cave_size;
      cave[p] = 0xE9;
      *(int32_t*)(cave + p + 1) =
         (int32_t)((cf.return_offset + exe_base) - (uintptr_t)(cave + p + 5));

      write_jmp(cf.overwrite_offset + exe_base, (uintptr_t)cave, cf.overwrite_size);
      total_patches++;
      log.printf("Tentacle:   Constructor pre-zero → cave at %p\n", cave);
   }

   // --- 1. Initialize bone hash table ---
   uintptr_t hash_table_runtime = addrs.bone_hash_table_va - 0x400000 + exe_base;
   if (!init_bone_hashes(hash_table_runtime, log)) {
      log.printf("Tentacle: bone hash verification FAILED (algorithm mismatch)\n");
      return false;
   }
   log.printf("Tentacle: bone hash table initialized (45 entries, verified against game)\n");

   // --- 2. Scan each function for oldPos and distance offset patches ---
   // With the frozen-prefix architecture, scalar fields stay at original offsets
   // (they never move). Only oldPos-related displacements need updating, because
   // per-function array base redirects shift the base to this+0x268.
   struct func_entry {
      const char* name;
      uintptr_t file_offset;
      size_t size;
   };

   func_entry functions[] = {
      {"Constructor",         addrs.fn_constructor,       addrs.sz_constructor},
      {"UpdateTimer",         addrs.fn_updateTimer,       addrs.sz_updateTimer},
      {"EnforceSphere",       addrs.fn_enforceSphere,     addrs.sz_enforceSphere},
      {"EnforceBox",          addrs.fn_enforceBox,        addrs.sz_enforceBox},
      {"EnforceCylinder",     addrs.fn_enforceCylinder,   addrs.sz_enforceCylinder},
      {"EnforceCollisions",   addrs.fn_enforceCollisions, addrs.sz_enforceCollisions},
      {"UpdatePose",          addrs.fn_updatePose,        addrs.sz_updatePose},
      {"UpdatePositions",     addrs.fn_updatePositions,   addrs.sz_updatePositions},
      {"DoTentacles",         addrs.fn_doTentacles,       addrs.sz_doTentacles},
   };

   for (const auto& fn : functions) {
      uintptr_t fn_addr = fn.file_offset + exe_base;
      int fn_patches = 0;

      // Patch oldPos start offset: 0x120 → 0x288 (relative to redirected array base)
      for (const auto& off : OLDPOS_REDIRECT) {
         fn_patches += scan_and_patch(fn_addr, fn.size, off.old_val, off.new_val, log);
      }

      // Patch negative offsets (tPos-to-oldPos distance) — only in UpdatePositions and UpdatePose
      for (const auto& off : NEGATIVE_OFFSETS) {
         fn_patches += scan_and_patch(fn_addr, fn.size, off.old_val, off.new_val, log);
      }

      // Patch interior oldPos offsets (only in relevant functions)
      for (const auto& off : OLDPOS_INTERIOR_OFFSETS) {
         fn_patches += scan_and_patch(fn_addr, fn.size, off.old_val, off.new_val, log);
      }

      log.printf("Tentacle:   %s: %d offset patches\n", fn.name, fn_patches);
      total_patches += fn_patches;
   }

   // --- 2b. Per-function array base redirects (frozen-prefix architecture) ---
   // Each TentacleSimulator function uses a separate register for array iteration.
   // We redirect it from this+0 to this+0x268 (extension area) via code caves:
   //   LEA dest, [src + new_disp]   — set array base to extension area
   //   <restore overwritten bytes>   — replay any instructions we overwrote
   //   JMP back                      — resume original code
   for (int i = 0; i < build_addrs::NUM_ARRAY_REDIRECTS; i++) {
      const auto& r = addrs.array_redirects[i];
      if (r.file_offset == 0) continue;

      size_t cave_size = 6 + r.restore_size + 5; // LEA(6) + restore + JMP(5)
      uint8_t* cave = alloc_cave(cave_size);
      if (!cave) {
         log.printf("Tentacle: code cave alloc failed for array redirect %d\n", i);
         ok = false;
         continue;
      }

      size_t p = 0;
      cave[p++] = 0x8D;                           // LEA opcode
      cave[p++] = r.lea_modrm;                    // ModRM: dest,[src+disp32]
      *(uint32_t*)(cave + p) = r.new_disp; p += 4;

      memcpy(cave + p, r.restore_bytes, r.restore_size);
      p += r.restore_size;

      cave[p] = 0xE9;                             // JMP rel32
      *(int32_t*)(cave + p + 1) =
         (int32_t)((r.return_offset + exe_base) - (uintptr_t)(cave + p + 5));

      write_jmp(r.file_offset + exe_base, (uintptr_t)cave, r.overwrite_size);
      total_patches++;
      log.printf("Tentacle:   Array base redirect %d → cave at %p\n", i, cave);

      // Fix loop-back Jcc that targets the middle of our overwrite.
      // Redirect it to cave+6 (the restore_bytes, skipping the LEA).
      if (r.loopback_rel32 != 0) {
         uintptr_t rel32_addr = r.loopback_rel32 + exe_base;
         uintptr_t cave_secondary = (uintptr_t)(cave + 6); // after LEA, at restore_bytes
         *(int32_t*)rel32_addr = (int32_t)(cave_secondary - (rel32_addr + 4));
         total_patches++;
         log.printf("Tentacle:     Loop-back fixup at %08x → cave+6 (%p)\n",
                    (uint32_t)rel32_addr, (void*)cave_secondary);
      }
   }

   // --- 3. Redirect bone hash table pointers ---
   uint32_t new_hash_addr = (uint32_t)(uintptr_t)&g_bone_hashes[0];
   for (int i = 0; i < 3; i++) {
      uintptr_t ref_addr = addrs.hash_ref[i] + exe_base;
      uint32_t old_val = *(uint32_t*)ref_addr;

      // The reference should contain the original hash table VA (possibly relocated)
      uint32_t expected_va = (uint32_t)(addrs.bone_hash_table_va - 0x400000 + exe_base);
      if (old_val != expected_va) {
         log.printf("Tentacle: hash ref %d at %08x: expected %08x, found %08x — SKIPPED\n",
                    i, ref_addr, expected_va, old_val);
         continue;
      }

      *(uint32_t*)ref_addr = new_hash_addr;
      total_patches++;
      log.printf("Tentacle:   Hash table ref %d: %08x → %08x\n", i, expected_va, new_hash_addr);
   }

   // --- 4. Stack frame patches (EBP-framed functions, just patch imm32) ---
   struct stack_patch {
      const char* name;
      uintptr_t file_offset;
      uint32_t old_val;
      uint32_t new_val;
   };

   stack_patch stack_patches[] = {
      {"UpdatePose",        addrs.updatePose_sub_esp,        addrs.updatePose_old_esp,        addrs.updatePose_new_esp},
      {"UpdatePositions",   addrs.updatePositions_sub_esp,   addrs.updatePositions_old_esp,   addrs.updatePositions_new_esp},
      {"EnforceCollisions", addrs.enforceCollisions_sub_esp, addrs.enforceCollisions_old_esp, addrs.enforceCollisions_new_esp},
   };

   for (const auto& sp : stack_patches) {
      uintptr_t addr = sp.file_offset + exe_base;
      uint32_t* p = (uint32_t*)addr;
      if (*p != sp.old_val) {
         log.printf("Tentacle: stack frame %s FAILED: expected %x, found %x\n", sp.name, sp.old_val, *p);
         ok = false;
         continue;
      }
      *p = sp.new_val;
      total_patches++;
      log.printf("Tentacle:   %s stack: 0x%x → 0x%x\n", sp.name, sp.old_val, sp.new_val);
   }

   // --- 5. DoTentacles local bone_ptrs array redirect ---
   // Replace LEA reg,[ESP+0x20] with MOV reg,&g_bone_ptrs (via code caves)
   uint32_t bone_ptrs_base = (uint32_t)(uintptr_t)&g_bone_ptrs[0];

   for (int i = 0; i < 4; i++) {
      const auto& lea = addrs.doTentacles_leas[i];
      uintptr_t lea_addr = lea.file_offset + exe_base;

      // Allocate code cave: MOV reg,imm32 (5) + restore bytes + JMP rel32 (5)
      size_t cave_size = 5 + lea.restore_size + 5;
      uint8_t* cave = alloc_cave(cave_size);
      if (!cave) {
         log.printf("Tentacle: code cave alloc failed for LEA redirect %d\n", i);
         ok = false;
         continue;
      }

      // MOV reg, &g_bone_ptrs + delta
      cave[0] = lea.mov_reg_opcode;
      *(uint32_t*)(cave + 1) = bone_ptrs_base + lea.addr_delta;

      // Restore overwritten instruction bytes
      memcpy(cave + 5, lea.restore_bytes, lea.restore_size);

      // JMP back
      uintptr_t return_addr = lea.return_offset + exe_base;
      size_t jmp_offset = 5 + lea.restore_size;
      cave[jmp_offset] = 0xE9;
      *(int32_t*)(cave + jmp_offset + 1) =
         (int32_t)(return_addr - (uintptr_t)(cave + jmp_offset + 5));

      // Patch original code: JMP to cave + NOP padding
      write_jmp(lea_addr, (uintptr_t)cave, lea.overwrite_size);

      total_patches++;
      log.printf("Tentacle:   DoTentacles LEA redirect %d → cave at %p\n", i, cave);
   }

   // --- 6. SetProperty tentacle limit: 4 → 9 (not present in all builds) ---
   if (addrs.setProperty_limit != 0)
   {
      uintptr_t addr = addrs.setProperty_limit + exe_base;
      uint8_t* p = (uint8_t*)addr;
      if (*p == 0x04) {
         *p = 0x09;
         total_patches++;
         log.printf("Tentacle:   SetProperty limit: 4 → 9\n");
      }
      else {
         log.printf("Tentacle: SetProperty limit at %08x: expected 04, found %02x — SKIPPED\n",
                    addr, *p);
      }
   }

   // --- 7. sMemoryPool element size: 0x268 → 0x778 ---
   // Frozen prefix (0x268) + tPos_ext (0x288) + oldPos_ext (0x288) = 0x778
   {
      uintptr_t pool_sites[] = {
         addrs.memPool1 + exe_base,
         addrs.memPool2 + exe_base,
         addrs.memPool3 ? addrs.memPool3 + exe_base : 0,
      };

      for (int i = 0; i < 3; i++) {
         if (pool_sites[i] == 0) continue;
         uint32_t* p = (uint32_t*)pool_sites[i];
         if (*p == 0x268) {
            *p = 0x778;
            total_patches++;
            log.printf("Tentacle:   MemoryPool site %d: 0x268 → 0x778\n", i + 1);
         }
         else {
            log.printf("Tentacle: MemoryPool site %d at %08x: expected 268, found %x — SKIPPED\n",
                       i + 1, pool_sites[i], *p);
         }
      }
   }

   // --- 8. Render stack sizes ---
   struct render_patch {
      const char* name;
      uintptr_t file_offset;
      uint32_t old_val;
      uint32_t new_val;
   };

   render_patch render_patches[] = {
      {"EntitySoldierClass::Render", addrs.renderSoldier, addrs.renderSoldier_old, addrs.renderSoldier_new},
      {"AnimatedAddon::Render (1)",  addrs.renderAddon1,  addrs.renderAddon1_old,  addrs.renderAddon1_new},
      {"AnimatedAddon::Render (2)",  addrs.renderAddon2,  addrs.renderAddon2_old,  addrs.renderAddon2_new},
   };

   for (const auto& rp : render_patches) {
      uintptr_t addr = rp.file_offset + exe_base;
      uint32_t* p = (uint32_t*)addr;
      if (*p == rp.old_val) {
         *p = rp.new_val;
         total_patches++;
         log.printf("Tentacle:   %s: 0x%x → 0x%x\n", rp.name, rp.old_val, rp.new_val);
      }
      else {
         log.printf("Tentacle: %s at %08x: expected %x, found %x — SKIPPED\n",
                    rp.name, addr, rp.old_val, *p);
      }
   }

   // --- 9. Bitfield widening: numTentacles 3-bit → 4-bit ---
   for (int i = 0; i < build_addrs::NUM_BF_PATCHES; i++) {
      const auto& bp = addrs.bitfield[i];
      uintptr_t addr = bp.file_offset + exe_base;

      if (bp.size == 1) {
         uint8_t* p = (uint8_t*)addr;
         if (*p == (uint8_t)bp.old_val) {
            *p = (uint8_t)bp.new_val;
            total_patches++;
         }
         else {
            log.printf("Tentacle: bitfield[%d] at %08x: expected %02x, found %02x — SKIPPED\n",
                       i, addr, bp.old_val, *p);
         }
      }
      else {
         uint32_t* p = (uint32_t*)addr;
         if (*p == bp.old_val) {
            *p = bp.new_val;
            total_patches++;
         }
         else {
            log.printf("Tentacle: bitfield[%d] at %08x: expected %08x, found %08x — SKIPPED\n",
                       i, addr, bp.old_val, *p);
            ok = false;
         }
      }
   }
   log.printf("Tentacle:   Bitfield widening: %d patches\n", build_addrs::NUM_BF_PATCHES);

   // --- 10. UpdatePositions: oldPos via tPos-relative pointer patches ---
   for (int i = 0; i < build_addrs::NUM_TPOS_REL_PATCHES; i++) {
      const auto& dp = addrs.updatePositions_tpos_rel[i];
      if (dp.file_offset == 0) continue; // unused slot (some builds need fewer patches)
      uintptr_t addr = dp.file_offset + exe_base;
      uint32_t* p = (uint32_t*)addr;
      if (*p == dp.old_val) {
         *p = dp.new_val;
         total_patches++;
      }
      else {
         log.printf("Tentacle: tpos_rel[%d] at %08x: expected %x, found %x — SKIPPED\n",
                    i, addr, dp.old_val, *p);
         ok = false;
      }
   }
   log.printf("Tentacle:   UpdatePositions tPos-relative oldPos: %d patches\n",
              build_addrs::NUM_TPOS_REL_PATCHES);

   // --- 11. DoTentacles mFirstUpdate: oldPos displacement patches (Steam/GOG only) ---
   // The mFirstUpdate init block uses pre-increment + smaller displacements (0x114, 0x11C)
   // that scan_and_patch doesn't catch. Modtools uses 0x120 which IS caught by scan_and_patch.
   {
      int dt_count = 0;
      for (int i = 0; i < build_addrs::NUM_DOTENTACLES_OLDPOS_PATCHES; i++) {
         const auto& dp = addrs.doTentacles_oldpos_disp[i];
         if (dp.file_offset == 0) continue;
         uintptr_t addr = dp.file_offset + exe_base;
         uint32_t* p = (uint32_t*)addr;
         if (*p == dp.old_val) {
            *p = dp.new_val;
            total_patches++;
            dt_count++;
         }
         else {
            log.printf("Tentacle: doTentacles_oldpos[%d] at %08x: expected %x, found %x — SKIPPED\n",
                       i, addr, dp.old_val, *p);
            ok = false;
         }
      }
      if (dt_count > 0)
         log.printf("Tentacle:   DoTentacles mFirstUpdate oldPos: %d patches\n", dt_count);
   }

   log.printf("Tentacle: total patches applied: %d (ok=%d)\n", total_patches, (int)ok);
   return ok;
}

// ============================================================================
// Public API
// ============================================================================

bool patch_tentacle_limit(uintptr_t exe_base)
{
   cfile log{"BF2GameExt.log", "a"};
   if (!log) return false;

   log.printf("\n--- Tentacle Bone Limit Patch ---\n");

   // Identify executable build
   static const build_addrs* builds[] = {&MODTOOLS, &STEAM, &GOG};

   for (const build_addrs* build : builds) {
      char* id_addr = (char*)(build->id_file_offset + exe_base);

      if (memcmp(id_addr, &build->id_expected, sizeof(build->id_expected)) != 0)
         continue;

      log.printf("Tentacle: identified build, applying patches\n");
      return apply_tentacle_patches(exe_base, *build, log);
   }

   log.printf("Tentacle: no matching build found, skipping\n");
   return true; // Not an error — just a build we don't support yet
}
