#include "pch.h"

#include "particle_renderer_patch.hpp"
#include "patch_table.hpp"
#include "cfile.hpp"
#include "lua_hooks.hpp"

#include <detours.h>
#include <string.h>

// ============================================================================
// 1. Constants
// ============================================================================

static constexpr int MAX_CACHES   = 120;
static constexpr int CACHE_STRIDE = 0x3558;

// Field offsets within each RedParticleRenderer cache entry
static constexpr int OFF_PARTICLE_INDEX = 0x3520;
static constexpr int OFF_TEX_HASH       = 0x3524;
static constexpr int OFF_BLEND_MODE     = 0x3528;
static constexpr int OFF_RENDER_FLAGS   = 0x352C;
static constexpr int OFF_NUM_VERTS      = 0x3530;
static constexpr int OFF_NUM_INDICES    = 0x3534;

// ============================================================================
// 2. Per-build addresses (offsets from exe base, i.e. VA - 0x400000)
// ============================================================================

struct renderer_addrs {
   uintptr_t id_file_offset;
   uint64_t  id_expected;

   uintptr_t fn_SubmitParticle;  // __cdecl, 8 params
   uintptr_t fn_AddParticle;     // __thiscall, 8 params + this

   uintptr_t g_currentCache;     // RedParticleRenderer** (global ptr)
   uintptr_t g_s_cacheIndex;     // int* (global)
   uintptr_t g_s_caches;         // char* (array base)

   uintptr_t limit_byte_file_offset;  // CMP EDX,imm8 in SetCurrentCache (file offset of the imm8)
};

// clang-format off
static const renderer_addrs MODTOOLS = {
   .id_file_offset     = 0x62b59c,
   .id_expected        = 0x746163696c707041,

   .fn_SubmitParticle  = 0x425180,
   .fn_AddParticle     = 0x424E10,

   .g_currentCache     = 0xA5F644,
   .g_s_cacheIndex     = 0xA5F648,
   .g_s_caches         = 0xA5F650,

   .limit_byte_file_offset = 0x424D3D,  // CMP EDX, 0x0F in SetCurrentCache
};

static const renderer_addrs STEAM = {
   .id_file_offset     = 0x39e234,
   .id_expected        = 0x746163696c707041,

   .fn_SubmitParticle  = 0,  // TODO
   .fn_AddParticle     = 0,
   .g_currentCache     = 0,
   .g_s_cacheIndex     = 0,
   .g_s_caches         = 0,

   .limit_byte_file_offset = 0,  // TODO
};

static const renderer_addrs GOG = {
   .id_file_offset     = 0x39f298,
   .id_expected        = 0x746163696c707041,

   .fn_SubmitParticle  = 0,  // TODO
   .fn_AddParticle     = 0,
   .g_currentCache     = 0,
   .g_s_cacheIndex     = 0,
   .g_s_caches         = 0,

   .limit_byte_file_offset = 0,  // TODO
};
// clang-format on

// ============================================================================
// 3. Original function typedefs and pointers
// ============================================================================

// SubmitParticle is __cdecl: all params on stack, caller cleans up.
//   void SubmitParticle(int pit, PblVector3* pos, RedColor* color,
//                       float size, float rot, uint texCoords,
//                       PblVector3* right, PblVector3* up)
typedef void(__cdecl* fn_SubmitParticle)(int, void*, void*, float, float,
                                         unsigned int, void*, void*);

// AddParticle is __thiscall: this in ECX, 8 params on stack, RET 0x20.
//   bool AddParticle(RedParticleRenderer* this, int pit, PblVector3* pos,
//                    RedColor* color, float size, float rot, uint texCoords,
//                    PblVector3* right, PblVector3* up)
typedef bool(__thiscall* fn_AddParticle)(void*, int, void*, void*, float, float,
                                         unsigned int, void*, void*);

static fn_SubmitParticle original_SubmitParticle = nullptr;
static fn_AddParticle    original_AddParticle    = nullptr;

// ============================================================================
// 4. Resolved globals
// ============================================================================

static void** g_currentCache  = nullptr;   // -> RedParticleRenderer*
static int*   g_s_cacheIndex  = nullptr;
static char*  g_s_caches      = nullptr;

// ============================================================================
// 5. Inline vert/index count tables (from GetVertCount / GetIndexCount)
// ============================================================================

static int get_vert_count(int pit)
{
   switch (pit) {
   case 0: case 3: case 6: case 7: return 4;
   case 2:                          return 2;
   case 5:                          return 9;
   default:                         return 0;
   }
}

static int get_index_count(int pit)
{
   switch (pit) {
   case 0: case 2: case 6: case 7: return 6;
   case 3:                          return 12;
   case 5:                          return 21;
   default:                         return 0;
   }
}

// ============================================================================
// 6. Hooked SubmitParticle
// ============================================================================

