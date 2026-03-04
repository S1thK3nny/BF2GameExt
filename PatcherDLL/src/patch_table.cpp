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
static const uint32_t steam_sCachedParticles_va = 0x01EF5120;
static const uint32_t gog_sCachedParticles_va = 0x01EF6640;

// Renderer cache increase: 15 -> 60 entries
// Each RedParticleRenderer cache entry is 0x3558 bytes.
static const uint32_t renderer_cache_new_limit = 120;
char g_sCaches_storage[renderer_cache_new_limit * 0x3558] = {};
static const uint32_t g_sCaches_address = (uint32_t)&g_sCaches_storage[0];
static const uint32_t modtools_sCaches_va = 0xE5F650;
static const uint32_t steam_sCaches_va = 0x009661E0;
static const uint32_t gog_sCaches_va = 0x00967680;

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
               .patches =
                  {
                     // GOG .text: PointerToRawData=0x400, VirtualAddress=0x1000
                     // file_offset = RVA - 0xC00 for all .text patches (same as Steam)
                     // Value patches (GOG FlushParticleCache uses EBP frame — no ADD ESP patch needed)
                     patch{0x20EBA9, 0x0000012C, 0x000004B0, {.file_offset = true}},                                                                                             // CacheParticle: CMP EDI, 300 -> 1200
                     patch{0x20EC1A, 0x00000980, 0x000025A0, {.file_offset = true}},                                                                                             // FlushParticleCache: SUB ESP, 0x980 -> 0x25A0
                     patch{0x20EC79, 0x0000012C, 0x000004B0, {.file_offset = true}},                                                                                             // FlushParticleCache: heap.maxCount 300 -> 1200
                     // VA redirects — CacheParticle function (sCachedParticles array -> DLL static buffer)
                     patch{0x20EBBD, gog_sCachedParticles_va, g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},                                         // mPos.x/y (MOVQ, base)
                     patch{0x20EBC7, 0x01EF6648, (0x01EF6648 - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mPos.z
                     patch{0x20EBDA, 0x01EF664C, (0x01EF664C - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor (MOVDQU, 16 bytes)
                     patch{0x20EBE3, 0x01EF665C, (0x01EF665C - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mSize
                     patch{0x20EBEC, 0x01EF6660, (0x01EF6660 - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mRotation
                     // VA redirects — FlushParticleCache sort loop
                     patch{0x20ECCE, gog_sCachedParticles_va, g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},                                         // mPos.x
                     patch{0x20ECD7, 0x01EF6644, (0x01EF6644 - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mPos.y
                     patch{0x20ECE5, 0x01EF6648, (0x01EF6648 - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mPos.z
                     patch{0x20ED19, 0x01EF665C, (0x01EF665C - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mSize cmp
                     patch{0x20ED2E, 0x01EF6658, (0x01EF6658 - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.a fade
                     patch{0x20ED37, 0x01EF6658, (0x01EF6658 - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.a write
                     // VA redirects — FlushParticleCache render loop
                     patch{0x20EDCB, 0x01EF6658, (0x01EF6658 - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.a
                     patch{0x20EDEE, 0x01EF6654, (0x01EF6654 - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.b
                     patch{0x20EE02, 0x01EF6650, (0x01EF6650 - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.g
                     patch{0x20EE16, 0x01EF664C, (0x01EF664C - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.r
                     patch{0x20EE2A, 0x01EF6660, (0x01EF6660 - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mRotation
                     patch{0x20EE39, 0x01EF665C, (0x01EF665C - gog_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mSize
                     patch{0x20EE4C, gog_sCachedParticles_va, g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},                                         // mPos (SubmitParticle LEA)
                     // VA redirects — RedParticleRenderer s_caches[15] -> DLL static buffer s_caches[120]
                     // SubmitTexture: MOV EAX, &s_caches[0].m_blendMode
                     patch{0x2D376C, 0x0096ABA8, (0x0096ABA8 - gog_sCaches_va) + g_sCaches_address, {.file_offset = true, .expected_is_va = true}},
                     // SubmitTexture: ADD EAX, s_caches (found-entry path)
                     patch{0x2D37A6, gog_sCaches_va, g_sCaches_address, {.file_offset = true, .expected_is_va = true}},
                     // SubmitTexture: ADD EAX, s_caches (new-entry path)
                     patch{0x2D37CA, gog_sCaches_va, g_sCaches_address, {.file_offset = true, .expected_is_va = true}},
                     // RenderAllCaches: MOV ESI, &s_caches[0].m_numVerts
                     patch{0x2D357C, 0x0096ABB0, (0x0096ABB0 - gog_sCaches_va) + g_sCaches_address, {.file_offset = true, .expected_is_va = true}},
                  },
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
               .patches =
                  {
                     // Steam .text: PointerToRawData=0x400, VirtualAddress=0x1000
                     // file_offset = RVA - 0xC00 for all .text patches
                     // Value patches (Steam FlushParticleCache uses EBP frame — no ADD ESP patch needed)
                     patch{0x20DB09, 0x0000012C, 0x000004B0, {.file_offset = true}},                                                                                             // CacheParticle: CMP EDI, 300 -> 1200
                     patch{0x20DB7A, 0x00000980, 0x000025A0, {.file_offset = true}},                                                                                             // FlushParticleCache: SUB ESP, 0x980 -> 0x25A0
                     patch{0x20DBD9, 0x0000012C, 0x000004B0, {.file_offset = true}},                                                                                             // FlushParticleCache: heap.maxCount 300 -> 1200
                     // VA redirects — CacheParticle function (sCachedParticles array -> DLL static buffer)
                     patch{0x20DB1D, steam_sCachedParticles_va, g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},                                         // mPos.x/y (MOVQ, base)
                     patch{0x20DB27, 0x01EF5128, (0x01EF5128 - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mPos.z
                     patch{0x20DB3A, 0x01EF512C, (0x01EF512C - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor (MOVDQU, 16 bytes)
                     patch{0x20DB43, 0x01EF513C, (0x01EF513C - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mSize
                     patch{0x20DB4C, 0x01EF5140, (0x01EF5140 - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mRotation
                     // VA redirects — FlushParticleCache sort loop
                     patch{0x20DC2E, steam_sCachedParticles_va, g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},                                         // mPos.x
                     patch{0x20DC37, 0x01EF5124, (0x01EF5124 - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mPos.y
                     patch{0x20DC45, 0x01EF5128, (0x01EF5128 - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mPos.z
                     patch{0x20DC79, 0x01EF513C, (0x01EF513C - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mSize cmp
                     patch{0x20DC8E, 0x01EF5138, (0x01EF5138 - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.a fade
                     patch{0x20DC97, 0x01EF5138, (0x01EF5138 - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.a write
                     // VA redirects — FlushParticleCache render loop
                     patch{0x20DD2B, 0x01EF5138, (0x01EF5138 - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.a
                     patch{0x20DD4E, 0x01EF5134, (0x01EF5134 - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.b
                     patch{0x20DD62, 0x01EF5130, (0x01EF5130 - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.g
                     patch{0x20DD76, 0x01EF512C, (0x01EF512C - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mColor.r
                     patch{0x20DD8A, 0x01EF5140, (0x01EF5140 - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mRotation
                     patch{0x20DD99, 0x01EF513C, (0x01EF513C - steam_sCachedParticles_va) + g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},             // mSize
                     patch{0x20DDAC, steam_sCachedParticles_va, g_cachedParticles_address, {.file_offset = true, .expected_is_va = true}},                                         // mPos (SubmitParticle LEA)
                     // VA redirects — RedParticleRenderer s_caches[15] -> DLL static buffer s_caches[120]
                     // SubmitTexture: MOV EAX, &s_caches[0].m_blendMode
                     patch{0x2D26CC, 0x00969708, (0x00969708 - steam_sCaches_va) + g_sCaches_address, {.file_offset = true, .expected_is_va = true}},
                     // SubmitTexture: ADD EAX, s_caches (found-entry path)
                     patch{0x2D2706, steam_sCaches_va, g_sCaches_address, {.file_offset = true, .expected_is_va = true}},
                     // SubmitTexture: ADD EAX, s_caches (new-entry path)
                     patch{0x2D272A, steam_sCaches_va, g_sCaches_address, {.file_offset = true, .expected_is_va = true}},
                     // RenderAllCaches: MOV ESI, &s_caches[0].m_numVerts
                     patch{0x2D24DC, 0x00969710, (0x00969710 - steam_sCaches_va) + g_sCaches_address, {.file_offset = true, .expected_is_va = true}},
                  },
            },
         },
   },
};
