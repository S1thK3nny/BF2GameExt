#include "pch.h"

#include "patch_table.hpp"
#include "game_addrs.hpp"

// Matrix/Item Pool Limit Extension: redirect matrixPool to larger static buffer
// Original pool: 0x2FD80 bytes (0xBF6 entries × 64-byte matrices)
// New pool: 256× original capacity
static const uint32_t matrixPool_size = 0x2fd80 * 0x100;
static char matrixPool_storage[matrixPool_size] = {};
static const uint32_t matrixPool_address = (uint32_t)&matrixPool_storage[0];

const static uint32_t DLC_mission_size = 0x110;
const static uint32_t DLC_mission_patch_limit = 0x1000;

static char DLC_mission_table_storage[DLC_mission_size * DLC_mission_patch_limit] = {};
static const uint32_t DLC_mission_table_address = (uint32_t)&DLC_mission_table_storage[0];

// Sound limit extension: redirect smSampleRAMBitmap to larger buffer, increase malloc sizes
// Original: 32MB (0x2000000), New: 256MB (0x10000000)
// smSampleRAMBitmap: 0x8000 * 8 = 0x40000 bytes (262144)
static const uint32_t smSampleRAMBitmapNew_size = 0x8000 * 0x8;
static char smSampleRAMBitmapNew_storage[smSampleRAMBitmapNew_size] = {};
static const uint32_t smSampleRAMBitmapNew_address = (uint32_t)&smSampleRAMBitmapNew_storage[0];

// Object limit increase: EntityEx::mIdMap hash table relocation
// PblHashTable<EntityEx, 1024> uses open addressing with parallel key/value arrays.
// Layout: [4-byte header (count)] [N uint32 keys] [N uint32 values]
// Doubling from 1024 to 2048 buckets: new size = 4 + 2048*4 + 2048*4 = 0x4004 bytes
// Extra 4 bytes at [0x4004] hold the sentinel value (Entity::rttiHashEntity._uiValue).
// The game's iterator reads 1 entry past the values array and compares against this
// sentinel to detect end-of-iteration. Must be initialized at runtime before any iteration.
char EntityEx_mIdMap_new[0x4004 + 4] = {};
static const uint32_t EntityEx_mIdMap_header_addr  = (uint32_t)&EntityEx_mIdMap_new[0];
static const uint32_t EntityEx_mIdMap_table_addr   = (uint32_t)&EntityEx_mIdMap_new[4];
static const uint32_t EntityEx_mIdMap_mid_addr     = (uint32_t)&EntityEx_mIdMap_new[0x2004]; // values array start

// Particle cache increase: 300 -> 1200 entries
// CacheParticle struct is 36 (0x24) bytes: PblVector3 mPos, RedColorValue mColor, float mSize, float mRotation
static const uint32_t particle_cache_new_limit = 1200;
static char g_cachedParticles_storage[particle_cache_new_limit * 0x24] = {};
static const uint32_t g_cachedParticles_address = (uint32_t)&g_cachedParticles_storage[0];
static const uint32_t modtools_sCachedParticles_va = game_addrs::modtools::s_cached_particles;
static const uint32_t steam_sCachedParticles_va = game_addrs::steam::s_cached_particles;
static const uint32_t gog_sCachedParticles_va = game_addrs::gog::s_cached_particles;

// Combo animation increase: 30 -> 90 entries
// ComboAnimation struct is 0x24 bytes each
static char s_aComboAnimation_storage[0x24 * 90] = {};
static const uint32_t s_aComboAnimation_addr = (uint32_t)&s_aComboAnimation_storage[0];
// ComboAnimationPool: 0x4 * 256 per pool, 3 pools
static char s_aeComboAnimationPool_storage[0x4 * 256 * 3] = {};
static const uint32_t s_aeComboAnimationPool_addr = (uint32_t)&s_aeComboAnimationPool_storage[0];

// Renderer cache increase: 15 -> 60 entries
// Each RedParticleRenderer cache entry is 0x3558 bytes.
static const uint32_t renderer_cache_new_limit = 120;
char g_sCaches_storage[renderer_cache_new_limit * 0x3558] = {};
static const uint32_t g_sCaches_address = (uint32_t)&g_sCaches_storage[0];
static const uint32_t modtools_sCaches_va = game_addrs::modtools::s_caches;
static const uint32_t steam_sCaches_va = game_addrs::steam::s_caches;
static const uint32_t gog_sCaches_va = game_addrs::gog::s_caches;

// FNV-1a hash with forced lowercase — matches PblHash::calcHash in the game engine.
static uint32_t pbl_hash(const char* str)
{
   if (!str || !*str) return 0;
   uint32_t hash = 0x811c9dc5;
   while (*str) {
      hash = (hash ^ ((uint8_t)*str | 0x20)) * 0x1000193;
      str++;
   }
   return hash;
}