static void __cdecl hooked_SubmitParticle(int pit, void* pos, void* color,
                                           float size, float rot,
                                           unsigned int texCoords,
                                           void* right, void* up)
{
   void* cache = *g_currentCache;
   if (!cache) return;

   bool added = original_AddParticle(cache, pit, pos, color, size, rot,
                                      texCoords, right, up);

   if (!added) {
      // Current cache entry is full (200 particles). Try to allocate a new one.
      if (*g_s_cacheIndex >= MAX_CACHES) return;  // all slots used, drop particle

      // Copy texture/blend/flags from current entry
      char* cur = (char*)cache;
      uint32_t texHash     = *(uint32_t*)(cur + OFF_TEX_HASH);
      int      blendMode   = *(int*)(cur + OFF_BLEND_MODE);
      uint32_t renderFlags = *(uint32_t*)(cur + OFF_RENDER_FLAGS);

      // Allocate new cache entry
      char* newEntry = g_s_caches + (*g_s_cacheIndex) * CACHE_STRIDE;
      (*g_s_cacheIndex)++;

      // Initialize new entry
      *(uint32_t*)(newEntry + OFF_TEX_HASH)       = texHash;
      *(int*)(newEntry + OFF_BLEND_MODE)           = blendMode;
      *(uint32_t*)(newEntry + OFF_RENDER_FLAGS)    = renderFlags;
      *(int*)(newEntry + OFF_NUM_VERTS)            = 0;
      *(int*)(newEntry + OFF_NUM_INDICES)          = 0;
      *(int*)(newEntry + OFF_PARTICLE_INDEX)       = 0;

      // Update global pointer
      *g_currentCache = newEntry;
      cache = newEntry;

      added = original_AddParticle(cache, pit, pos, color, size, rot,
                                    texCoords, right, up);
      if (!added) return;  // shouldn't happen on a fresh entry, but be safe
   }

   // Update vertex/index counts on the (possibly new) current cache entry
   char* cur = (char*)*g_currentCache;
   *(int*)(cur + OFF_NUM_VERTS)   += get_vert_count(pit);
   *(int*)(cur + OFF_NUM_INDICES) += get_index_count(pit);

   // Trail particle degenerate index optimization:
   // If this is a trail segment (type 2) and the particle two slots back is a
   // trail start (type 1), subtract 6 indices (shared degenerate vertices).
   if (pit == 2) {
      int particleIndex = *(int*)(cur + OFF_PARTICLE_INDEX);
      if (particleIndex >= 2) {
         // Each particle entry is 0x44 bytes; the type field is at offset 0.
         int prevPrevType = *(int*)(cur + (particleIndex - 2) * 0x44);
         if (prevPrevType == 1) {
            *(int*)(cur + OFF_NUM_INDICES) -= 6;
         }
      }
   }
}

// ============================================================================
// 7. Detours install / uninstall
// ============================================================================

static bool g_hooks_installed = false;

static bool install_hooks(uintptr_t exe_base, const renderer_addrs& addrs, cfile& log)
{
   if (!addrs.fn_SubmitParticle || !addrs.fn_AddParticle) {
      log.printf("ParticleRenderer: addresses not available for this build, skipping\n");
      return true;  // not an error, just unsupported build
   }

   original_SubmitParticle = (fn_SubmitParticle)(addrs.fn_SubmitParticle + exe_base);
   original_AddParticle    = (fn_AddParticle)(addrs.fn_AddParticle + exe_base);

   g_currentCache  = (void**)(addrs.g_currentCache + exe_base);
   g_s_cacheIndex  = (int*)(addrs.g_s_cacheIndex + exe_base);
   g_s_caches      = g_sCaches_storage;  // use DLL-allocated array (matches binary patch redirects)

   // Patch the renderer cache entry limit (CMP EDX, imm8 in SetCurrentCache).
   // Sections are writable when this runs — no VirtualProtect needed.
   if (addrs.limit_byte_file_offset) {
      // resolve_file_address isn't available here, but the exe is loaded at exe_base
      // and the .text section's file_offset == virtual_offset for this exe.
      uint8_t* limit_ptr = (uint8_t*)(addrs.limit_byte_file_offset + exe_base);
      if (*limit_ptr == 0x0F) {  // verify old value (15)
         *limit_ptr = (uint8_t)MAX_CACHES;
         log.printf("ParticleRenderer: patched cache limit %d -> %d\n", 15, MAX_CACHES);
      } else {
         log.printf("ParticleRenderer: WARNING limit byte mismatch (expected 0x0F, got 0x%02X)\n", *limit_ptr);
      }
   }

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SubmitParticle, hooked_SubmitParticle);
   LONG result = DetourTransactionCommit();

   if (result != NO_ERROR) {
      log.printf("ParticleRenderer: Detours commit FAILED (%ld)\n", result);
      return false;
   }

   g_hooks_installed = true;
   log.printf("ParticleRenderer: SubmitParticle hook installed (SubmitParticle=%08x, AddParticle=%08x)\n",
              (unsigned)(addrs.fn_SubmitParticle + exe_base),
              (unsigned)(addrs.fn_AddParticle + exe_base));
   return true;
}

static void uninstall_hooks()
{
   if (!g_hooks_installed) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_SubmitParticle)
      DetourDetach(&(PVOID&)original_SubmitParticle, hooked_SubmitParticle);
   DetourTransactionCommit();

   g_hooks_installed = false;
}

// ============================================================================
// 8. Public API
// ============================================================================

bool patch_particle_renderer(uintptr_t exe_base)
{
   cfile log{"BF2GameExt.log", "a"};
   if (!log) return false;

   log.printf("\n--- Particle Renderer Overflow Patch ---\n");

   static const renderer_addrs* builds[] = {&MODTOOLS, &STEAM, &GOG};

   for (const renderer_addrs* build : builds) {
      char* id_addr = (char*)(build->id_file_offset + exe_base);
      if (memcmp(id_addr, &build->id_expected, sizeof(build->id_expected)) != 0)
         continue;

      log.printf("ParticleRenderer: identified build, installing hooks\n");

      if (!install_hooks(exe_base, *build, log))
         return false;

      return true;
   }

   log.printf("ParticleRenderer: no matching build found, skipping\n");
   return true;
}

void unpatch_particle_renderer()
{
   uninstall_hooks();
}
