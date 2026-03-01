#include "pch.h"

#include "patch_table.hpp"

const static uint32_t DLC_mission_size = 0x110;
const static uint32_t DLC_mission_patch_limit = 0x1000;

static char DLC_mission_table_storage[DLC_mission_size * DLC_mission_patch_limit] = {};
static const uint32_t DLC_mission_table_address = (uint32_t)&DLC_mission_table_storage[0];

// Particle cache increase: 300 -> 1200 entries
// CacheParticle struct is 36 (0x24) bytes: PblVector3 mPos, RedColorValue mColor, float mSize, float mRotation
static const uint32_t particle_cache_new_limit = 1200;
static char g_cachedParticles_storage[particle_cache_new_limit * 0x24] = {};
static const uint32_t g_cachedParticles_address = (uint32_t)&g_cachedParticles_storage[0];
static const uint32_t modtools_sCachedParticles_va = 0xB9DB78;

// Renderer cache increase: 15 -> 60 entries
// Each RedParticleRenderer cache entry is 0x3558 bytes.
static const uint32_t renderer_cache_new_limit = 120;
char g_sCaches_storage[renderer_cache_new_limit * 0x3558] = {};
static const uint32_t g_sCaches_address = (uint32_t)&g_sCaches_storage[0];
static const uint32_t modtools_sCaches_va = 0xE5F650;

// Function names matched from BF1 Mac executable. Could be wrong in cases.

// clang-format off