void init_object_limit_sentinel(const char* rtti_class_name)
{
   // The game's hash table iterator reads 1 entry past the values array and compares
   // against a RTTI hash global that sits right after mIdMap in BSS. Our relocated table
   // needs the same sentinel value placed at the overflow position (offset 0x4004).
   // We compute the hash ourselves because the RTTI global is initialized by a CRT
   // static constructor that runs AFTER our DLL init.
   // The RTTI class differs per build: modtools="Entity", Steam="EntityBuilding",
   // GOG="EntityBuildingClass" — different BSS layouts place different globals after mIdMap.
   *(uint32_t*)&EntityEx_mIdMap_new[0x4004] = pbl_hash(rtti_class_name);
}

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
               .name = "Sound Limit Extension",
               .patches =
                  {
                     patch{0x486ae0 + 0x1, 0x2331f08, smSampleRAMBitmapNew_address, {.file_offset = true, .expected_is_va = true}}, // Snd::Engine::Open smSampleRAMBitmap ptr
                     patch{0x486aea + 0x1, 0x2000000, 0x10000000, {.file_offset = true}},                                           // malloc call 1 arg: 32MB -> 256MB
                     patch{0x486939 + 0x1, 0x2000000, 0x10000000, {.file_offset = true}},                                           // malloc call 2 arg: 32MB -> 256MB
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

            patch_set{
               .name = "Object Limit Increase",
               .patches =
                  {
                     // EntityEx::mIdMap (PblHashTable<EntityEx, 1024>) relocation + bucket count doubling.
                     // Hash table at 0xb7ad38 (header) / 0xb7ad3c (keys) / 0xb7bd3c (values).
                     // Doubling: 1024 -> 2048 buckets.

                     // --- _Find tableParam: PUSH 0x800 -> PUSH 0x1000 (bucket_count * 2) ---
                     patch{0x701d5 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // EntityEx::Find
                     patch{0x70f11 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // _GetEntity<EntityGeometry>
                     patch{0x71041 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // _GetEntity<GameObject>
                     patch{0x71171 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // _GetEntity<EntityEx>
                     patch{0x713e1 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00471390
                     patch{0x71511 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_004714c0
                     patch{0x89e86 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FindRegisterStatics
                     patch{0xd04ea + 0x1, 0x800, 0x1000, {.file_offset = true}},   // EntityEx::Store
                     patch{0xd0515 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // EntityEx::Remove
                     patch{0xd0584 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // EntityEx::EntityEx
                     patch{0xd0648 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // EntityEx::~EntityEx
                     patch{0x126bb5 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_00526ba0
                     patch{0x126c25 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_00526c10
                     patch{0x1276d1 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_005276a0
                     patch{0x127cf0 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_00527ca0
                     patch{0x127d96 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_00527d60
                     patch{0x1e4a4b + 0x1, 0x800, 0x1000, {.file_offset = true}},  // IsEnabled
                     patch{0x25f084 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_0065f030
                     patch{0x25f3a0 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_0065f360
                     patch{0x265d2b + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_00665a50 (site 1)
                     patch{0x265db5 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_00665a50 (site 2)
                     patch{0x2ef87a + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_006ef870
                     patch{0x3a54c4 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_007a54b0
                     patch{0x3a5504 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_007a54b0 (second call)
                     patch{0x3a5544 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_007a5530
                     patch{0x3a5584 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_007a5530 (second call)
                     patch{0x3a7254 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_007a7200
                     patch{0x3a72e4 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_007a7290
                     patch{0x3a7374 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_007a7320
                     patch{0x3a7404 + 0x1, 0x800, 0x1000, {.file_offset = true}},  // FUN_007a73b0
                     // PblHashTable _Find internal
                     patch{0x700c8 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // PblHashTable::_Find caller
                     patch{0xd044b + 0x1, 0x800, 0x1000, {.file_offset = true}},   // Store internal
                     patch{0xd0410 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // Store internal

                     // --- Bucket count: 0x400 -> 0x800 ---
                     // Iterator / init / inline loop bounds
                     patch{0x333d40 + 0x1, 0x400, 0x800, {.file_offset = true}},   // InitFind
                     patch{0x33476f + 0x1, 0x400, 0x800, {.file_offset = true}},   // Init
                     patch{0x338eaa + 0x1, 0x400, 0x800, {.file_offset = true}},   // Init
                     patch{0x616490 + 0x1, 0x400, 0x800, {.file_offset = true}},   // FUN_00a16490
                     // PblHashTable functions
                     patch{0x89f2c + 0x2, 0x400, 0x800, {.file_offset = true}},    // _Find internal CMP
                     patch{0x89f56 + 0x3, 0x400, 0x800, {.file_offset = true}},    // _Find internal CMP
                     patch{0x894dc + 0x2, 0x400, 0x800, {.file_offset = true}},    // Iterator
                     patch{0x89476 + 0x2, 0x400, 0x800, {.file_offset = true}},    // Itor::operator*
                     patch{0x8948f + 0x2, 0x400, 0x800, {.file_offset = true}},    // Itor::operator*
                     patch{0xd03d6 + 0x1, 0x400, 0x800, {.file_offset = true}},    // Store hash mask
                     patch{0xd0696 + 0x1, 0x400, 0x800, {.file_offset = true}},    // ~EntityEx internal
                     // Inline iteration (FUN_0048e7e0 / FUN_0048eaa0)
                     patch{0x8e7fc + 0x2, 0x400, 0x800, {.file_offset = true}},    // FUN_0048e7e0 loop bound
                     patch{0x8e83b + 0x2, 0x400, 0x800, {.file_offset = true}},    // FUN_0048e7e0 loop bound
                     patch{0x8eb4c + 0x2, 0x400, 0x800, {.file_offset = true}},    // FUN_0048eaa0 loop bound
                     patch{0x8eba5 + 0x2, 0x400, 0x800, {.file_offset = true}},    // FUN_0048eaa0 loop bound
                     patch{0x8ebbc + 0x2, 0x400, 0x800, {.file_offset = true}},    // FUN_0048eaa0 loop bound

                     // --- Value array displacement: 0x1004 -> 0x2004 (4 + bucket_count * 4) ---
                     // PblHashTable iterator/accessor functions
                     patch{0x89455 + 0x3, 0x1004, 0x2004, {.file_offset = true}},  // Itor::operator*
                     patch{0x89465 + 0x3, 0x1004, 0x2004, {.file_offset = true}},  // Itor::operator_EntityEx_*
                     patch{0x8e045 + 0x3, 0x1004, 0x2004, {.file_offset = true}},  // Itor::operator->
                     // pvs::PortalReader::Read — hash table iteration
                     patch{0x8d69d + 0x3, 0x1004, 0x2004, {.file_offset = true}},  // PortalReader::Read value access
                     patch{0x8d6fd + 0x3, 0x1004, 0x2004, {.file_offset = true}},  // PortalReader::Read loop value access

                     // --- Address redirects: table base (0xb7ad3c -> new) ---
                     patch{0x701da + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // EntityEx::Find
                     patch{0x70f16 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // _GetEntity<EntityGeometry>
                     patch{0x71046 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // _GetEntity<GameObject>
                     patch{0x71176 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // _GetEntity<EntityEx>
                     patch{0x713e6 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_00471390
                     patch{0x71516 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_004714c0
                     patch{0x89e8b + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FindRegisterStatics
                     patch{0xd04ef + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // EntityEx::Store
                     patch{0xd051a + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // EntityEx::Remove
                     patch{0xd0589 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // EntityEx::EntityEx
                     patch{0xd064d + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // EntityEx::~EntityEx
                     patch{0x126bba + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00526ba0
                     patch{0x126c2a + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00526c10
                     patch{0x1276d6 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_005276a0
                     patch{0x127cf5 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00527ca0
                     patch{0x127d9b + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00527d60
                     patch{0x1e4a50 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // IsEnabled
                     patch{0x25f089 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_0065f030
                     patch{0x25f3a5 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_0065f360
                     patch{0x265d30 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00665a50 (site 1)
                     patch{0x265dba + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00665a50 (site 2)
                     patch{0x2ef87f + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_006ef870
                     patch{0x333d45 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // InitFind
                     patch{0x334774 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // Init
                     patch{0x338eaf + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // Init
                     patch{0x3a54c9 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_007a54b0
                     patch{0x3a5509 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_007a54b0
                     patch{0x3a5549 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_007a5530
                     patch{0x3a5589 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_007a5530
                     patch{0x3a7259 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_007a7200
                     patch{0x3a72e9 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_007a7290
                     patch{0x3a7379 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_007a7320
                     patch{0x3a7409 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_007a73b0
                     patch{0x616495 + 0x1, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00a16490
                     // Inline iteration SIB+disp (base address in displacement)
                     patch{0x8e7f0 + 0x3, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0048e7e0 key check
                     patch{0x8e843 + 0x3, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0048e7e0 key check
                     patch{0x8eb40 + 0x3, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0048eaa0 key check
                     patch{0x8ebb0 + 0x3, 0xb7ad3c, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0048eaa0 key check

                     // --- Address redirects: header (0xb7ad38 -> new) ---
                     patch{0x88f30 + 0x1, 0xb7ad38, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},   // GetEntityMap
                     patch{0x8d68b + 0x1, 0xb7ad38, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},   // pvs::PortalReader::Read
                     patch{0xd0500 + 0x2, 0xb7ad38, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},   // EntityEx::Store
                     patch{0xd052b + 0x2, 0xb7ad38, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},   // EntityEx::Remove
                     patch{0xd059a + 0x2, 0xb7ad38, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},   // EntityEx::EntityEx
                     patch{0xd065e + 0x2, 0xb7ad38, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},   // EntityEx::~EntityEx
                     patch{0x333d52 + 0x2, 0xb7ad38, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},  // InitFind
                     patch{0x33477e + 0x2, 0xb7ad38, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},  // Init
                     patch{0x338ec3 + 0x2, 0xb7ad38, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},  // Init
                     patch{0x6164a2 + 0x2, 0xb7ad38, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00a16490

                     // --- Address redirects: mid/values (0xb7bd3c -> new) ---
                     // These are in inline iteration code using SIB+displacement to access values array directly
                     patch{0x8e804 + 0x3, 0xb7bd3c, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0048e7e0 value read
                     patch{0x8e822 + 0x3, 0xb7bd3c, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0048e7e0 value read
                     patch{0x8e857 + 0x3, 0xb7bd3c, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0048e7e0 value read
                     patch{0x8eb54 + 0x3, 0xb7bd3c, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0048eaa0 value read
                     patch{0x8eb79 + 0x3, 0xb7bd3c, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0048eaa0 value read
                     patch{0x8ebc4 + 0x3, 0xb7bd3c, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0048eaa0 value read
                  },
            },

            patch_set{
               .name = "Combo Anims Increase",
               .patches =
                  {
                     // Combo animation array redirect: 30 -> 90 entries (0x24 bytes each)
                     patch{0x170467 + 0x3, 0xb8c620, s_aComboAnimation_addr, {.file_offset = true, .expected_is_va = true}},        // _GetComboAnimation
                     patch{0x1709b1 + 0x1, 0xb8c640, s_aComboAnimation_addr + 0x20, {.file_offset = true, .expected_is_va = true}}, // FindComboAnimation

                     // Combo limit: 0x1E (30) -> 0x5A (90)
                     patch{0x170a65 + 0x2, 0x1e, 0x5a, {.file_offset = true, .values_are_8bit = true}}, // AddComboAnimation
                     patch{0x188a40 + 0x2, 0x1e, 0x5a, {.file_offset = true, .values_are_8bit = true}}, // IsWeaponMeleeAnimIndex

                     // ComboAnimationPool redirect + pool size (0x100 -> 0x300)
                     patch{0x170a2b + 0x3, 0xb8cc80, s_aeComboAnimationPool_addr, {.file_offset = true, .expected_is_va = true}}, // AddComboAnimation
                     patch{0x170b31 + 0x3, 0xb8cc80, s_aeComboAnimationPool_addr, {.file_offset = true, .expected_is_va = true}}, // GetComboAnimationIndex
                     patch{0x170a22 + 0x1, 0x100, 0x300, {.file_offset = true}}, // AddComboAnimation pool size
                     patch{0x170b27 + 0x2, 0x100, 0x300, {.file_offset = true}}, // GetComboAnimationIndex pool size

                     // Animation name table upper limit
                     patch{0x1722e8 + 0x1, 0x148, 0x1fc, {.file_offset = true}}, // s_pAnimationNameTable upper limit

                     // SoldierAnimationData struct size: 0xf60 -> 0x17d0
                     patch{0x1737be + 0x1, 0xf60, 0x17d0, {.file_offset = true}}, // InitAnimationData
                     patch{0x1739a6 + 0x2, 0xf60, 0x17d0, {.file_offset = true}}, // InitAnimationData

                     // Anim index limit: 0xA4 (164) -> 0xFE (254)
                     patch{0x188b06 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // GetAnimFromAnimIndex
                     patch{0x178175 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SoldierAnimator ctor
                     patch{0x17ad35 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetNewOwner
                     patch{0x17b02c + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateActionAnimation
                     patch{0x17b13a + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateActionAnimation
                     patch{0x17b1ca + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateActionAnimation
                     patch{0x17b9d1 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateMovementAnimation
                     patch{0x17baaf + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateMovementAnimation
                     patch{0x17bc81 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateMovementAnimation
                     patch{0x17bc89 + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateMovementAnimation
                     patch{0x17ccc3 + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetupPose
                     patch{0x187951 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // LowResClass::PostLoad
                     patch{0x187a36 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // LowResClass::PostLoad
                     patch{0x18788a + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // LowResClass::PostLoad
                     patch{0x176a3f + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponAnimationMap
                     patch{0x176c60 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponComboState
                     patch{0x176c62 + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponComboState
                     patch{0x176c84 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponComboState
                     patch{0x176c97 + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponComboState
                     // EntitySoldier::Render (4-byte, NOT 8-bit)
                     patch{0x136d47 + 0x1, 0xa4, 0xfe, {.file_offset = true}}, // Render
                     patch{0x136d4c + 0x1, 0xa4, 0xfe, {.file_offset = true}}, // Render
                     patch{0x136c99 + 0x1, 0xa4, 0xfe, {.file_offset = true}}, // Render
                     patch{0x136c54 + 0x1, 0xa4, 0xfe, {.file_offset = true}}, // Render
                     // FUN_005f* and FUN_006009* animation functions
                     patch{0x1f5cf7 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // FUN_005f5bb0
                     patch{0x1f6c63 + 0x3, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // FUN_005f6b20
                     patch{0x1f7754 + 0x3, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // FUN_005f7600
                     patch{0x200af3 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // FUN_00600990
                     patch{0x1f6094 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // FUN_005f6090
                     // g_fnAnim_Data
                     patch{0x1778b3 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // g_fnAnim_Data (8-bit)
                     patch{0x177d4a + 0x1, 0xa4, 0xfe, {.file_offset = true}},                          // g_fnAnim_Data (4-byte)
                  },
            },

            patch_set{
               .name = "High-Res Animation Limit",
               .patches =
                  {
                     // SoldierAnimatorHighResClass::PostLoad — increase capacity 50 -> 12800
                     // Modtools uses 32-bit immediates so no code cave needed
                     patch{0x1840c7 + 0x2, 0x32, 0x3200, {.file_offset = true}},                              // MOV [EAX], count
                     patch{0x1840cf + 0x1, 0x32, 0x3200, {.file_offset = true}},                              // MOV EDI, count
                     patch{0x184136 + 0x2, 0x64960, 0x3200 * 0x2030, {.file_offset = true}},                  // CMP EDI, array_size
                     patch{0x17e57e + 0x2, 0x64960, 0x3200 * 0x2030, {.file_offset = true}},                  // CMP EDX, array_size
                     patch{0x1840b3 + 0x1, 0x64970, 0x3200 * 0x2030 + 0x10, {.file_offset = true}},          // PUSH heap_alloc_size
                  },
            },

            patch_set{
               .name = "Network Timer Increase",
               .patches =
                  {
                     // TTYScroll: Timer 2 (FrameUpdate::Update) divisor 30 -> 120 Hz
                     // PUSH imm8 operand at 0x00449b5b (VA)
                     patch{0x00449b5b, 0x1e, 0x78, {.values_are_8bit = true}}, // Timer 2: 30 Hz -> 120 Hz
                  },
            },

            patch_set{
               .name = "Chunk Push Fix",
               .patches =
                  {
                     // ApplyRadiusPush: remove early return when ChunkFrequency triggers.
                     // Vanilla skips push entirely when chunk flag is set — replace
                     // POP ESI; ADD ESP,0x30 with JMP +0x2C to push calculation.
                     // Bytes: 5E 83 C4 30 -> EB 2C 90 90
                     patch{0x0052bfa1, 0x30C4835E, 0x90902CEB},
                  },
            },

            patch_set{
               .name = "Matrix/Item Pool Limit Extension",
               .patches =
                  {
                     // matrixPool address redirects
                     patch{0x405c0f + 0x2, 0xd64090, matrixPool_address, {.file_offset = true, .expected_is_va = true}},
                     patch{0x405c83 + 0x2, 0xd64090, matrixPool_address, {.file_offset = true, .expected_is_va = true}},
                     patch{0x410747 + 0x1, 0xd64090, matrixPool_address, {.file_offset = true, .expected_is_va = true}},
                     // matrixPool size
                     patch{0x405c15 + 0x2, 0xbf6, matrixPool_size, {.file_offset = true}},
                     patch{0x405c89 + 0x2, 0xbf6, matrixPool_size, {.file_offset = true}},
                     // transparentItemsSize: 800 -> 204800
                     patch{0x61f8b0 + 0x1, 0x320, 0x32000, {.file_offset = true}},
                     // postTransparentItemSize: 512 -> 131072
                     patch{0x61f8e0 + 0x1, 0x200, 0x20000, {.file_offset = true}},
                     // preShadowTransparentItemSize code cave: PUSH 100 -> PUSH 25600
                     patch{0x61f880,       0x6a, 0xeb, {.file_offset = true, .values_are_8bit = true}},       // JMP +0x21
                     patch{0x61f880 + 0x1, 0x64, 0x21, {.file_offset = true, .values_are_8bit = true}},       // JMP offset
                     patch{0x61f8a3,       0xcc, 0x68, {.file_offset = true, .values_are_8bit = true}},       // PUSH imm32 opcode
                     patch{0x61f8a3 + 0x1, 0xcccccccc, 0x6400, {.file_offset = true}},                        // PUSH 0x6400
                     patch{0x61f8a8,       0xcc, 0xeb, {.file_offset = true, .values_are_8bit = true}},       // JMP short back
                     patch{0x61f8a8 + 0x1, 0xcc, 0xd8, {.file_offset = true, .values_are_8bit = true}},       // JMP offset (-0x28)
                  },
            },

            patch_set{
               .name = "String Pool Increase",
               .patches =
                  {
                     patch{0x4ef77 + 0x1, 0x8000, 0x20000, {.file_offset = true}}, // 32KB -> 128KB
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
               .name = "Sound Limit Extension",
               .patches =
                  {
                     patch{0x332aa2 + 0x1, 0x9d1258, smSampleRAMBitmapNew_address, {.file_offset = true, .expected_is_va = true}}, // Snd::Engine::Open smSampleRAMBitmap ptr
                     patch{0x332aac + 0x1, 0x2000000, 0x10000000, {.file_offset = true}},                                          // malloc call 1 arg: 32MB -> 256MB
                     patch{0x3328e7 + 0x1, 0x2000000, 0x10000000, {.file_offset = true}},                                          // malloc call 2 arg: 32MB -> 256MB
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

            patch_set{
               .name = "Object Limit Increase",
               .patches =
                  {
                     // GOG EntityEx::mIdMap (PblHashTable<EntityEx, 1024>) relocation + bucket count doubling.
                     // Hash table: header 0x1EBAD20, keys 0x1EBAD24, values 0x1EBBD24.
                     // file_offset = VA - 0x400C00 (.text: PointerToRawData=0x400, VirtualAddress=0x1000)
                     // Doubling: 1024 -> 2048 buckets.

                     // --- _Find/_Store tableParam: PUSH 0x800 -> PUSH 0x1000 (bucket_count * 2) ---
                     patch{0x6CF31 + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_0046DB30 (_Find wrapper)
                     patch{0x6CF85 + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_0046DB50 (_Find entity class cache)
                     patch{0x90B9E + 0x1, 0x800, 0x1000, {.file_offset = true}},    // EntityEx ctor (_Store)
                     patch{0x90C80 + 0x1, 0x800, 0x1000, {.file_offset = true}},    // ~EntityEx dtor (_Remove)
                     patch{0xD16DB + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_004D22C0 (_Find flag check)
                     patch{0xDBF43 + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_004DCB30 (_Find cached A)
                     patch{0xDBFA3 + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_004DCB90 (_Find cached B)
                     patch{0xDCAE9 + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_004DD6A0 (_Find chained A)
                     patch{0xDCB7A + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_004DD740 (_Find chained B)
                     patch{0x113BD1 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00514780 (Lua entity resolve)
                     patch{0x113C41 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_005147F0 (Lua entity resolve)
                     patch{0x113CB1 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00514860 (Lua entity resolve)
                     patch{0x113D21 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_005148D0 (Lua entity resolve)
                     patch{0x191438 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00591FF0 (entity resolve)
                     patch{0x1914C9 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00592080 (entity resolve)
                     patch{0x191558 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00592110 (entity resolve)
                     patch{0x1916C9 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00592280 (entity resolve)
                     patch{0x191759 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00592310 (entity resolve)
                     patch{0x2215F2 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_006221C0 (iteration + entity resolve)
                     patch{0x24C858 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_0064D430 (sound/effect entity lookup)
                     patch{0x24C97F + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_0064D540 (sound/effect team lookup)
                     patch{0x26FA90 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00670410 (ordnance/projectile 1st)
                     patch{0x26FB46 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00670410 (ordnance/projectile 2nd)

                     // --- Bucket count: 0x400 -> 0x800 ---
                     patch{0x1C60 + 0x1, 0x400, 0x800, {.file_offset = true}},      // standalone init PUSH
                     patch{0x13179C + 0x1, 0x400, 0x800, {.file_offset = true}},    // level init (FUN_00531C40) PUSH
                     patch{0x236225 + 0x1, 0x400, 0x800, {.file_offset = true}},    // game init (FUN_00636E10) PUSH
                     // Inline iteration (FUN_006532B0)
                     patch{0x2526DB + 0x2, 0x400, 0x800, {.file_offset = true}},    // Begin scan bound CMP ESI
                     patch{0x25271A + 0x2, 0x400, 0x800, {.file_offset = true}},    // operator++ bound CMP ESI
                     patch{0x252739 + 0x2, 0x400, 0x800, {.file_offset = true}},    // operator++ inner CMP ESI
                     // Inline iteration (FUN_00653740)
                     patch{0x252BF0 + 0x2, 0x400, 0x800, {.file_offset = true}},    // Begin scan bound CMP ESI
                     patch{0x252C4C + 0x2, 0x400, 0x800, {.file_offset = true}},    // operator++ bound CMP ESI
                     patch{0x252C69 + 0x2, 0x400, 0x800, {.file_offset = true}},    // operator++ inner CMP ESI
                     // PblHashTable Begin/operator++ (FUN_00623510 / FUN_00623550)
                     patch{0x22292F + 0x2, 0x400, 0x800, {.file_offset = true}},    // Begin CMP ECX
                     patch{0x222956 + 0x1, 0x400, 0x800, {.file_offset = true}},    // operator++ CMP EAX
                     patch{0x22296B + 0x1, 0x400, 0x800, {.file_offset = true}},    // operator++ inner CMP EAX

                     // --- Value array displacement: 0x1004 -> 0x2004 (4 + bucket_count * 4) ---
                     patch{0x222792 + 0x3, 0x1004, 0x2004, {.file_offset = true}},  // Itor dereference (Read)
                     patch{0x2227EB + 0x3, 0x1004, 0x2004, {.file_offset = true}},  // Itor dereference 2nd (Read)

                     // --- Address redirects: table base (0x1EBAD24 -> new) ---
                     patch{0x1C65 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},    // standalone init PUSH
                     patch{0x6CF36 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0046DB30
                     patch{0x6CF8A + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0046DB50
                     patch{0x90BA3 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // EntityEx ctor
                     patch{0x90C85 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // ~EntityEx dtor
                     patch{0xD16E0 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_004D22C0
                     patch{0xDBF48 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_004DCB30
                     patch{0xDBFA8 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_004DCBA8 [sic, 004DCB90]
                     patch{0xDCAEE + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_004DD6A0
                     patch{0xDCB7F + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_004DD740
                     patch{0x113BD6 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00514780
                     patch{0x113C46 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_005147F0
                     patch{0x113CB6 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00514860
                     patch{0x113D26 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_005148D0
                     patch{0x1317A1 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // level init PUSH
                     patch{0x19143D + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00591FF0
                     patch{0x1914CE + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00592080
                     patch{0x19155D + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00592110
                     patch{0x1916CE + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00592280
                     patch{0x19175E + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00592310
                     patch{0x2215F7 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_006221C0
                     patch{0x23622A + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // game init PUSH
                     patch{0x24C85D + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_0064D430
                     patch{0x24C984 + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_0064D540
                     patch{0x26FA9A + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00670410 (1st)
                     patch{0x26FB4B + 0x1, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00670410 (2nd)
                     // SIB+disp inline iteration (FUN_006532B0 / FUN_00653740)
                     patch{0x2526D0 + 0x3, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // CMP key scan
                     patch{0x252722 + 0x3, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // LEA key addr
                     patch{0x252BE5 + 0x3, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // CMP key scan
                     patch{0x252C54 + 0x3, 0x1EBAD24, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // LEA key addr

                     // --- Address redirects: header (0x1EBAD20 -> new) ---
                     patch{0x1C72 + 0x2, 0x1EBAD20, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},   // standalone init MOV [imm32], 0
                     patch{0x90BB6 + 0x2, 0x1EBAD20, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},  // EntityEx ctor INC
                     patch{0x90C96 + 0x2, 0x1EBAD20, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},  // ~EntityEx dtor DEC
                     patch{0x1317AB + 0x2, 0x1EBAD20, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}}, // level init MOV [imm32], 0
                     patch{0x222781 + 0x1, 0x1EBAD20, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}}, // Read: MOV ECX, imm32
                     patch{0x236237 + 0x2, 0x1EBAD20, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}}, // game init MOV [imm32], 0

                     // --- Address redirects: mid/values (0x1EBBD24 -> new) ---
                     // Inline iteration SIB+displacement (FUN_006532B0 / FUN_00653740)
                     patch{0x2526E3 + 0x3, 0x1EBBD24, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},    // FUN_006532B0 value read
                     patch{0x2526FE + 0x3, 0x1EBBD24, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},    // FUN_006532B0 value read
                     patch{0x252741 + 0x3, 0x1EBBD24, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},    // FUN_006532B0 value read
                     patch{0x252BF8 + 0x3, 0x1EBBD24, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},    // FUN_00653740 value read
                     patch{0x252C11 + 0x3, 0x1EBBD24, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},    // FUN_00653740 value read
                     patch{0x252C71 + 0x3, 0x1EBBD24, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},    // FUN_00653740 value read
                  },
            },

            patch_set{
               .name = "Combo Anims Increase",
               .patches =
                  {
                     // GOG combo animation array redirect: 30 -> 90 entries
                     // GOG aComboAnimation = 0x1EB0610, aeComboAnimationPool = 0x1EB0BB8
                     // GOG .text offset from Steam: +0x10A0 for 0x63xxxx-0x64xxxx region, 0 for lower addresses
                     patch{0x23C8C3 + 0x3, 0x1eb0610, s_aComboAnimation_addr, {.file_offset = true, .expected_is_va = true}},        // _GetComboAnimation
                     patch{0x23D0AD + 0x1, 0x1eb0630, s_aComboAnimation_addr + 0x20, {.file_offset = true, .expected_is_va = true}}, // FindComboAnimation

                     // Combo limit: 0x1E (30) -> 0x5A (90)
                     patch{0x23D170 + 0x2, 0x1e, 0x5a, {.file_offset = true, .values_are_8bit = true}}, // AddComboAnimation
                     patch{0x24A90D + 0x2, 0x1e, 0x5a, {.file_offset = true, .values_are_8bit = true}}, // IsWeaponMeleeAnimIndex

                     // ComboAnimationPool redirect + pool size (0x100 -> 0x300)
                     patch{0x23D13F + 0x3, 0x1eb0bb8, s_aeComboAnimationPool_addr, {.file_offset = true, .expected_is_va = true}}, // AddComboAnimation
                     patch{0x23D20B + 0x3, 0x1eb0bb8, s_aeComboAnimationPool_addr, {.file_offset = true, .expected_is_va = true}}, // GetComboAnimationIndex
                     patch{0x23D120 + 0x2, 0x100, 0x300, {.file_offset = true}}, // AddComboAnimation pool size
                     patch{0x23D1F2 + 0x2, 0x100, 0x300, {.file_offset = true}}, // GetComboAnimationIndex pool size

                     // Animation name table upper limit
                     patch{0x23E897 + 0x1, 0x148, 0x1fc, {.file_offset = true}}, // s_pAnimationNameTable upper limit

                     // SoldierAnimationData struct size
                     patch{0x23E21B + 0x1, 0xf60, 0x17d0, {.file_offset = true}}, // InitAnimationData
                     patch{0x23E38C + 0x2, 0xa4, 0xfe, {.file_offset = true}},    // InitAnimationData (4-byte 0xA4->0xFE)

                     // Anim index limit: 0xA4 (164) -> 0xFE (254)
                     patch{0x24A9A5 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // GetAnimFromAnimIndex
                     patch{0x23ECD4 + 0x7, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SoldierAnimator ctor (byte 1)
                     patch{0x23ECD4 + 0x8, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SoldierAnimator ctor (byte 2)
                     patch{0x23EF3F + 0x7, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetNewOwner (byte 1)
                     patch{0x23EF3F + 0x8, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetNewOwner (byte 2)
                     patch{0x240D87 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateActionAnimation
                     patch{0x240EA2 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateActionAnimation
                     patch{0x240F2B + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateActionAnimation
                     patch{0x241642 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateMovementAnimation
                     patch{0x241715 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateMovementAnimation
                     patch{0x241905 + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateMovementAnimation
                     patch{0x24190C + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateMovementAnimation
                     patch{0x240372 + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetupPose
                     patch{0x248D23 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // LowResClass::PostLoad
                     patch{0x248DF6 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // LowResClass::PostLoad
                     patch{0x248C60 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // LowResClass::PostLoad
                     patch{0x23FC77 + 0x7, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponAnimationMap (byte 1)
                     patch{0x23FC77 + 0x8, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponAnimationMap (byte 2)
                     patch{0x23FE76 + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponComboState
                     patch{0x23FE7D + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponComboState
                     patch{0x23FE9E + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponComboState
                     patch{0x23FEAF + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponComboState
                     // EntitySoldier::Render (4-byte, NOT 8-bit) — same file offsets as Steam (offset 0 in this region)
                     patch{0xe2838 + 0x1, 0xa4, 0xfe, {.file_offset = true}}, // Render
                     patch{0xe283d + 0x1, 0xa4, 0xfe, {.file_offset = true}}, // Render
                     patch{0xe2778 + 0x1, 0xa4, 0xfe, {.file_offset = true}}, // Render
                     patch{0xe274a + 0x1, 0xa4, 0xfe, {.file_offset = true}}, // Render
                     // Combo::ResolveForWeapon + DeflectAnimation — same file offsets as Steam (offset 0)
                     patch{0x74B82 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // ResolveForWeapon (CMP AL, 0xA4)
                     patch{0x72BD9 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // DeflectAnimation (CMP CL, 0xA4)
                  },
            },

            patch_set{
               .name = "High-Res Animation Limit",
               .patches =
                  {
                     // SoldierAnimatorHighResClass::PostLoad — increase capacity 50 -> 335
                     // GOG retail uses PUSH imm8 (6A 32) — need code cave to widen to PUSH imm32
                     // Code cave: JMP to INT3 padding, PUSH 0x14F, LEA ESI,[EAX+0x10], JMP back
                     // All file offsets = Steam + 0x10A0 (verified via Ghidra)
                     patch{0x247872,     0x6a,        0xe9,        {.file_offset = true, .values_are_8bit = true}}, // JMP to code cave
                     patch{0x247872 + 1, 0x10708d32,  0x8c1,       {.file_offset = true}},                          // JMP displacement
                     patch{0x248138,     0xcc,        0x68,        {.file_offset = true, .values_are_8bit = true}}, // PUSH imm32 opcode
                     patch{0x248138 + 1, 0xcccccccc,  0x14f,       {.file_offset = true}},                          // PUSH 0x14F
                     patch{0x24813d,     0xcc,        0xeb,        {.file_offset = true, .values_are_8bit = true}}, // JMP short opcode
                     patch{0x24813d + 1, 0xcc,        0x05,        {.file_offset = true, .values_are_8bit = true}}, // JMP short +5
                     patch{0x248144,     0xcc,        0x8d,        {.file_offset = true, .values_are_8bit = true}}, // LEA ESI,[EAX+0x10] byte 1
                     patch{0x248144 + 1, 0xcc,        0x70,        {.file_offset = true, .values_are_8bit = true}}, // LEA ESI,[EAX+0x10] byte 2
                     patch{0x248144 + 2, 0xcc,        0x10,        {.file_offset = true, .values_are_8bit = true}}, // LEA ESI,[EAX+0x10] byte 3
                     patch{0x248147,     0xcc,        0xe9,        {.file_offset = true, .values_are_8bit = true}}, // JMP back opcode
                     patch{0x248147 + 1, 0xcccccccc,  0xfffff72b,  {.file_offset = true}},                          // JMP back displacement
                     // Standard value patches
                     patch{0x247877 + 0x2, 0x32,    0x14f,              {.file_offset = true}}, // MOV [EAX], count
                     patch{0x2478D9 + 0x2, 0x64640, 0x14f * 0x2020,     {.file_offset = true}}, // CMP EDI, array_size
                     patch{0x243D02 + 0x1, 0x64640, 0x14f * 0x2020,     {.file_offset = true}}, // CMP EAX, array_size
                     patch{0x247850 + 0x1, 0x64650, 0x14f * 0x2020 + 0x10, {.file_offset = true}}, // PUSH heap_alloc_size
                  },
            },

            patch_set{
               .name = "Network Timer Increase",
               .patches =
                  {
                     // TTYScroll: Timer 2 (FrameUpdate::Update) divisor 30 -> 120 Hz
                     // PUSH imm8 operand at 0x0052d4c2 (VA) — same address as Steam
                     patch{0x0052d4c2, 0x1e, 0x78, {.values_are_8bit = true}}, // Timer 2: 30 Hz -> 120 Hz
                  },
            },

            patch_set{
               .name = "Chunk Push Fix",
               .patches =
                  {
                     // ApplyRadiusPush: remove early return when ChunkFrequency triggers.
                     // Vanilla skips push entirely when chunk flag is set — replace
                     // POP ESI; MOV ESP,EBP with JMP +0x25 to push calculation.
                     // Bytes: 5E 8B E5 5D -> EB 25 90 90
                     patch{0x004e1a24, 0x5DE58B5E, 0x909025EB},
                  },
            },

            patch_set{
               .name = "Matrix/Item Pool Limit Extension",
               .patches =
                  {
                     // GOG addresses not yet identified — empty patch set
                  },
            },

            patch_set{
               .name = "String Pool Increase",
               .patches =
                  {
                     patch{0x13b293 + 0x1, 0x1770, 0x20000, {.file_offset = true}}, // 6000 -> 128KB
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
               .name = "Sound Limit Extension",
               .patches =
                  {
                     patch{0x3319b2 + 0x1, 0x9cfdb8, smSampleRAMBitmapNew_address, {.file_offset = true, .expected_is_va = true}}, // Snd::Engine::Open smSampleRAMBitmap ptr
                     patch{0x3319bc + 0x1, 0x2000000, 0x10000000, {.file_offset = true}},                                          // malloc call 1 arg: 32MB -> 256MB
                     patch{0x3317f7 + 0x1, 0x2000000, 0x10000000, {.file_offset = true}},                                          // malloc call 2 arg: 32MB -> 256MB
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

            patch_set{
               .name = "Object Limit Increase",
               .patches =
                  {
                     // Steam EntityEx::mIdMap (PblHashTable<EntityEx, 1024>) relocation + bucket count doubling.
                     // Hash table: header 0x1EB9870, keys 0x1EB9874, values 0x1EBA874.
                     // file_offset = VA - 0x400C00 (.text: PointerToRawData=0x400, VirtualAddress=0x1000)
                     // Doubling: 1024 -> 2048 buckets.

                     // --- _Find/_Store tableParam: PUSH 0x800 -> PUSH 0x1000 (bucket_count * 2) ---
                     patch{0x6CF31 + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_0046DB30 (_Find wrapper)
                     patch{0x6CF85 + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_0046DB50 (_Find entity class cache)
                     patch{0x90B9E + 0x1, 0x800, 0x1000, {.file_offset = true}},    // EntityEx ctor (_Store)
                     patch{0x90C80 + 0x1, 0x800, 0x1000, {.file_offset = true}},    // ~EntityEx dtor (_Remove)
                     patch{0xD16DB + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_004D22C0 (_Find flag check)
                     patch{0xDBF43 + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_004DCB30 (_Find cached A)
                     patch{0xDBFA3 + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_004DCBA3 [sic, 004DCB90] (_Find cached B)
                     patch{0xDCAE9 + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_004DD6A0 (_Find chained A)
                     patch{0xDCB7A + 0x1, 0x800, 0x1000, {.file_offset = true}},    // FUN_004DD740 (_Find chained B)
                     patch{0x113BD1 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00514780 (Lua entity resolve)
                     patch{0x113C41 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_005147F0 (Lua entity resolve)
                     patch{0x113CB1 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00514860 (Lua entity resolve)
                     patch{0x113D21 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_005148D0 (Lua entity resolve)
                     patch{0x190498 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00591050 (entity resolve)
                     patch{0x190529 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // _GetEntity<GameObject> (entity resolve)
                     patch{0x1905B8 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // _GetEntity<EntityEx> (entity resolve)
                     patch{0x190729 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_005912E0 (entity resolve)
                     patch{0x1907B9 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00591370 (entity resolve)
                     patch{0x220562 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_00621130 (iteration + entity resolve)
                     patch{0x24B7B8 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_0064C390 (sound/effect entity lookup)
                     patch{0x24B8DF + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_0064C4A0 (sound/effect team lookup)
                     patch{0x26E9F0 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_0066F370 (ordnance/projectile 1st)
                     patch{0x26EAA6 + 0x1, 0x800, 0x1000, {.file_offset = true}},   // FUN_0066F370 (ordnance/projectile 2nd)

                     // --- Bucket count: 0x400 -> 0x800 ---
                     patch{0x1C60 + 0x1, 0x400, 0x800, {.file_offset = true}},      // standalone init PUSH
                     patch{0x130A3C + 0x1, 0x400, 0x800, {.file_offset = true}},    // level init (Init) PUSH
                     patch{0x235185 + 0x1, 0x400, 0x800, {.file_offset = true}},    // game init (Init) PUSH
                     // Inline iteration (FUN_00652210)
                     patch{0x25163B + 0x2, 0x400, 0x800, {.file_offset = true}},    // Begin scan bound CMP ESI
                     patch{0x25167A + 0x2, 0x400, 0x800, {.file_offset = true}},    // operator++ bound CMP ESI
                     patch{0x251699 + 0x2, 0x400, 0x800, {.file_offset = true}},    // operator++ inner CMP ESI
                     // Inline iteration (FUN_006526A0)
                     patch{0x251B50 + 0x2, 0x400, 0x800, {.file_offset = true}},    // Begin scan bound CMP ESI
                     patch{0x251BAC + 0x2, 0x400, 0x800, {.file_offset = true}},    // operator++ bound CMP ESI
                     patch{0x251BC9 + 0x2, 0x400, 0x800, {.file_offset = true}},    // operator++ inner CMP ESI
                     // PblHashTable Begin/operator++ (FUN_00622480 / FUN_006224C0)
                     patch{0x22189F + 0x2, 0x400, 0x800, {.file_offset = true}},    // Begin CMP ECX
                     patch{0x2218C6 + 0x1, 0x400, 0x800, {.file_offset = true}},    // operator++ CMP EAX
                     patch{0x2218DB + 0x1, 0x400, 0x800, {.file_offset = true}},    // operator++ inner CMP EAX

                     // --- Value array displacement: 0x1004 -> 0x2004 (4 + bucket_count * 4) ---
                     patch{0x221702 + 0x3, 0x1004, 0x2004, {.file_offset = true}},  // Itor dereference (Read)
                     patch{0x22175B + 0x3, 0x1004, 0x2004, {.file_offset = true}},  // Itor dereference 2nd (Read)

                     // --- Address redirects: table base (0x1EB9874 -> new) ---
                     patch{0x1C65 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},    // standalone init PUSH
                     patch{0x6CF36 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0046DB30
                     patch{0x6CF8A + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_0046DB50
                     patch{0x90BA3 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // EntityEx ctor
                     patch{0x90C85 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // ~EntityEx dtor
                     patch{0xD16E0 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_004D22C0
                     patch{0xDBF48 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_004DCB30
                     patch{0xDBFA8 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_004DCB90
                     patch{0xDCAEE + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_004DD6A0
                     patch{0xDCB7F + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},   // FUN_004DD740
                     patch{0x113BD6 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00514780
                     patch{0x113C46 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_005147F0
                     patch{0x113CB6 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00514860
                     patch{0x113D26 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_005148D0
                     patch{0x130A41 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // level init PUSH
                     patch{0x19049D + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00591050
                     patch{0x19052E + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // _GetEntity<GameObject>
                     patch{0x1905BD + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // _GetEntity<EntityEx>
                     patch{0x19072E + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_005912E0
                     patch{0x1907BE + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00591370
                     patch{0x220567 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_00621130
                     patch{0x23518A + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // game init PUSH
                     patch{0x24B7BD + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_0064C390
                     patch{0x24B8E4 + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_0064C4A0
                     patch{0x26E9FA + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_0066F370 (1st)
                     patch{0x26EAAB + 0x1, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_0066F370 (2nd)
                     // SIB+disp inline iteration (FUN_00652210 / FUN_006526A0)
                     patch{0x251630 + 0x3, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // CMP key scan
                     patch{0x251682 + 0x3, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // LEA key addr
                     patch{0x251B45 + 0x3, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // CMP key scan
                     patch{0x251BB4 + 0x3, 0x1EB9874, EntityEx_mIdMap_table_addr, {.file_offset = true, .expected_is_va = true}},  // LEA key addr

                     // --- Address redirects: header (0x1EB9870 -> new) ---
                     patch{0x1C72 + 0x2, 0x1EB9870, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},   // standalone init MOV [imm32], 0
                     patch{0x90BB6 + 0x2, 0x1EB9870, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},  // EntityEx ctor INC
                     patch{0x90C96 + 0x2, 0x1EB9870, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}},  // ~EntityEx dtor DEC
                     patch{0x130A4B + 0x2, 0x1EB9870, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}}, // level init MOV [imm32], 0
                     patch{0x2216F1 + 0x1, 0x1EB9870, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}}, // Read: MOV ECX, imm32
                     patch{0x235197 + 0x2, 0x1EB9870, EntityEx_mIdMap_header_addr, {.file_offset = true, .expected_is_va = true}}, // game init MOV [imm32], 0

                     // --- Address redirects: mid/values (0x1EBA874 -> new) ---
                     // Inline iteration SIB+displacement (FUN_00652210 / FUN_006526A0)
                     patch{0x251643 + 0x3, 0x1EBA874, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},    // FUN_00652210 value read
                     patch{0x25165E + 0x3, 0x1EBA874, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},    // FUN_00652210 value read
                     patch{0x2516A1 + 0x3, 0x1EBA874, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},    // FUN_00652210 value read
                     patch{0x251B58 + 0x3, 0x1EBA874, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},    // FUN_006526A0 value read
                     patch{0x251B71 + 0x3, 0x1EBA874, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},    // FUN_006526A0 value read
                     patch{0x251BD1 + 0x3, 0x1EBA874, EntityEx_mIdMap_mid_addr, {.file_offset = true, .expected_is_va = true}},    // FUN_006526A0 value read
                  },
            },

            patch_set{
               .name = "Combo Anims Increase",
               .patches =
                  {
                     // Steam combo animation array redirect: 30 -> 90 entries
                     // file_offset = VA - 0x400C00 (.text: PointerToRawData=0x400, VirtualAddress=0x1000)
                     patch{0x23b823 + 0x3, 0x1eaf0a0, s_aComboAnimation_addr, {.file_offset = true, .expected_is_va = true}},        // _GetComboAnimation
                     patch{0x23c00d + 0x1, 0x1eaf0c0, s_aComboAnimation_addr + 0x20, {.file_offset = true, .expected_is_va = true}}, // FindComboAnimation

                     // Combo limit: 0x1E (30) -> 0x5A (90)
                     patch{0x23c0d0 + 0x2, 0x1e, 0x5a, {.file_offset = true, .values_are_8bit = true}}, // AddComboAnimation
                     patch{0x24986d + 0x2, 0x1e, 0x5a, {.file_offset = true, .values_are_8bit = true}}, // IsWeaponMeleeAnimIndex

                     // ComboAnimationPool redirect + pool size (0x100 -> 0x300)
                     patch{0x23c09f + 0x3, 0x1eaf710, s_aeComboAnimationPool_addr, {.file_offset = true, .expected_is_va = true}}, // AddComboAnimation
                     patch{0x23c16b + 0x3, 0x1eaf710, s_aeComboAnimationPool_addr, {.file_offset = true, .expected_is_va = true}}, // GetComboAnimationIndex
                     patch{0x23c080 + 0x2, 0x100, 0x300, {.file_offset = true}}, // AddComboAnimation pool size
                     patch{0x23c152 + 0x2, 0x100, 0x300, {.file_offset = true}}, // GetComboAnimationIndex pool size

                     // Animation name table upper limit
                     patch{0x23d7f7 + 0x1, 0x148, 0x1fc, {.file_offset = true}}, // s_pAnimationNameTable upper limit

                     // SoldierAnimationData struct size
                     patch{0x23d17b + 0x1, 0xf60, 0x17d0, {.file_offset = true}}, // InitAnimationData
                     patch{0x23d2ec + 0x2, 0xa4, 0xfe, {.file_offset = true}},    // InitAnimationData (4-byte 0xA4->0xFE)

                     // Anim index limit: 0xA4 (164) -> 0xFE (254)
                     patch{0x249905 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // GetAnimFromAnimIndex
                     patch{0x23dc34 + 0x7, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SoldierAnimator ctor (byte 1)
                     patch{0x23dc34 + 0x8, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SoldierAnimator ctor (byte 2)
                     patch{0x23de9f + 0x7, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetNewOwner (byte 1)
                     patch{0x23de9f + 0x8, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetNewOwner (byte 2)
                     patch{0x23fce7 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateActionAnimation
                     patch{0x23fe02 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateActionAnimation
                     patch{0x23fe8b + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateActionAnimation
                     patch{0x2405a2 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateMovementAnimation
                     patch{0x240675 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateMovementAnimation
                     patch{0x240865 + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateMovementAnimation
                     patch{0x24086c + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // UpdateMovementAnimation
                     patch{0x23f2d2 + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetupPose
                     patch{0x247c83 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // LowResClass::PostLoad
                     patch{0x247d56 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // LowResClass::PostLoad
                     patch{0x247bc0 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // LowResClass::PostLoad
                     patch{0x23ebd7 + 0x7, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponAnimationMap (byte 1)
                     patch{0x23ebd7 + 0x8, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponAnimationMap (byte 2)
                     patch{0x23edd6 + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponComboState
                     patch{0x23eddd + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponComboState
                     patch{0x23edfe + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponComboState
                     patch{0x23ee0f + 0x6, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // SetWeaponComboState
                     // EntitySoldier::Render (4-byte, NOT 8-bit)
                     patch{0xe2838 + 0x1, 0xa4, 0xfe, {.file_offset = true}}, // Render
                     patch{0xe283d + 0x1, 0xa4, 0xfe, {.file_offset = true}}, // Render
                     patch{0xe2778 + 0x1, 0xa4, 0xfe, {.file_offset = true}}, // Render
                     patch{0xe274a + 0x1, 0xa4, 0xfe, {.file_offset = true}}, // Render
                     // Combo::ResolveForWeapon — gates GetUpperBodyAnimation on anim index < 0xA4
                     patch{0x74B82 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // ResolveForWeapon (CMP AL, 0xA4)
                     // Combo::State::Deflect::DeflectAnimation — rejects anim indices >= 0xA4
                     patch{0x72BD9 + 0x2, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // DeflectAnimation (CMP CL, 0xA4)
                  },
            },

            patch_set{
               .name = "High-Res Animation Limit",
               .patches =
                  {
                     // SoldierAnimatorHighResClass::PostLoad — increase capacity 50 -> 335
                     // Steam retail uses PUSH imm8 (6A 32) — need code cave to widen to PUSH imm32
                     // Code cave: JMP to INT3 padding, PUSH 0x14F, LEA ESI,[EAX+0x10], JMP back
                     patch{0x2467d2,     0x6a,        0xe9,        {.file_offset = true, .values_are_8bit = true}}, // JMP to code cave
                     patch{0x2467d2 + 1, 0x10708d32,  0x8c1,       {.file_offset = true}},                          // JMP displacement
                     patch{0x247098,     0xcc,        0x68,        {.file_offset = true, .values_are_8bit = true}}, // PUSH imm32 opcode
                     patch{0x247098 + 1, 0xcccccccc,  0x14f,       {.file_offset = true}},                          // PUSH 0x14F
                     patch{0x24709d,     0xcc,        0xeb,        {.file_offset = true, .values_are_8bit = true}}, // JMP short opcode
                     patch{0x24709d + 1, 0xcc,        0x05,        {.file_offset = true, .values_are_8bit = true}}, // JMP short +5
                     patch{0x2470a4,     0xcc,        0x8d,        {.file_offset = true, .values_are_8bit = true}}, // LEA ESI,[EAX+0x10] byte 1
                     patch{0x2470a4 + 1, 0xcc,        0x70,        {.file_offset = true, .values_are_8bit = true}}, // LEA ESI,[EAX+0x10] byte 2
                     patch{0x2470a4 + 2, 0xcc,        0x10,        {.file_offset = true, .values_are_8bit = true}}, // LEA ESI,[EAX+0x10] byte 3
                     patch{0x2470a7,     0xcc,        0xe9,        {.file_offset = true, .values_are_8bit = true}}, // JMP back opcode
                     patch{0x2470a7 + 1, 0xcccccccc,  0xfffff72b,  {.file_offset = true}},                          // JMP back displacement
                     // Standard value patches
                     patch{0x2467d7 + 0x2, 0x32,    0x14f,              {.file_offset = true}}, // MOV [EAX], count
                     patch{0x246839 + 0x2, 0x64640, 0x14f * 0x2020,     {.file_offset = true}}, // CMP EDI, array_size
                     patch{0x242c62 + 0x1, 0x64640, 0x14f * 0x2020,     {.file_offset = true}}, // CMP EAX, array_size
                     patch{0x2467b0 + 0x1, 0x64650, 0x14f * 0x2020 + 0x10, {.file_offset = true}}, // PUSH heap_alloc_size
                  },
            },

            patch_set{
               .name = "Network Timer Increase",
               .patches =
                  {
                     // TTYScroll: Timer 2 (FrameUpdate::Update) divisor 30 -> 120 Hz
                     // PUSH imm8 operand at 0x0052d4c2 (VA) — same address as GOG
                     patch{0x0052d4c2, 0x1e, 0x78, {.values_are_8bit = true}}, // Timer 2: 30 Hz -> 120 Hz
                  },
            },

            patch_set{
               .name = "Chunk Push Fix",
               .patches =
                  {
                     // ApplyRadiusPush: remove early return when ChunkFrequency triggers.
                     // Vanilla skips push entirely when chunk flag is set — replace
                     // POP ESI; MOV ESP,EBP with JMP +0x25 to push calculation.
                     // Bytes: 5E 8B E5 5D -> EB 25 90 90  (same address as GOG)
                     patch{0x004e1a24, 0x5DE58B5E, 0x909025EB},
                  },
            },

            patch_set{
               .name = "Matrix/Item Pool Limit Extension",
               .patches =
                  {
                     // matrixPool address redirects
                     patch{0x2af682 + 0x1, 0x8bef50, matrixPool_address, {.file_offset = true, .expected_is_va = true}},
                     patch{0x2af6ef + 0x2, 0x8bef50, matrixPool_address, {.file_offset = true, .expected_is_va = true}},
                     patch{0x2b7da7 + 0x2, 0x8bef50, matrixPool_address, {.file_offset = true, .expected_is_va = true}},
                     patch{0x6992 + 0x1,   0x8bef50, matrixPool_address, {.file_offset = true, .expected_is_va = true}},
                     // matrixPool size
                     patch{0x2af68a + 0x2, 0xbf6, matrixPool_size, {.file_offset = true}},
                     patch{0x2af6f8 + 0x1, 0xbf6, matrixPool_size, {.file_offset = true}},
                     patch{0x6997 + 0x1,   0xbf5, matrixPool_size - 1, {.file_offset = true}},
                     // transparentItemsSize: 800 -> 204800
                     patch{0x6b10 + 0x1, 0x320, 0x32000, {.file_offset = true}},
                     // postTransparentItemSize: 512 -> 131072
                     patch{0x6a80 + 0x1, 0x200, 0x20000, {.file_offset = true}},
                     // preShadowTransparentItemSize code cave: PUSH 100 -> PUSH 25600
                     patch{0x6ab0,       0x6a, 0xeb, {.file_offset = true, .values_are_8bit = true}},       // JMP +0x21
                     patch{0x6ab0 + 0x1, 0x64, 0x21, {.file_offset = true, .values_are_8bit = true}},       // JMP offset
                     patch{0x6ad3,       0xcc, 0x68, {.file_offset = true, .values_are_8bit = true}},       // PUSH imm32 opcode
                     patch{0x6ad3 + 0x1, 0xcccccccc, 0x6400, {.file_offset = true}},                        // PUSH 0x6400
                     patch{0x6ad8,       0xcc, 0xeb, {.file_offset = true, .values_are_8bit = true}},       // JMP short back
                     patch{0x6ad8 + 0x1, 0xcc, 0xd8, {.file_offset = true, .values_are_8bit = true}},       // JMP offset (-0x28)
                  },
            },

            patch_set{
               .name = "String Pool Increase",
               .patches =
                  {
                     patch{0x13a543 + 0x1, 0x1770, 0x20000, {.file_offset = true}}, // 6000 -> 128KB
                  },
            },

         },
   },
};