const exe_patch_list patch_lists[EXE_COUNT] = {
   exe_patch_list{
      .name = "BF2_modtools",
      .id_address_is_file_offset = true,
      .id_address = 0x62b59c,
      .expected_id = 0x746163696c707041,
      .patches =
         {
            patch_set{
               .name = "RedMemory Heap Extensions",
               .patches =
                  {
                     patch{0x337921, 0x4000000, 0x10000000, {.file_offset = true}}, // malloc call arg
                     patch{0x33792c, 0x4000000, 0x10000000, {.file_offset = true}}, // malloc'd block end pointer
                  },
            },

            patch_set{
               .name = "SoundParameterized Layer Limit Extension",
               .patches =
                  {
                     patch{0x6227c2, 0xa0, 0x2000, {.file_offset = true}},
                  },
            },

            patch_set{
               .name = "DLC Mission Limit Extension",
               .patches =
                  {
                     patch{0x4935c, 0xb08308, DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}},                         // SetCurrentMap
                     patch{0x493ac, 0xb0830c, (0xb0830c - 0xb08308) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // SetCurrentMission
                     patch{0x49415, 0xb08310, (0xb08310 - 0xb08308) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // GetContentDirectory
                     patch{0x49472, 0xb0830c, (0xb0830c - 0xb08308) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // IsMissionDownloaded
                     patch{0x494fb, 0x1f4, DLC_mission_patch_limit, {.file_offset = true, .expected_is_va = true}},                              // AddDownloadableContent
                     patch{0x4951f, 0xb08308, DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}},                         // AddDownloadableContent
                     patch{0x49542, 0xb0830c, (0xb0830c - 0xb08308) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // AddDownloadableContent
                     patch{0x49548, 0xb08310, (0xb08310 - 0xb08308) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // AddDownloadableContent
                     patch{0x49571, 0xb08413, (0xb08413 - 0xb08308) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // AddDownloadableContent
                     patch{0x4957d, 0xb08414, (0xb08414 - 0xb08308) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // AddDownloadableContent
                  },
            },

            patch_set{
               .name = "Particle Cache Increase",
               .patches =
                  {
                     // Value patches
                     patch{0x26D828, 0x0000012C, 0x000004B0, {.file_offset = true}},                                                                                             // CacheParticle: CMP ECX, 300 -> 1200
                     patch{0x26DAF7, 0x00000994, 0x000025B4, {.file_offset = true}},                                                                                             // FlushParticleCache: SUB ESP, 0x994 -> 0x25B4
                     patch{0x26DD1E, 0x00000994, 0x000025B4, {.file_offset = true}},                                                                                             // FlushParticleCache: ADD ESP, 0x994 -> 0x25B4
                     patch{0x26DB6D, 0x0000012C, 0x000004B0, {.file_offset = true}},                                                                                             // FlushParticleCache: heap.maxCount 300 -> 1200
                     // VA redirects — CacheParticle function (sCachedParticles array -> DLL static buffer)
                     patch{0x26D83E, modtools_sCachedParticles_va, g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},                                     // mPos.x (base)
                     patch{0x26D858, 0xB9DB84, (0xB9DB84 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.r
                     patch{0x26D876, 0xB9DB94, (0xB9DB94 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mSize
                     patch{0x26D882, 0xB9DB98, (0xB9DB98 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mRotation
                     // VA redirects — FlushParticleCache sort loop
                     patch{0x26DB95, modtools_sCachedParticles_va, g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},                                     // mPos.x
                     patch{0x26DBAA, 0xB9DB7C, (0xB9DB7C - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mPos.y
                     patch{0x26DBBF, 0xB9DB80, (0xB9DB80 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mPos.z
                     patch{0x26DC05, 0xB9DB94, (0xB9DB94 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mSize cmp
                     patch{0x26DC12, 0xB9DB90, (0xB9DB90 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.a fade
                     patch{0x26DC18, 0xB9DB94, (0xB9DB94 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mSize fade
                     patch{0x26DC22, 0xB9DB90, (0xB9DB90 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.a write
                     // VA redirects — FlushParticleCache render loop
                     patch{0x26DC78, 0xB9DB8C, (0xB9DB8C - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.b
                     patch{0x26DC89, 0xB9DB88, (0xB9DB88 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.g
                     patch{0x26DC9E, 0xB9DB84, (0xB9DB84 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.r
                     patch{0x26DCB3, 0xB9DB90, (0xB9DB90 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.a
                     patch{0x26DCC8, 0xB9DB94, (0xB9DB94 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mSize
                     patch{0x26DCDD, 0xB9DB98, (0xB9DB98 - modtools_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mRotation
                     patch{0x26DCEA, modtools_sCachedParticles_va, g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},                                     // mPos (SubmitParticle LEA)
                     // VA redirects — RedParticleRenderer s_caches[15] -> DLL static buffer s_caches[120]
                     // SetCurrentCache: MOV EAX, &s_caches[0].m_blendMode
                     patch{0x424D1F, 0xE62B78, (0xE62B78 - modtools_sCaches_va) + g_sCaches_address, {.file_offset = true, .expected_is_va = true}},
                     // SetCurrentCache: ADD ECX, s_caches (found-entry path)
                     patch{0x424D55, modtools_sCaches_va, g_sCaches_address, {.file_offset = true, .expected_is_va = true}},
                     // SetCurrentCache: ADD EAX, s_caches (new-entry path)
                     patch{0x424D7C, modtools_sCaches_va, g_sCaches_address, {.file_offset = true, .expected_is_va = true}},
                     // RenderAllCaches: MOV EBP, &s_caches[0].m_numVerts
                     patch{0x42770A, 0xE62B80, (0xE62B80 - modtools_sCaches_va) + g_sCaches_address, {.file_offset = true, .expected_is_va = true}},
                  },
            },
         },
   },
   
   exe_patch_list{
      .name = "BattlefrontII.exe GoG",
      .id_address_is_file_offset = true,
      .id_address = 0x39f298,
      .expected_id = 0x746163696c707041,
      .patches =
         {
            patch_set{
               .name = "RedMemory Heap Extensions",
               .patches =
                  {
                     patch{0x217651, 0x4000000, 0x10000000, {.file_offset = true}}, // malloc call arg
                     patch{0x217667, 0x4000000, 0x10000000, {.file_offset = true}}, // malloc'd block end pointer
                  },
            },

            patch_set{
               .name = "SoundParameterized Layer Limit Extension",
               .patches =
                  {
                     patch{0x3e310c, 0xa0, 0x2000, {.file_offset = true}},
                  },
            },

            patch_set{
               .name = "DLC Mission Limit Extension",
               .patches =
                  {
                     patch{0x8de7d, 0x1f4, DLC_mission_patch_limit, {.file_offset = true}},                                                         // AddDownloadableContent
                     patch{0x8de9f, 0x1e31f00, DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}},                           // AddDownloadableContent
                     patch{0x8dec3, 0x1e31f04, (0x1e31f04 - 0x1e31f00) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // AddDownloadableContent
                     patch{0x8dec9, 0x1e31f08, (0x1e31f08 - 0x1e31f00) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // AddDownloadableContent
                     patch{0x8def0, 0x1e3200b, (0x1e3200b - 0x1e31f00) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // AddDownloadableContent
                     patch{0x8def7, 0x1e3200c, (0x1e3200c - 0x1e31f00) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // AddDownloadableContent
                     patch{0x8df28, 0x1e31f00, DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}},                           // SetCurrentMap
                     patch{0x8df68, 0x1e31f04, (0x1e31f04 - 0x1e31f00) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // SetCurrentMission
                     patch{0x8dfb4, 0x1e31f08, (0x1e31f08 - 0x1e31f00) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // GetContentDirectory
                     patch{0x8dfce, 0x1e31f04, (0x1e31f04 - 0x1e31f00) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // IsMissionDownloaded
                  },
            },

            patch_set{
               .name = "Particle Cache Increase",
               .patches = {}, // TODO: find GOG addresses
            },
         },
   },

   exe_patch_list{
      .name = "BattlefrontII.exe Steam",
      .id_address_is_file_offset = true,
      .id_address = 0x39e234,
      .expected_id = 0x746163696c707041,
      .patches =
         {
            patch_set{
               .name = "RedMemory Heap Extensions",
               .patches =
                  {
                     patch{0x2165b1, 0x4000000, 0x10000000, {.file_offset = true}}, // malloc call arg
                     patch{0x2165c7, 0x4000000, 0x10000000, {.file_offset = true}}, // malloc'd block end pointer
                  },
            },

            patch_set{
               .name = "SoundParameterized Layer Limit Extension",
               .patches =
                  {
                     patch{0x3e170c, 0xa0, 0x2000, {.file_offset = true}},
                  },
            },

            patch_set{
               .name = "DLC Mission Limit Extension",
               .patches =
                  {
                     patch{0x8de7d, 0x1f4, DLC_mission_patch_limit, {.file_offset = true}},                                                         // AddDownloadableContent
                     patch{0x8de9f, 0x1e30950, DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}},                           // AddDownloadableContent
                     patch{0x8dec3, 0x1e30954, (0x1e30954 - 0x1e30950) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // AddDownloadableContent
                     patch{0x8dec9, 0x1e30958, (0x1e30958 - 0x1e30950) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // AddDownloadableContent
                     patch{0x8def0, 0x1e30a5b, (0x1e30a5b - 0x1e30950) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // AddDownloadableContent
                     patch{0x8def7, 0x1e30a5c, (0x1e30a5c - 0x1e30950) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // AddDownloadableContent
                     patch{0x8df28, 0x1e30950, DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}},                           // SetCurrentMap
                     patch{0x8df68, 0x1e30954, (0x1e30954 - 0x1e30950) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // SetCurrentMission
                     patch{0x8dfb4, 0x1e30958, (0x1e30958 - 0x1e30950) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // GetContentDirectory
                     patch{0x8dfce, 0x1e30954, (0x1e30954 - 0x1e30950) + DLC_mission_table_address, {.file_offset = true, .expected_is_va = true}}, // IsMissionDownloaded
                  },
            },

            patch_set{
               .name = "Particle Cache Increase",
               .patches = {}, // TODO: find Steam addresses
            },
         },
   },
};
