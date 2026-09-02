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
static const uint32_t EntityEx_mIdMap_sentinel_addr = (uint32_t)&EntityEx_mIdMap_new[0x4004]; // end-of-iteration sentinel

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

// Renderer cache increase: 15 -> RENDERER_CACHE_SLOTS entries
// Each RedParticleRenderer cache entry is 0x3558 bytes.
char g_sCaches_storage[RENDERER_CACHE_SLOTS * 0x3558] = {};
static const uint32_t g_sCaches_address = (uint32_t)&g_sCaches_storage[0];
static const uint32_t modtools_sCaches_va = game_addrs::modtools::s_caches;
static const uint32_t steam_sCaches_va = game_addrs::steam::s_caches;
static const uint32_t gog_sCaches_va = game_addrs::gog::s_caches;

// Audio stream limit increase: 6 -> AUDIO_STREAM_SLOTS concurrent OpenAudioStream
// handles.  Snd::Stream is 0x3611BC bytes (mostly the 8 stream buffers plus the
// 3x8 ADPCM packet buffers, 110592 bytes each), so each extra slot costs ~3.4 MB
// and the whole array is ~40 MB of a 32-bit process's 2 GB.
//
// That is far too much to spend unconditionally, and a DLL global would: .bss is
// part of SizeOfImage, so the loader reserves it before any of our code runs and
// long before the INI is read.  Instead the buffers are VirtualAlloc'd by
// audio_stream_prepare() below, which apply_patches calls only after the set's
// INI toggle passes — so AudioStreamLimit=0 really does cost zero address space.
//
// The base addresses are therefore not known at static-init time.  The table
// entries carry `&<name>_address` in patch::replacement_base and an offset in
// replacement_value, and apply_patches resolves the sum at write time.
char* snd_stream_storage = nullptr;
char* snd_stream_queue_storage = nullptr;
static char* snd_playing_props_storage = nullptr;

static uint32_t snd_stream_storage_address = 0;
static uint32_t snd_playing_props_address  = 0;
static uint32_t snd_stream_count_address   = 0;
static uint32_t snd_playing_pos_address    = 0;
static uint32_t snd_playing_vel_address    = 0;
static uint32_t snd_stream_queue_address   = 0;

char* audio_stream_playing_props() { return snd_playing_props_storage; }

// One reservation carved into the stream array plus Snd::SoundStream's five
// per-slot arrays.  The stream array goes first so it inherits VirtualAlloc's
// 64 KB-aligned base, matching the alignment the exe's own BSS array had.
static bool audio_stream_prepare()
{
   if (snd_stream_storage) return true; // already prepared

   constexpr uint32_t streams_size = AUDIO_STREAM_SLOTS * AUDIO_STREAM_SIZE;
   constexpr uint32_t props_size   = AUDIO_STREAM_SLOTS * 4;
   constexpr uint32_t count_size   = AUDIO_STREAM_SLOTS * 4;
   constexpr uint32_t pos_size     = AUDIO_STREAM_SLOTS * 12;
   constexpr uint32_t vel_size     = AUDIO_STREAM_SLOTS * 12;
   constexpr uint32_t queue_size   = AUDIO_STREAM_SLOTS * 12;
   constexpr uint32_t total =
      streams_size + props_size + count_size + pos_size + vel_size + queue_size;

   static_assert(streams_size % 4 == 0, "arrays after the streams must stay dword-aligned");

   char* block = (char*)VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
   if (!block) return false; // let apply_patches skip the set rather than run half-patched

   char* cursor = block;
   auto carve = [&cursor](uint32_t size) {
      char* p = cursor;
      cursor += size;
      return p;
   };

   snd_stream_storage       = carve(streams_size);
   snd_playing_props_storage = carve(props_size);
   char* count_storage      = carve(count_size);
   char* pos_storage        = carve(pos_size);
   char* vel_storage        = carve(vel_size);
   snd_stream_queue_storage = carve(queue_size);

   snd_stream_storage_address = (uint32_t)(uintptr_t)snd_stream_storage;
   snd_playing_props_address  = (uint32_t)(uintptr_t)snd_playing_props_storage;
   snd_stream_count_address   = (uint32_t)(uintptr_t)count_storage;
   snd_playing_pos_address    = (uint32_t)(uintptr_t)pos_storage;
   snd_playing_vel_address    = (uint32_t)(uintptr_t)vel_storage;
   snd_stream_queue_address   = (uint32_t)(uintptr_t)snd_stream_queue_storage;

   return true;
}

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
               .name = "Particle Effect Skip Fix",
               .patches =
                  {
                     // RedParticleRenderer::IsFull -- the 200-entry batch test.
                     //
                     // ParticleEmitterObject::Render_ calls IsFull as its first gate and
                     // returns early when it reports full.  That early return lands on the
                     // function's epilogue, which is PAST the FlushParticleCache and
                     // RenderAll calls that end Render_ -- so a full batch does not just
                     // drop one effect, it skips the very drain that would have emptied
                     // the batch.  The batch therefore stays full and every remaining
                     // effect in that pass takes the same early return: one full cache
                     // silently deletes the rest of the frame's particles.  That is the
                     // "whole explosions blink out under load" symptom.
                     //
                     // The threshold is raised past reach instead of being removed.  The
                     // test is SETGE (signed), so 0x7FFFFFFF is never met, and Render_
                     // always runs through to its RenderAll.  Particles past the batch's
                     // real 200 are still refused by AddParticle, which drops them one at
                     // a time -- a partial effect instead of no effect, with the flush
                     // cadence restored for everything drawn after it.
                     //
                     // Single call site on all three builds (verified by xref), so nothing
                     // else depends on this returning true.
                     patch{0x424DF3, 0xC8, 0x7FFFFFFF, {.file_offset = true}}, // IsFull: CMP EDX,0xC8 -> 0x7FFFFFFF
                  },
            },

            patch_set{
               .name = "Particle Cache Reset Fix",
               .patches =
                  {
                     // RedParticleRenderer::RenderAllCaches leaks its pool state when a
                     // frame's dynamic-mesh acquisition fails.
                     //
                     //   0x00827768  TEST <mesh>,<mesh> / JZ 0x008278a5   <- bail
                     //   ...
                     //   0x00827899  s_cacheIndex  = 0                  <- SKIPPED
                     //              currentCache = NULL                   <- SKIPPED
                     //   0x008278a5  POP .. / RET                       <- bail lands here
                     //
                     // The bail jumps PAST the two resets that end the function, so
                     // s_cacheIndex keeps whatever height it had reached and currentCache
                     // stays pointing at a stale cache.  On the following frames
                     // SetCurrentCache starts from that height, walks into its allocation
                     // clamp, and sets currentCache = NULL -- after which EVERY
                     // SubmitParticle in the game silently no-ops.  Particles stay gone
                     // until some later RenderAllCaches happens to complete in full.
                     //
                     // Retargeting the branch to the reset block instead of the epilogue
                     // makes the failure path drop that frame's particles (which it was
                     // doing anyway) without poisoning the next one; it then falls through
                     // into the same epilogue.  The reset block only zeroes two globals, so
                     // reaching it early is safe.
                     //
                     // This is a stock engine bug, not one the cache patches introduce --
                     // but they make it easier to reach, because the spill hook deliberately
                     // uses more caches per frame and therefore requests more meshes.
                     patch{0x42776A, 0x137, 0x12B, {.file_offset = true}}, // JZ 0x008278a5 -> 0x00827899
                  },
            },

            patch_set{
               .name = "EntityPath Branch Region Fix",
               .patches =
                  {
                     // EntityPath::BranchRegionFactory has its CreateRegion in the WRONG
                     // VTABLE SLOT, so the engine never calls it and no BranchRegion is
                     // ever constructed -- path nodes carrying BranchRegion("x") can never
                     // resolve, in any map, on any build. Proven at runtime: the factory is
                     // correctly registered and correctly selected by name prefix, yet
                     // BranchRegionFactory::CreateRegion is never entered and the region
                     // list stays empty (live=0) while lookups run.
                     //
                     // LoadUtil::ProcessRegionInfo dispatches through vtable SLOT 1:
                     //     (**(code **)(*factory + 4))(desc, name)
                     //
                     // and every factory that works overrides that slot:
                     //     soundstatic vtbl 0x00A2B970  slot1 = 0x00403E0E  (its own)
                     //     danger      vtbl 0x00A47014  slot1 = 0x00405C22  (its own)
                     //
                     // but the branch factory leaves slot 1 as the BASE implementation and
                     // puts its own in slot 3, which nothing calls:
                     //     entitypathbranch vtbl 0x00A4B5A4
                     //         slot0 0x0040FAB5 -> 0x005E4690
                     //         slot1 0x00821F60    RedRegionFactory::CreateRegion (base,
                     //                             builds a plain RedRegion)
                     //         slot2 0x00821FC0    base
                     //         slot3 0x0040DF58 -> 0x005E4C90  BranchRegionFactory::CreateRegion
                     //
                     // Almost certainly a signature mismatch that made the compiler append a
                     // new virtual rather than override. Point slot 1 at the real one.
                     //
                     // NOTE: this set only repairs the dispatch. The id a region
                     // registers under is hashed from strchr(name, ' ') with the pointer
                     // left ON the space, so on its own a region named
                     // "entitypathbranch dropzone1" answers to " dropzone1" -- with the
                     // leading space. entity/branch_region_fix.cpp re-stamps the id from
                     // the text AFTER the separator, which is what makes the spelling a
                     // mapper would actually write, BranchRegion("dropzone1"), resolve.
                     patch{0x00A4B5A8, 0x00821F60, 0x005E4C90, {.values_are_va = true}}, // vtable slot 1 -> BranchRegionFactory::CreateRegion
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
                     patch{0x8e84f + 0x2, 0x400, 0x800, {.file_offset = true}},    // FUN_0048e7e0 loop bound (inner rescan)
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

                     // --- Address redirects: end-of-iteration sentinel (0xb7cd3c -> new) ---
                     // These CMP sites compare the value just read above against the sentinel to
                     // detect an exhausted scan. In the original table this aliased for free (the
                     // value-array's one-past-the-end read landed exactly on 0xb7cd3c). The value
                     // read was relocated above; without also relocating the compare, the two sides
                     // no longer refer to the same slot and the boundary case dereferences garbage.
                     patch{0x8e80b + 0x2, 0xb7cd3c, EntityEx_mIdMap_sentinel_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_0048e7e0 sentinel compare
                     patch{0x8e85e + 0x2, 0xb7cd3c, EntityEx_mIdMap_sentinel_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_0048e7e0 sentinel compare
                     patch{0x8eb5b + 0x2, 0xb7cd3c, EntityEx_mIdMap_sentinel_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_0048eaa0 sentinel compare
                     patch{0x8ebcb + 0x2, 0xb7cd3c, EntityEx_mIdMap_sentinel_addr, {.file_offset = true, .expected_is_va = true}},  // FUN_0048eaa0 sentinel compare
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

            // Port of PrismaticFlower's fix (upstream 2cb6a11): PropGenerator::Update
            // sometimes branches over the cluster-object-array bounds check (typically
            // at very high FOVs) and reads past the array end. Redirect that branch to
            // the bounds check; the two swapped instructions create a clean jump target.
            patch_set{
               .name = "PropGenerator Update Loop Exit Condition",
               .patches =
                  {
                     patch{0x0073d314, 0x49, 0x33, {.values_are_8bit = true}},  // branch offset -> bounds check
                     patch{0x0073d344, 0x4024548B, 0x01714488},                 // swap: MOV EDX,[ESP+40] <-> MOV [ECX+ESI*2+1],AL
                     patch{0x0073d348, 0x01714488, 0x4024548B},
                  },
            },

            // Port of PrismaticFlower's fix (upstream 9c6170e): SkyObjectClass
            // instances are capped by a global counter; NOP out the "INC ECX;
            // MOV [count],ECX" (7 bytes) so the count never advances and the
            // limit is never hit.  3-byte prefix as three 8-bit patches + the
            // 4-byte address operand -> a 4-byte NOP (0F 1F 40 00 = 0x00401F0F).
            patch_set{
               .name = "SkyObjectClass Limit Extension",
               .patches =
                  {
                     patch{0x006c23ae, 0x41, 0x0f, {.values_are_8bit = true}},   // INC ECX     -> NOP (0F 1F 00)
                     patch{0x006c23af, 0x89, 0x1f, {.values_are_8bit = true}},
                     patch{0x006c23b0, 0x0d, 0x00, {.values_are_8bit = true}},
                     patch{0x006c23b1, 0x00ba45cc, 0x00401f0f, {.expected_is_va = true}}, // MOV [count],ECX operand -> 4-byte NOP
                  },
            },
            // Raise the concurrent Lua OpenAudioStream limit from 6 to
            // AUDIO_STREAM_SLOTS.  Snd::EngineBase::smStreams is a *pointer* to a
            // 6-element BSS array of 0x3611BC-byte Snd::Stream objects, so the
            // array itself relocates cleanly to a DLL buffer; the engine's own
            // ctor/dtor loops in EngineBase::Open/Close then construct and destroy
            // all AUDIO_STREAM_SLOTS of them.  Most loops express the limit as a
            // byte bound (6 * 0x3611BC = 0x1446A68) rather than a count.
            //
            // Snd::SoundStream keeps five arrays indexed by the same slot index
            // (smPlayingProps / smCount / smPlayingPos / smPlayingVel / smQueue),
            // packed into BSS with no slack, so those relocate alongside it -
            // otherwise slot 6+ would write over its neighbour.
            //
            // The `cmp reg, 6` sites listed here are the stream-index bounds only.
            // Several neighbouring `cmp reg, 6` / `cmp reg, 5` in the same Lua
            // callbacks are lua_gettop() argument-count checks and must NOT move.
            patch_set{
               .name = "Audio Stream Limit Increase",
               .prepare = audio_stream_prepare,
               .patches =
                  {
                     patch{0x482D0D, 0xEEA618, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_storage_address}, // EngineBase::Open: smStreams = smStreamStorage

                     patch{0x34F066, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x0074F064
                     patch{0x356EC2, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00756EC1
                     patch{0x482846, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00882844
                     patch{0x48297F, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x0088297D
                     patch{0x4829EE, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x008829EC
                     patch{0x482BC6, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00882BC4
                     patch{0x482D70, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00882D6E
                     patch{0x4861C8, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x008861C6

                     patch{0x7EB8F, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // StopAudioStream: stream handle bound
                     patch{0x7F0F5, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // AudioStreamAppendSegments: handle bound (warn path)
                     patch{0x7F126, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // AudioStreamAppendSegments: handle bound
                     patch{0x8160F, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // AudioStreamComplete: stream scan bound
                     patch{0x4828CD, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // EngineBase::GetFreeStream
                     patch{0x48B044, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // SoundStream::UpdateAll
                     // SoundStream::StopAll counts *bytes* into smQueue and compares with
                     // `cmp reg, 0x48` — opcode 83, whose imm8 is SIGN-extended.  12 slots
                     // would need 0x90, which reads back as -112 and makes the unsigned JB
                     // loop forever off the end of the array.  The counter is never
                     // dereferenced (smQueue/smCount have their own pointers), so count
                     // slots instead of bytes and both immediates stay small.
                     patch{0x48A410, 0x0C, 1, {.file_offset = true, .values_are_8bit = true}},  // SoundStream::StopAll: ADD EBP,0xC -> ADD EBP,1
                     patch{0x48A419, 0x48, AUDIO_STREAM_SLOTS, {.file_offset = true, .values_are_8bit = true}}, // SoundStream::StopAll: CMP EBP,72 -> CMP EBP,slots

                     patch{0x6225AB, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // smQueue vector ctor count
                     patch{0x6280C6, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // smQueue vector dtor count

                     patch{0x48A459, 0x233A130, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[0]
                     patch{0x48A45F, 0x233A134, 4, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[1]
                     patch{0x48A465, 0x233A138, 8, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[2]
                     patch{0x48A46B, 0x233A13C, 12, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[3]
                     patch{0x48A471, 0x233A140, 16, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[4]
                     patch{0x48A477, 0x233A144, 20, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[5]
                     patch{0x48ADEA, 0x233A130, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[0]
                     patch{0x48AE3A, 0x233A130, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[0]
                     patch{0x48AF3E, 0x233A130, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[0]

                     patch{0x489BF7, 0x233A148, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x489C6F, 0x233A148, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x48A3B5, 0x233A148, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x48A565, 0x233A148, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x48A5BA, 0x233A148, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x48A5C7, 0x233A148, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x48A608, 0x233A148, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]

                     patch{0x48ADFA, 0x233A1A8, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_vel_address}, // smPlayingVel[0]
                     patch{0x48AE64, 0x233A1A8, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_vel_address}, // smPlayingVel[0]
                     patch{0x48AF64, 0x233A1A8, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_vel_address}, // smPlayingVel[0]
                     patch{0x48AFE8, 0x233A1A8, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_vel_address}, // smPlayingVel[0]

                     patch{0x489C3B, 0x233A1F0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x489C42, 0x233A1F0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x48A3BA, 0x233A1F0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x48A4FD, 0x233A1F0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x6225AF, 0x233A1F0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x6280CA, 0x233A1F0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]

                     patch{0x48AE01, 0x233A720, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_pos_address}, // smPlayingPos[0]
                     patch{0x48AE48, 0x233A720, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_pos_address}, // smPlayingPos[0]
                     patch{0x48AF44, 0x233A720, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_pos_address}, // smPlayingPos[0]
                  },
            },

            patch_set{
               .name = "Lightsaber Block Direction Fix",
               .patches =
                  {
                     // WeaponMelee::UpdateFire (VA 0x639020) calls the victim's
                     // GameObject::Deflect (vtable +0xD4) with its two vectors SWAPPED.
                     // The engine signature is Deflect(Ordnance*, PblVector3* worldPos,
                     // PblVector3* dir), but the melee hit path pushes
                     // (NULL, attackerMatrix.row2 /*a direction*/, hitPoint /*a position*/).
                     //
                     // Combo::Deflect::TestDeflect gates the block on
                     //   atan2(-dot(defender.row0, dir), -dot(defender.row2, dir))
                     // against the ODF DeflectAngle(min,max) window.  Fed a world-space
                     // position, that measures the bearing of the world-origin -> hit-point
                     // vector in the defender's frame, so a saber only blocks another saber
                     // while the defender happens to be facing the map origin.  Blaster
                     // deflection uses a different, correct call site, which is why only
                     // saber-vs-saber blocking is affected.  A combo with no DeflectAngle
                     // (min == max) skips the gate entirely and never showed the bug.
                     //
                     // Both operands are 4-byte LEA displacements, so the fix is a pure
                     // disp32 rewrite with no length change:
                     //   0x639F84  LEA ECX,[ESP+0x108]  (hit point) -> pushed as dir
                     //   0x639FB1  LEA EDX,[ESP+0x0F4]  (row2)      -> pushed as worldPos
                     // The +4 asymmetry on the second site is real: it executes after the
                     // first PUSH, so its displacement is relative to ESP-4.
                     patch{0x239F87, 0x108, 0x0F0, {.file_offset = true}}, // dir  <- attacker forward (matrix row 2)
                     patch{0x239FB4, 0x0F4, 0x10C, {.file_offset = true}}, // pos  <- world-space hit point
                  },
            },

            // -----------------------------------------------------------------
            // RedLodManager::SetClassMaxCost(class, maxCount, cost0, cost1, cost2, cost3)
            //
            // This is what decides when a soldier drops to its humanlz model, and it
            // is a COST BUDGET, not a distance.  RedLodManager::GetObjectLodHint
            // (mt 0x008D0220) walks LOD levels 3..0 over mClass[class].mData[level],
            // skipping levels whose mMaxCost is 0, and returns the first level whose
            // cost band contains the object's cost.  SoldierLodData (mt 0x00416248)
            // gives every soldier _LodClass 2 and a cost of 2000, so the stock LOD0
            // budget of 0x4650 (18000) buys **nine** soldiers at full detail before
            // the tenth is demoted.  At 20x that becomes 180.
            //
            // maxCount is NOT the soldier limiter and is deliberately left alone for
            // class 2.  SetClassMaxCost floors it at 256 (`if (0x100 < param_2)`)
            // before sizing the PblHeap, so the stock 100 is already 256 and
            // upstream's 100 -> 127 bump changes nothing on any build.  Upstream's
            // "8-bit signed int, needs relocating to increase it" note is chasing a
            // ceiling that only starts to bind above 256 simultaneously-tracked
            // soldiers, which BF2 does not reach on screen.
            // -----------------------------------------------------------------
            patch_set{
               .name = "LOD Limit Extension",
               .patches =
                  {
                     patch{0x41D455 + 0x1, 0xc8,    0xc8 * 0x14,    {.file_offset = true}}, // modelClass(0)    maxCount
                     patch{0x41D450 + 0x1, 0xc350,  0xc350 * 0x14,  {.file_offset = true}}, // modelClass(0)    LOD0
                     patch{0x41D441 + 0x1, 0x9c40,  0x9c40 * 0x14,  {.file_offset = true}}, // modelClass(0)    LOD3
                     patch{0x41D3C0 + 0x1, 0x258,   0x258 * 0x14,   {.file_offset = true}}, // bigModelClass(1) maxCount
                     patch{0x41D3BB + 0x1, 0x186a0, 0x186a0 * 0x14, {.file_offset = true}}, // bigModelClass(1) LOD0
                     patch{0x41D3AC + 0x1, 0x9c40,  0x9c40 * 0x14,  {.file_offset = true}}, // bigModelClass(1) LOD3
                     patch{0x41D387 + 0x1, 0x4650,  0x4650 * 0x14,  {.file_offset = true}}, // soldierClass(2)  LOD0  9 -> 180 soldiers
                     patch{0x41D37A + 0x1, 0x9c40,  0x9c40 * 0x14,  {.file_offset = true}}, // soldierClass(2)  LOD3
                     patch{0x41D410 + 0x1, 0x5dc,   0x5dc * 0x14,   {.file_offset = true}}, // hugeModelClass(3) maxCount, uber
                     patch{0x41D40B + 0x1, 0x2710,  0x2710 * 0x14,  {.file_offset = true}}, // hugeModelClass(3) LOD0, uber
                     patch{0x41D426 + 0x1, 0x12c,   0x12c * 0x14,   {.file_offset = true}}, // hugeModelClass(3) maxCount
                     patch{0x41D421 + 0x1, 0x3e8,   0x3e8 * 0x14,   {.file_offset = true}}, // hugeModelClass(3) LOD0
                     patch{0x41D3FA + 0x1, 0x9c40,  0x9c40 * 0x14,  {.file_offset = true}}, // hugeModelClass(3) LOD3
                  },
            },


            patch_set{
               .name = "Explosion VisibleRadius Increase",
               .patches =
                  {
                     patch{0x203637 + 0x6, 0x42700000, 0x461c4000, {.file_offset = true}}, // 60.0f -> 10000.0f
                  },
            },

            // Upstream's fourth render patch, "nearScene Extension", is deliberately
            // not carried on any build.  It is a single byte 0 -> 1 whose effect
            // nobody has pinned down, upstream ships it commented out on the modtools
            // build they develop on, and its modtools offset (0x398B75) does not even
            // hold the expected 0 in the shipping debug exe.  With no symptom to
            // describe there is nothing to put in front of a user, so it stays out
            // until someone can say what it fixes.

            patch_set{
               .name = "Soldier Height Ceiling Removal",
               .patches =
                  {
                     // EntitySoldier::Update runs an inline world-bounds block; its last
                     // test is `pos.Y > 1000.0f -> "Unit flew over world" -> Kill()`.
                     // The constant is 1000.0f, NOT 1024 -- verified at 0x00A331B0
                     // (`00 00 7A 44`).  That float is a shared .rdata pool entry with 33
                     // other readers, so it must NOT be edited; the branch goes instead.
                     //
                     //   00546679  D8 1D B031A300  FCOMP dword [0x00A331B0]
                     //   0054667F  DF E0           FNSTSW AX
                     //   00546681  F6 C4 41        TEST AH,0x41
                     //   00546684  75 48           JNZ  -> taken when Y <= 1000, skips Kill
                     //
                     // JNZ -> JMP makes the skip unconditional.  The floor (-1000), the
                     // fall-speed kill (-60) and the horizontal walls (+/-2500) are left
                     // alone -- only the ceiling goes.
                     patch{0x00546684, 0x75, 0xEB, {.values_are_8bit = true}},
                  },
            },

            patch_set{
               .name = "Attached Effects Overflow Fix",
               .patches =
                  {
                     // AttachedEffectsClass keeps s_aAttachClassData[64] plus a
                     // 32-bit s_uiNumAttached.  Both appending SetProperty
                     // handlers guard their TABLE store with `CMP count,0x40`,
                     // but the full-table path then falls into a shared tail:
                     //
                     //   004C28C9  A1 D4A2B700   MOV  EAX,[s_uiNumAttached]
                     //   004C28CE  40            INC  EAX
                     //   004C28CF  50            PUSH EAX          ; the %d arg
                     //   004C28D0  68 2C88A300   PUSH "AttachEffects: too many
                     //                                 effects - increase to %d"
                     //   004C28D5  A3 D4A2B700   MOV  [s_uiNumAttached],EAX  <== BUG
                     //   004C28DA  E8 ........   CALL RedWarning
                     //
                     // The increment exists to be the printf argument; writing it
                     // BACK is the defect.  Nothing is stored in the table, so the
                     // counter runs ahead of the data and BuildAttachedEffectsClass
                     // then copies N*20 bytes out of a 1280-byte array -- a pure
                     // over-read of adjacent .bss.  BuildEffects walks the ghost
                     // entries and dereferences their null pOdfClass with no check
                     // (0x004C25C7 `MOV EDX,[ECX]`), so the real symptom is an
                     // access violation reading 0x00000000 during level load.
                     //
                     // NOPping the write-back makes the engine refuse effects past
                     // 64 and keep going -- which is exactly what the retail builds
                     // already do: their optimizer dropped this store along with the
                     // RedWarning, so every retail increment is immediately followed
                     // by `CMP EAX,0x40 / JNC`.  Verified on both; there is no
                     // retail site to patch.
                     //
                     // Safe as a bare NOP run: EAX was already incremented and
                     // pushed one instruction earlier, the CALL is __cdecl and the
                     // single ADD ESP,0x1C afterwards pops all seven pushed dwords,
                     // so stack balance is untouched.  No branch target lands inside
                     // the five bytes.  Cosmetic: the warning now always says
                     // "increase to 65".
                     //
                     // Do NOT touch the legitimate in-range advances at 0x004C2865
                     // and 0x004C29AF.
                     patch{0x004C28D5, 0xA3, 0x90, {.values_are_8bit = true}},
                     patch{0x004C28D6, 0xD4, 0x90, {.values_are_8bit = true}},
                     patch{0x004C28D7, 0xA2, 0x90, {.values_are_8bit = true}},
                     patch{0x004C28D8, 0xB7, 0x90, {.values_are_8bit = true}},
                     patch{0x004C28D9, 0x00, 0x90, {.values_are_8bit = true}},
                  },
            },

            patch_set{
               .name = "Reverb Restore On Map Exit",
               .patches =
                  {
                     // EAX reverb stops working for the rest of the session once you
                     // leave a map through the pause menu.  Snd::Listener::mFlags bit
                     // 3 is the "reverb updates enabled" gate, and it is the ONLY
                     // thing in the chain that survives a map change:
                     //
                     //   Snd::Listener::Update
                     //     if ((mFlags & 6) == 6 && preset != Engine::smReverb
                     //         && (mFlags & 8))          <== bit 3
                     //         Engine::SetGlobalReverb(preset);
                     //
                     // and SetGlobalReverb is the sole writer of the EAX property set
                     // (DSBuffer::SetProperty on EAXPROPERTYID_EAX40_FXSlot0), so with
                     // the bit clear nothing ever reaches EAX again.
                     //
                     // PauseMenu::_SetPauseAudio(true) clears bit 3 on every listener
                     // and applies a dry pause reverb.  _SetPauseAudio(false) sets it
                     // back -- but both halves sit behind `!GameLoop::IsGameOver()`,
                     // and the two ways out of a map do not agree:
                     //
                     //   ScriptCB_RestartMission  calls PauseMenu::SetPaused(v,false)
                     //                            first, so the restore runs.
                     //   ScriptCB_QuitToShell     goes straight to NetSetup::RequestQuit
                     //                            and never unpauses.
                     //
                     // That is exactly the reported behaviour: restarting a map keeps
                     // EAX, quitting to the main menu kills it until the game is
                     // restarted.  Snd::EngineBase::smListeners is a static array of
                     // four built once by a CRT initializer, so the cleared bit lives
                     // as long as the process.
                     //
                     // GameSoundEngine::Destroy is the single choke point for "this
                     // map is going away" (GameLoop::PostStateCleanup and
                     // GameSoundEngine::Term are its only callers).  It already
                     // rewrites the same flags word to release each listener:
                     //
                     //   0074F0C5  8B 0E        MOV ECX,[ESI]
                     //   0074F0C7  83 E1 FE     AND ECX,0xFFFFFFFE    ; drop "in use"
                     //   0074F0CA  83 C9 06     OR  ECX,0x6           ; space + preset
                     //   0074F0CD  89 0E        MOV [ESI],ECX
                     //
                     // Folding bit 3 into that OR re-arms the reverb on every teardown,
                     // whichever way the player left.  Nothing reads bit 3 outside
                     // Listener::Update, and a listener being released has no space to
                     // apply a preset from, so the bit only takes effect once the next
                     // map assigns one.  Pausing still mutes the reverb normally.
                     //
                     // Guarded as the full 4-byte run `83 C9 06 89` so the imm8 cannot
                     // match some unrelated OR.
                     patch{0x0074F0CA, 0x8906C983, 0x890EC983}, // OR ECX,0x6 -> OR ECX,0xE
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
                     // Value patches (GOG FlushParticleCache uses an EBP frame, so there is no
                     // ADD ESP to patch — but the sort pool is EBP-relative too; see the rebase below)
                     patch{0x20EBA9, 0x0000012C, 0x000004B0, {.file_offset = true}},                                                                                             // CacheParticle: CMP EDI, 300 -> 1200
                     patch{0x20EC1A, 0x00000980, 0x000025A0, {.file_offset = true}},                                                                                             // FlushParticleCache: SUB ESP, 0x980 -> 0x25A0
                     patch{0x20EC79, 0x0000012C, 0x000004B0, {.file_offset = true}},                                                                                             // FlushParticleCache: heap.maxCount 300 -> 1200
                     // FlushParticleCache sort-pool rebase.  Unlike modtools -- whose pool is
                     // ESP-relative (LEA EDX,[ESP+0x34]) and therefore moves down with a larger
                     // SUB ESP -- both retail builds address the pool from EBP
                     // (LEA EDI,[EBP-0x988]).  Growing SUB ESP there only adds unused space
                     // BELOW the pool: capacity stays at the vanilla 301 records while
                     // CacheParticle above is now allowed 1200.  Index 301 then overwrites the
                     // PblHeap object itself, 302 its mPool pointer, 303 the saved SEH handler
                     // at EBP-0xC, 304 the SEH trylevel, and 305 the saved EBP and RETURN
                     // ADDRESS -- a stack smash on any frame with >300 particles in front of
                     // the camera.
                     //
                     // So move the pool base instead: EBP-0x25A8 holds 1201 records
                     // (idx 0..1200, 1201*8 = 0x2588 bytes) ending exactly at the lowest local,
                     // EBP-0x20.  SUB ESP 0x25A0 above puts ESP at EBP-0x25AC, covering the new
                     // base with the same 4 bytes of slack the vanilla frame had.  Every
                     // EBP-relative reference to the pool is rewritten below; the render loop
                     // and the heap-pop helper reach it through mPool (EBP-0x18), which the
                     // rebased LEA writes, so they need no patch.
                     patch{0x20EC67, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // LEA EDI,[EBP-0x988]           - pool base
                     patch{0x20EC7F, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // MOV [EBP-0x988],0x7F7FFFFF    - pool[0].mKey sentinel
                     patch{0x20ED46, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // COMISS XMM3,[EBP+ECX*8-0x988] - sift compare
                     patch{0x20ED53, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // MOV EAX,[EBP+ECX*8-0x988]     - sift read  .mKey
                     patch{0x20ED5A, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // MOV [EBP+EDX*8-0x988],EAX     - sift write .mKey
                     patch{0x20ED61, 0xFFFFF67C, 0xFFFFDA5C, {.file_offset = true}}, // MOV EAX,[EBP+ECX*8-0x984]     - sift read  .mObj
                     patch{0x20ED68, 0xFFFFF67C, 0xFFFFDA5C, {.file_offset = true}}, // MOV [EBP+EDX*8-0x984],EAX     - sift write .mObj
                     patch{0x20ED74, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // COMISS XMM3,[EBP+ECX*8-0x988] - loop compare
                     patch{0x20ED7F, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // MOVSS [EBP+EDX*8-0x988],XMM3  - insert .mKey
                     patch{0x20ED8B, 0xFFFFF67C, 0xFFFFDA5C, {.file_offset = true}}, // MOV [EBP+EDX*8-0x984],EAX     - insert .mObj
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
               .name = "Particle Effect Skip Fix",
               .patches =
                  {
                     // RedParticleRenderer::IsFull -- the 200-entry batch test.
                     //
                     // ParticleEmitterObject::Render_ calls IsFull as its first gate and
                     // returns early when it reports full.  That early return lands on the
                     // function's epilogue, which is PAST the FlushParticleCache and
                     // RenderAll calls that end Render_ -- so a full batch does not just
                     // drop one effect, it skips the very drain that would have emptied
                     // the batch.  The batch therefore stays full and every remaining
                     // effect in that pass takes the same early return: one full cache
                     // silently deletes the rest of the frame's particles.  That is the
                     // "whole explosions blink out under load" symptom.
                     //
                     // The threshold is raised past reach instead of being removed.  The
                     // test is SETGE (signed), so 0x7FFFFFFF is never met, and Render_
                     // always runs through to its RenderAll.  Particles past the batch's
                     // real 200 are still refused by AddParticle, which drops them one at
                     // a time -- a partial effect instead of no effect, with the flush
                     // cadence restored for everything drawn after it.
                     //
                     // Single call site on all three builds (verified by xref), so nothing
                     // else depends on this returning true.
                     patch{0x2D3542, 0xC8, 0x7FFFFFFF, {.file_offset = true}}, // IsFull: CMP [ECX+0x3520],0xC8 -> 0x7FFFFFFF
                  },
            },

            patch_set{
               .name = "Particle Cache Reset Fix",
               .patches =
                  {
                     // RedParticleRenderer::RenderAllCaches leaks its pool state when a
                     // frame's dynamic-mesh acquisition fails.
                     //
                     //   0x006d41d1  TEST <mesh>,<mesh> / JZ 0x006d430d   <- bail
                     //   ...
                     //   0x006d42f9  s_cacheIndex  = 0                  <- SKIPPED
                     //              currentCache = NULL                   <- SKIPPED
                     //   0x006d430d  POP .. / RET                       <- bail lands here
                     //
                     // The bail jumps PAST the two resets that end the function, so
                     // s_cacheIndex keeps whatever height it had reached and currentCache
                     // stays pointing at a stale cache.  On the following frames
                     // SetCurrentCache starts from that height, walks into its allocation
                     // clamp, and sets currentCache = NULL -- after which EVERY
                     // SubmitParticle in the game silently no-ops.  Particles stay gone
                     // until some later RenderAllCaches happens to complete in full.
                     //
                     // Retargeting the branch to the reset block instead of the epilogue
                     // makes the failure path drop that frame's particles (which it was
                     // doing anyway) without poisoning the next one; it then falls through
                     // into the same epilogue.  The reset block only zeroes two globals, so
                     // reaching it early is safe.
                     //
                     // This is a stock engine bug, not one the cache patches introduce --
                     // but they make it easier to reach, because the spill hook deliberately
                     // uses more caches per frame and therefore requests more meshes.
                     patch{0x2D35D3, 0x136, 0x122, {.file_offset = true}}, // JZ 0x006d430d -> 0x006d42f9
                  },
            },

            patch_set{
               .name = "EntityPath Branch Region Fix",
               .patches =
                  {
                     // EntityPath::BranchRegionFactory puts its CreateRegion in vtable
                     // slot 3, but LoadUtil::ProcessRegionInfo dispatches through slot 1,
                     // which still holds the inherited base implementation (it builds a
                     // plain RedRegion). The branch creator is therefore never called and
                     // no BranchRegion ever exists, so every path node using
                     // BranchRegion("id") fails to resolve. Same defect on all builds --
                     // Ghidra even labels slot 1 here "RedRegionFactory member function
                     // inherited by EntityPath::BranchRegionFactory".
                     //
                     // GOG vtable 0x0079d3e0: slot1 0x006dd9d0 (base) / slot3 -> 0x004d0f00 (branch)
                     //
                     // Retail note: these builds strip the RedWarning text, so the failure
                     // is completely silent there -- no "Unable to find branch region" line.
                     patch{0x0079d3e4, 0x006dd9d0, 0x004d0f00, {.values_are_va = true}}, // vtable slot 1 -> BranchRegionFactory::CreateRegion
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
                     // Ported from the Steam set below with tools/port_gog.py.
                     // The four low sites (0x6992/0x6997/0x6a80/0x6ab0) are in
                     // the shift-0 range so their file offsets are unchanged;
                     // the RedRenderer sites move by +0x1080/+0x1090.  GOG's
                     // matrixPool lives at 0x8c03f0 (Steam 0x8bef50), and the
                     // 0xbf6/0xbf5/0x320/0x200 immediates and the 0xCC-padded
                     // code cave at 0x6ad3 are all present unchanged.
                     // matrixPool address redirects
                     patch{0x2b0702 + 0x1, 0x8c03f0, matrixPool_address, {.file_offset = true, .expected_is_va = true}},
                     patch{0x2b076f + 0x2, 0x8c03f0, matrixPool_address, {.file_offset = true, .expected_is_va = true}},
                     patch{0x2b8e37 + 0x2, 0x8c03f0, matrixPool_address, {.file_offset = true, .expected_is_va = true}},
                     patch{0x6992 + 0x1,   0x8c03f0, matrixPool_address, {.file_offset = true, .expected_is_va = true}},
                     // matrixPool size
                     patch{0x2b070a + 0x2, 0xbf6, matrixPool_size, {.file_offset = true}},
                     patch{0x2b0778 + 0x1, 0xbf6, matrixPool_size, {.file_offset = true}},
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
                     patch{0x13b293 + 0x1, 0x1770, 0x20000, {.file_offset = true}}, // 6000 -> 128KB
                  },
            },

            // Port of PrismaticFlower's fix (upstream 2cb6a11) — see modtools set.
            patch_set{
               .name = "PropGenerator Update Loop Exit Condition",
               .patches =
                  {
                     patch{0x0062bf6b, 0x68, 0x42, {.values_are_8bit = true}},  // branch offset -> bounds check
                  },
            },

            // Port of PrismaticFlower's fix (upstream 9c6170e) — see modtools set.
            // GoG has two counter-increment sites (INC ECX;MOV and INC EAX;MOV).
            patch_set{
               .name = "SkyObjectClass Limit Extension",
               .patches =
                  {
                     patch{0x00639e3e, 0x41, 0x0f, {.values_are_8bit = true}},   // INC ECX -> NOP (0F 1F 00)
                     patch{0x00639e3f, 0x89, 0x1f, {.values_are_8bit = true}},
                     patch{0x00639e40, 0x0d, 0x00, {.values_are_8bit = true}},
                     patch{0x00639e41, 0x01eb051c, 0x00401f0f, {.expected_is_va = true}}, // MOV [count],ECX operand -> 4-byte NOP
                     patch{0x00639e68, 0x40, 0x66, {.values_are_8bit = true}},   // INC EAX -> NOP (66 90)
                     patch{0x00639e69, 0xa3, 0x90, {.values_are_8bit = true}},
                     patch{0x00639e6a, 0x01eb051c, 0x00401f0f, {.expected_is_va = true}}, // MOV [count],EAX operand -> 4-byte NOP
                  },
            },
            // Raise the concurrent Lua OpenAudioStream limit from 6 to
            // AUDIO_STREAM_SLOTS.  Snd::EngineBase::smStreams is a *pointer* to a
            // 6-element BSS array of 0x3611BC-byte Snd::Stream objects, so the
            // array itself relocates cleanly to a DLL buffer; the engine's own
            // ctor/dtor loops in EngineBase::Open/Close then construct and destroy
            // all AUDIO_STREAM_SLOTS of them.  Most loops express the limit as a
            // byte bound (6 * 0x3611BC = 0x1446A68) rather than a count.
            //
            // Snd::SoundStream keeps five arrays indexed by the same slot index
            // (smPlayingProps / smCount / smPlayingPos / smPlayingVel / smQueue),
            // packed into BSS with no slack, so those relocate alongside it -
            // otherwise slot 6+ would write over its neighbour.
            //
            // The `cmp reg, 6` sites listed here are the stream-index bounds only.
            // Several neighbouring `cmp reg, 6` / `cmp reg, 5` in the same Lua
            // callbacks are lua_gettop() argument-count checks and must NOT move.
            patch_set{
               .name = "Audio Stream Limit Increase",
               .prepare = audio_stream_prepare,
               .patches =
                  {
                     patch{0x3349BC, 0x9E40C0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_storage_address}, // EngineBase::Open: smStreams = smStreamStorage

                     patch{0x138C64, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00539862
                     patch{0x3320DB, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00732CD9
                     patch{0x334476, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00735074
                     patch{0x3344E0, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x007350DE
                     patch{0x33460A, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00735208
                     patch{0x334A0F, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x0073560D
                     patch{0x334B20, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x0073571E

                     patch{0x19BEB3, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // StopAudioStream: stream handle bound
                     patch{0x19C23F, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // PlayAudioStreamUsingProperties: handle bound
                     patch{0x19C475, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // AudioStreamComplete: stream scan bound
                     patch{0x33458C, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // EngineBase::GetFreeStream
                     patch{0x337E2D, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // SoundStream::UpdateAll
                     // SoundStream::StopAll counts *bytes* into smQueue and compares with
                     // `cmp reg, 0x48` — opcode 83, whose imm8 is SIGN-extended.  12 slots
                     // would need 0x90, which reads back as -112 and makes the unsigned JB
                     // loop forever off the end of the array.  The counter is never
                     // dereferenced (smQueue/smCount have their own pointers), so count
                     // slots instead of bytes and both immediates stay small.
                     patch{0x337CD3, 0x0C, 1, {.file_offset = true, .values_are_8bit = true}},  // SoundStream::StopAll: ADD EBX,0xC -> ADD EBX,1
                     patch{0x337CDF, 0x48, AUDIO_STREAM_SLOTS, {.file_offset = true, .values_are_8bit = true}}, // SoundStream::StopAll: CMP EBX,72 -> CMP EBX,slots

                     patch{0xAD68, 5, 11, {.file_offset = true}}, // smPlayingPos vector ctor count
                     patch{0xAD88, 5, 11, {.file_offset = true}}, // smPlayingVel vector ctor count
                     patch{0xADBB, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // smQueue vector ctor count
                     patch{0x36A9F6, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // smQueue vector dtor count

                     patch{0x336D66, 0x1E2AC40, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x336DD1, 0x1E2AC40, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x336DD8, 0x1E2AC40, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x336E1B, 0x1E2AC40, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x336FEB, 0x1E2AC40, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x337C07, 0x1E2AC40, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]

                     patch{0x337074, 0x1E2AC58, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[0]
                     patch{0x33707E, 0x1E2AC5C, 4, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[1]
                     patch{0x337088, 0x1E2AC60, 8, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[2]
                     patch{0x337092, 0x1E2AC64, 12, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[3]
                     patch{0x33709C, 0x1E2AC68, 16, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[4]
                     patch{0x3370A6, 0x1E2AC6C, 20, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[5]
                     patch{0x337551, 0x1E2AC58, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[0]
                     patch{0x3375A8, 0x1E2AC58, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[0]
                     patch{0x337D69, 0x1E2AC58, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[0]

                     patch{0xAD63, 0x1E2AC70, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_pos_address}, // smPlayingPos[0]
                     patch{0x337567, 0x1E2AC70, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_pos_address}, // smPlayingPos[0]
                     patch{0x3375B4, 0x1E2AC70, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_pos_address}, // smPlayingPos[0]
                     patch{0x337D6F, 0x1E2AC70, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_pos_address}, // smPlayingPos[0]

                     patch{0xADBF, 0x1E2ACB8, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x336D00, 0x1E2ACB8, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x336FBD, 0x1E2ACB8, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x337C12, 0x1E2ACB8, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x36A9FA, 0x1E2ACB8, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]

                     patch{0xAD83, 0x1E2B1E0, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_vel_address}, // smPlayingVel[0]
                     patch{0x33755F, 0x1E2B1E0, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_vel_address}, // smPlayingVel[0]
                     patch{0x3375AE, 0x1E2B1E0, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_vel_address}, // smPlayingVel[0]
                     patch{0x337D5F, 0x1E2B1E0, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_vel_address}, // smPlayingVel[0]
                  },
            },

            patch_set{
               .name = "Lightsaber Block Direction Fix",
               .patches =
                  {
                     // See the modtools set for the full write-up.  WeaponMelee::UpdateFire
                     // (VA 0x68D2C0) passes Deflect's worldPos/dir arguments swapped, so a
                     // saber only blocks another saber while facing the world origin.
                     //   0x68DF9A  LEA ECX,[ESP+0x090]  (hit point) -> pushed as dir
                     //   0x68DFD8  LEA ECX,[ESP+0x0CC]  (row2)      -> pushed as worldPos
                     // The second site executes after the first PUSH, hence its +4 offset.
                     // GOG = Steam + 0x1090 (tools/port_gog.py, score 1.00).
                     patch{0x28D39D, 0x090, 0x0C8, {.file_offset = true}}, // dir  <- attacker forward (matrix row 2)
                     patch{0x28D3DB, 0x0CC, 0x094, {.file_offset = true}}, // pos  <- world-space hit point
                  },
            },

            // The four sets below are transplanted from upstream BF2MemExt.  Every
            // modtools and Steam site was byte-verified against the shipping exes;
            // GOG could not be (no GOG install here), so these offsets are upstream's
            // word.  That is safe rather than reckless: apply_patches verifies every
            // site in a set before writing any of it and skips the whole set on the
            // first mismatch, so a wrong GOG offset costs the feature, not the process.
            patch_set{
               .name = "LOD Limit Extension",
               .patches =
                  {
                     patch{0x2BCD59 + 0x1, 0xc8,    0xc8 * 0xa,    {.file_offset = true}}, // modelClass(0)    maxCount
                     patch{0x2BCD54 + 0x1, 0xc350,  0xc350 * 0xa,  {.file_offset = true}}, // modelClass(0)    LOD0
                     patch{0x2BCD45 + 0x1, 0x9c40,  0x9c40 * 0xa,  {.file_offset = true}}, // modelClass(0)    LOD3
                     patch{0x2BCCC1 + 0x1, 0x258,   0x258 * 0xa,   {.file_offset = true}}, // bigModelClass(1) maxCount
                     patch{0x2BCCBC + 0x1, 0x186a0, 0x186a0 * 0xa, {.file_offset = true}}, // bigModelClass(1) LOD0
                     patch{0x2BCCAD + 0x1, 0x9c40,  0x9c40 * 0xa,  {.file_offset = true}}, // bigModelClass(1) LOD3
                     patch{0x2BCC81 + 0x1, 0x4650,  0x4650 * 0xa,  {.file_offset = true}}, // soldierClass(2)  LOD0
                     patch{0x2BCC75 + 0x1, 0x9c40,  0x9c40 * 0xa,  {.file_offset = true}}, // soldierClass(2)  LOD3
                     patch{0x2BCD11 + 0x1, 0x5dc,   0x5dc * 0xa,   {.file_offset = true}}, // hugeModelClass(3) maxCount, uber
                     patch{0x2BCD0C + 0x1, 0x2710,  0x2710 * 0xa,  {.file_offset = true}}, // hugeModelClass(3) LOD0, uber
                     patch{0x2BCD27 + 0x1, 0x12c,   0x12c * 0xa,   {.file_offset = true}}, // hugeModelClass(3) maxCount
                     patch{0x2BCD22 + 0x1, 0x3e8,   0x3e8 * 0xa,   {.file_offset = true}}, // hugeModelClass(3) LOD0
                     patch{0x2BCCFB + 0x1, 0x9c40,  0x9c40 * 0xa,  {.file_offset = true}}, // hugeModelClass(3) LOD3
                  },
            },


            patch_set{
               .name = "Explosion VisibleRadius Increase",
               .patches =
                  {
                     patch{0x11BF59 + 0x6, 0x42700000, 0x461c4000, {.file_offset = true}}, // 60.0f -> 10000.0f
                  },
            },


            patch_set{
               .name = "Soldier Height Ceiling Removal",
               .patches =
                  {
                     // Retail compiles the same block with the cold path hoisted out of
                     // line, so the ceiling test is `JA` TO the Kill rather than a `JNZ`
                     // around it.  NOP the whole 6-byte branch.
                     //
                     //   004E96D8  0F 2F 1D 40337B00  COMISS XMM3,[0x007B3340]  ; 1000.0f
                     //   004E96DF  0F 87 1C390000     JA -> out-of-line Kill block
                     //
                     // GOG.  Constant verified `00 00 7A 44` at 0x007B3340.
                     patch{0x004E96DF, 0x0F, 0x90, {.values_are_8bit = true}},
                     patch{0x004E96E0, 0x87, 0x90, {.values_are_8bit = true}},
                     patch{0x004E96E1, 0x1C, 0x90, {.values_are_8bit = true}},
                     patch{0x004E96E2, 0x39, 0x90, {.values_are_8bit = true}},
                     patch{0x004E96E3, 0x00, 0x90, {.values_are_8bit = true}},
                     patch{0x004E96E4, 0x00, 0x90, {.values_are_8bit = true}},
                  },
            },

            patch_set{
               .name = "Reverb Restore On Map Exit",
               .patches =
                  {
                     // See the modtools list for why bit 3 has to be re-armed here.
                     // Retail folds the same block differently -- EAX instead of ECX,
                     // and a PUSH 0 hoisted between the OR and the store -- so the
                     // guarded run is `83 C8 06 6A` rather than modtools' `83 C9 06 89`:
                     //
                     //   005398C5  8B 06        MOV EAX,[ESI]
                     //   005398C9  83 E0 FE     AND EAX,0xFFFFFFFE
                     //   005398CC  83 C8 06     OR  EAX,0x6
                     //   005398CF  6A 00        PUSH 0x0
                     //   005398D1  89 06        MOV [ESI],EAX
                     //
                     // GOG.  Ported from Steam with tools/port_gog.py (score 1.00,
                     // shift +0xD70) and the bytes read back from both images.
                     patch{0x005398CC, 0x6A06C883, 0x6A0EC883}, // OR EAX,0x6 -> OR EAX,0xE
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
                     // Value patches (Steam FlushParticleCache uses an EBP frame, so there is no
                     // ADD ESP to patch — but the sort pool is EBP-relative too; see the rebase below)
                     patch{0x20DB09, 0x0000012C, 0x000004B0, {.file_offset = true}},                                                                                             // CacheParticle: CMP EDI, 300 -> 1200
                     patch{0x20DB7A, 0x00000980, 0x000025A0, {.file_offset = true}},                                                                                             // FlushParticleCache: SUB ESP, 0x980 -> 0x25A0
                     patch{0x20DBD9, 0x0000012C, 0x000004B0, {.file_offset = true}},                                                                                             // FlushParticleCache: heap.maxCount 300 -> 1200
                     // FlushParticleCache sort-pool rebase.  Unlike modtools -- whose pool is
                     // ESP-relative (LEA EDX,[ESP+0x34]) and therefore moves down with a larger
                     // SUB ESP -- both retail builds address the pool from EBP
                     // (LEA EDI,[EBP-0x988]).  Growing SUB ESP there only adds unused space
                     // BELOW the pool: capacity stays at the vanilla 301 records while
                     // CacheParticle above is now allowed 1200.  Index 301 then overwrites the
                     // PblHeap object itself, 302 its mPool pointer, 303 the saved SEH handler
                     // at EBP-0xC, 304 the SEH trylevel, and 305 the saved EBP and RETURN
                     // ADDRESS -- a stack smash on any frame with >300 particles in front of
                     // the camera.
                     //
                     // So move the pool base instead: EBP-0x25A8 holds 1201 records
                     // (idx 0..1200, 1201*8 = 0x2588 bytes) ending exactly at the lowest local,
                     // EBP-0x20.  SUB ESP 0x25A0 above puts ESP at EBP-0x25AC, covering the new
                     // base with the same 4 bytes of slack the vanilla frame had.  Every
                     // EBP-relative reference to the pool is rewritten below; the render loop
                     // and the heap-pop helper reach it through mPool (EBP-0x18), which the
                     // rebased LEA writes, so they need no patch.
                     patch{0x20DBC7, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // LEA EDI,[EBP-0x988]           - pool base
                     patch{0x20DBDF, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // MOV [EBP-0x988],0x7F7FFFFF    - pool[0].mKey sentinel
                     patch{0x20DCA6, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // COMISS XMM3,[EBP+ECX*8-0x988] - sift compare
                     patch{0x20DCB3, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // MOV EAX,[EBP+ECX*8-0x988]     - sift read  .mKey
                     patch{0x20DCBA, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // MOV [EBP+EDX*8-0x988],EAX     - sift write .mKey
                     patch{0x20DCC1, 0xFFFFF67C, 0xFFFFDA5C, {.file_offset = true}}, // MOV EAX,[EBP+ECX*8-0x984]     - sift read  .mObj
                     patch{0x20DCC8, 0xFFFFF67C, 0xFFFFDA5C, {.file_offset = true}}, // MOV [EBP+EDX*8-0x984],EAX     - sift write .mObj
                     patch{0x20DCD4, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // COMISS XMM3,[EBP+ECX*8-0x988] - loop compare
                     patch{0x20DCDF, 0xFFFFF678, 0xFFFFDA58, {.file_offset = true}}, // MOVSS [EBP+EDX*8-0x988],XMM3  - insert .mKey
                     patch{0x20DCEB, 0xFFFFF67C, 0xFFFFDA5C, {.file_offset = true}}, // MOV [EBP+EDX*8-0x984],EAX     - insert .mObj
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
               .name = "Particle Effect Skip Fix",
               .patches =
                  {
                     // RedParticleRenderer::IsFull -- the 200-entry batch test.
                     //
                     // ParticleEmitterObject::Render_ calls IsFull as its first gate and
                     // returns early when it reports full.  That early return lands on the
                     // function's epilogue, which is PAST the FlushParticleCache and
                     // RenderAll calls that end Render_ -- so a full batch does not just
                     // drop one effect, it skips the very drain that would have emptied
                     // the batch.  The batch therefore stays full and every remaining
                     // effect in that pass takes the same early return: one full cache
                     // silently deletes the rest of the frame's particles.  That is the
                     // "whole explosions blink out under load" symptom.
                     //
                     // The threshold is raised past reach instead of being removed.  The
                     // test is SETGE (signed), so 0x7FFFFFFF is never met, and Render_
                     // always runs through to its RenderAll.  Particles past the batch's
                     // real 200 are still refused by AddParticle, which drops them one at
                     // a time -- a partial effect instead of no effect, with the flush
                     // cadence restored for everything drawn after it.
                     //
                     // Single call site on all three builds (verified by xref), so nothing
                     // else depends on this returning true.
                     patch{0x2D24A2, 0xC8, 0x7FFFFFFF, {.file_offset = true}}, // IsFull: CMP [ECX+0x3520],0xC8 -> 0x7FFFFFFF
                  },
            },

            patch_set{
               .name = "Particle Cache Reset Fix",
               .patches =
                  {
                     // RedParticleRenderer::RenderAllCaches leaks its pool state when a
                     // frame's dynamic-mesh acquisition fails.
                     //
                     //   0x006d3131  TEST <mesh>,<mesh> / JZ 0x006d326d   <- bail
                     //   ...
                     //   0x006d3259  s_cacheIndex  = 0                  <- SKIPPED
                     //              currentCache = NULL                   <- SKIPPED
                     //   0x006d326d  POP .. / RET                       <- bail lands here
                     //
                     // The bail jumps PAST the two resets that end the function, so
                     // s_cacheIndex keeps whatever height it had reached and currentCache
                     // stays pointing at a stale cache.  On the following frames
                     // SetCurrentCache starts from that height, walks into its allocation
                     // clamp, and sets currentCache = NULL -- after which EVERY
                     // SubmitParticle in the game silently no-ops.  Particles stay gone
                     // until some later RenderAllCaches happens to complete in full.
                     //
                     // Retargeting the branch to the reset block instead of the epilogue
                     // makes the failure path drop that frame's particles (which it was
                     // doing anyway) without poisoning the next one; it then falls through
                     // into the same epilogue.  The reset block only zeroes two globals, so
                     // reaching it early is safe.
                     //
                     // This is a stock engine bug, not one the cache patches introduce --
                     // but they make it easier to reach, because the spill hook deliberately
                     // uses more caches per frame and therefore requests more meshes.
                     patch{0x2D2533, 0x136, 0x122, {.file_offset = true}}, // JZ 0x006d326d -> 0x006d3259
                  },
            },

            patch_set{
               .name = "EntityPath Branch Region Fix",
               .patches =
                  {
                     // EntityPath::BranchRegionFactory puts its CreateRegion in vtable
                     // slot 3, but LoadUtil::ProcessRegionInfo dispatches through slot 1,
                     // which still holds the inherited base implementation (it builds a
                     // plain RedRegion). The branch creator is therefore never called and
                     // no BranchRegion ever exists, so every path node using
                     // BranchRegion("id") fails to resolve. Same defect on all builds --
                     // Ghidra even labels slot 1 here "RedRegionFactory member function
                     // inherited by EntityPath::BranchRegionFactory".
                     //
                     // Steam vtable 0x0079c440: slot1 0x006dc930 (base) / slot3 -> 0x004d0f00 (branch)
                     //
                     // Retail note: these builds strip the RedWarning text, so the failure
                     // is completely silent there -- no "Unable to find branch region" line.
                     patch{0x0079c444, 0x006dc930, 0x004d0f00, {.values_are_va = true}}, // vtable slot 1 -> BranchRegionFactory::CreateRegion
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
                     // WeaponMelee current-combo-anim helper (VA 0x688b80) — returns hardcoded
                     // 0xA4 as its "no animation" sentinel (MOV AL,0xA4; POP ESI; RET).  Both of
                     // its callers are the EntitySoldier::Render compare sites patched to 0xFE
                     // above, so an unpatched return here makes "no melee anim" read as REAL anim
                     // index 0xA4 -> garbage combo/anim data -> crash whenever a melee unit is
                     // in play.  Modtools inlines this helper into Render, where the four Render
                     // patches already cover its sentinel — Steam keeps it out-of-line, so the
                     // sentinel needs its own patch.
                     patch{0x287FA5, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // melee-anim helper sentinel return (MOV AL, 0xA4)
                     // EntitySoldier::Update — inlined weapon-loop bound check on the melee anim
                     // index (CALL helper@0x688b70; CMP AL,0xA4; JAE skip), gated on
                     // weapon->IsMelee().  Same 0xA4 total-index bound as the Render sites.
                     patch{0xE94F9 + 0x1, 0xa4, 0xfe, {.file_offset = true, .values_are_8bit = true}}, // EntitySoldier::Update (CMP AL, 0xA4)
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

            // Port of PrismaticFlower's fix (upstream 2cb6a11) — see modtools set.
            patch_set{
               .name = "PropGenerator Update Loop Exit Condition",
               .patches =
                  {
                     patch{0x0062aedb, 0x68, 0x42, {.values_are_8bit = true}},  // branch offset -> bounds check
                  },
            },

            // Port of PrismaticFlower's fix (upstream 9c6170e) — see modtools set.
            patch_set{
               .name = "SkyObjectClass Limit Extension",
               .patches =
                  {
                     patch{0x00638d9e, 0x41, 0x0f, {.values_are_8bit = true}},   // INC ECX -> NOP (0F 1F 00)
                     patch{0x00638d9f, 0x89, 0x1f, {.values_are_8bit = true}},
                     patch{0x00638da0, 0x0d, 0x00, {.values_are_8bit = true}},
                     patch{0x00638da1, 0x01eaf068, 0x00401f0f, {.expected_is_va = true}}, // MOV [count],ECX operand -> 4-byte NOP
                     patch{0x00638dc8, 0x40, 0x66, {.values_are_8bit = true}},   // INC EAX -> NOP (66 90)
                     patch{0x00638dc9, 0xa3, 0x90, {.values_are_8bit = true}},
                     patch{0x00638dca, 0x01eaf068, 0x00401f0f, {.expected_is_va = true}}, // MOV [count],EAX operand -> 4-byte NOP
                  },
            },
            // Raise the concurrent Lua OpenAudioStream limit from 6 to
            // AUDIO_STREAM_SLOTS.  Snd::EngineBase::smStreams is a *pointer* to a
            // 6-element BSS array of 0x3611BC-byte Snd::Stream objects, so the
            // array itself relocates cleanly to a DLL buffer; the engine's own
            // ctor/dtor loops in EngineBase::Open/Close then construct and destroy
            // all AUDIO_STREAM_SLOTS of them.  Most loops express the limit as a
            // byte bound (6 * 0x3611BC = 0x1446A68) rather than a count.
            //
            // Snd::SoundStream keeps five arrays indexed by the same slot index
            // (smPlayingProps / smCount / smPlayingPos / smPlayingVel / smQueue),
            // packed into BSS with no slack, so those relocate alongside it -
            // otherwise slot 6+ would write over its neighbour.
            //
            // The `cmp reg, 6` sites listed here are the stream-index bounds only.
            // Several neighbouring `cmp reg, 6` / `cmp reg, 5` in the same Lua
            // callbacks are lua_gettop() argument-count checks and must NOT move.
            patch_set{
               .name = "Audio Stream Limit Increase",
               .prepare = audio_stream_prepare,
               .patches =
                  {
                     patch{0x3338CC, 0x9E2C20, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_storage_address}, // EngineBase::Open: smStreams = smStreamStorage

                     patch{0x137EF4, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00538AF2
                     patch{0x33100B, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00731C09
                     patch{0x333386, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00733F84
                     patch{0x3333F0, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00733FEE
                     patch{0x33351A, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x00734118
                     patch{0x33391F, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x0073451D
                     patch{0x333A30, 0x1446A68, 0x288D4D0, {.file_offset = true}}, // stream array byte bound @ 0x0073462E

                     patch{0x19AF03, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // StopAudioStream: stream handle bound
                     patch{0x19B28F, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // PlayAudioStreamUsingProperties: handle bound
                     patch{0x19B4C5, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // AudioStreamComplete: stream scan bound
                     patch{0x33349C, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // EngineBase::GetFreeStream
                     patch{0x336D3D, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // SoundStream::UpdateAll
                     // SoundStream::StopAll counts *bytes* into smQueue and compares with
                     // `cmp reg, 0x48` — opcode 83, whose imm8 is SIGN-extended.  12 slots
                     // would need 0x90, which reads back as -112 and makes the unsigned JB
                     // loop forever off the end of the array.  The counter is never
                     // dereferenced (smQueue/smCount have their own pointers), so count
                     // slots instead of bytes and both immediates stay small.
                     patch{0x336BE3, 0x0C, 1, {.file_offset = true, .values_are_8bit = true}},  // SoundStream::StopAll: ADD EBX,0xC -> ADD EBX,1
                     patch{0x336BEF, 0x48, AUDIO_STREAM_SLOTS, {.file_offset = true, .values_are_8bit = true}}, // SoundStream::StopAll: CMP EBX,72 -> CMP EBX,slots

                     patch{0xAD68, 5, 11, {.file_offset = true}}, // smPlayingPos vector ctor count
                     patch{0xAD88, 5, 11, {.file_offset = true}}, // smPlayingVel vector ctor count
                     patch{0xADBB, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // smQueue vector ctor count
                     patch{0x369746, 6, 12, {.file_offset = true, .values_are_8bit = true}}, // smQueue vector dtor count

                     patch{0x335C76, 0x1E297A0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x335CE1, 0x1E297A0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x335CE8, 0x1E297A0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x335D2B, 0x1E297A0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x335EFB, 0x1E297A0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]
                     patch{0x336B17, 0x1E297A0, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_count_address}, // smCount[0]

                     patch{0x335F84, 0x1E297B8, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[0]
                     patch{0x335F8E, 0x1E297BC, 4, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[1]
                     patch{0x335F98, 0x1E297C0, 8, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[2]
                     patch{0x335FA2, 0x1E297C4, 12, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[3]
                     patch{0x335FAC, 0x1E297C8, 16, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[4]
                     patch{0x335FB6, 0x1E297CC, 20, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[5]
                     patch{0x336461, 0x1E297B8, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[0]
                     patch{0x3364B8, 0x1E297B8, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[0]
                     patch{0x336C79, 0x1E297B8, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_props_address}, // smPlayingProps[0]

                     patch{0xAD63, 0x1E297D0, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_pos_address}, // smPlayingPos[0]
                     patch{0x336477, 0x1E297D0, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_pos_address}, // smPlayingPos[0]
                     patch{0x3364C4, 0x1E297D0, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_pos_address}, // smPlayingPos[0]
                     patch{0x336C7F, 0x1E297D0, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_pos_address}, // smPlayingPos[0]

                     patch{0xADBF, 0x1E29818, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x335C10, 0x1E29818, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x335ECD, 0x1E29818, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x336B22, 0x1E29818, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]
                     patch{0x36974A, 0x1E29818, 0, {.file_offset = true, .expected_is_va = true}, &snd_stream_queue_address}, // smQueue[0]

                     patch{0xAD83, 0x1E29D40, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_vel_address}, // smPlayingVel[0]
                     patch{0x33646F, 0x1E29D40, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_vel_address}, // smPlayingVel[0]
                     patch{0x3364BE, 0x1E29D40, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_vel_address}, // smPlayingVel[0]
                     patch{0x336C6F, 0x1E29D40, 0, {.file_offset = true, .expected_is_va = true}, &snd_playing_vel_address}, // smPlayingVel[0]
                  },
            },

            patch_set{
               .name = "Lightsaber Block Direction Fix",
               .patches =
                  {
                     // See the modtools set for the full write-up.  WeaponMelee::UpdateFire
                     // (VA 0x68C230) passes Deflect's worldPos/dir arguments swapped, so a
                     // saber only blocks another saber while facing the world origin.
                     //   0x68CF0A  LEA ECX,[ESP+0x090]  (hit point) -> pushed as dir
                     //   0x68CF48  LEA ECX,[ESP+0x0CC]  (row2)      -> pushed as worldPos
                     // The second site executes after the first PUSH, hence its +4 offset.
                     patch{0x28C30D, 0x090, 0x0C8, {.file_offset = true}}, // dir  <- attacker forward (matrix row 2)
                     patch{0x28C34B, 0x0CC, 0x094, {.file_offset = true}}, // pos  <- world-space hit point
                  },
            },

            patch_set{
               .name = "LOD Limit Extension",
               .patches =
                  {
                     // RedLodManager::SetClassMaxCost(class, maxCount, cost0..cost3).
                     // Retail multiplier is 10x (upstream BF2MemExt uses 10x on both
                     // retail builds and 20x on modtools); kept identical so the two
                     // tables stay diffable against upstream.
                     patch{0x2BBCC9 + 0x1, 0xc8,    0xc8 * 0xa,    {.file_offset = true}}, // modelClass(0)    maxCount
                     patch{0x2BBCC4 + 0x1, 0xc350,  0xc350 * 0xa,  {.file_offset = true}}, // modelClass(0)    LOD0
                     patch{0x2BBCB5 + 0x1, 0x9c40,  0x9c40 * 0xa,  {.file_offset = true}}, // modelClass(0)    LOD3
                     patch{0x2BBC31 + 0x1, 0x258,   0x258 * 0xa,   {.file_offset = true}}, // bigModelClass(1) maxCount
                     patch{0x2BBC2C + 0x1, 0x186a0, 0x186a0 * 0xa, {.file_offset = true}}, // bigModelClass(1) LOD0
                     patch{0x2BBC1D + 0x1, 0x9c40,  0x9c40 * 0xa,  {.file_offset = true}}, // bigModelClass(1) LOD3
                     patch{0x2BBBF1 + 0x1, 0x4650,  0x4650 * 0xa,  {.file_offset = true}}, // soldierClass(2)  LOD0
                     patch{0x2BBBE5 + 0x1, 0x9c40,  0x9c40 * 0xa,  {.file_offset = true}}, // soldierClass(2)  LOD3
                     patch{0x2BBC81 + 0x1, 0x5dc,   0x5dc * 0xa,   {.file_offset = true}}, // hugeModelClass(3) maxCount, uber
                     patch{0x2BBC7C + 0x1, 0x2710,  0x2710 * 0xa,  {.file_offset = true}}, // hugeModelClass(3) LOD0, uber
                     patch{0x2BBC97 + 0x1, 0x12c,   0x12c * 0xa,   {.file_offset = true}}, // hugeModelClass(3) maxCount
                     patch{0x2BBC92 + 0x1, 0x3e8,   0x3e8 * 0xa,   {.file_offset = true}}, // hugeModelClass(3) LOD0
                     patch{0x2BBC6B + 0x1, 0x9c40,  0x9c40 * 0xa,  {.file_offset = true}}, // hugeModelClass(3) LOD3
                     // soldierClass maxCount (0x2BBBF6+1, imm8 0x64) is deliberately NOT
                     // patched — see the modtools block for why it cannot matter.
                  },
            },


            patch_set{
               .name = "Explosion VisibleRadius Increase",
               .patches =
                  {
                     patch{0x11BF59 + 0x6, 0x42700000, 0x461c4000, {.file_offset = true}}, // 60.0f -> 10000.0f
                  },
            },


            patch_set{
               .name = "Soldier Height Ceiling Removal",
               .patches =
                  {
                     // Steam.  Byte-identical to GOG apart from the constant's address.
                     // Constant verified `00 00 7A 44` at 0x007B23C8.
                     //
                     //   004E96D8  0F 2F 1D C8237B00  COMISS XMM3,[0x007B23C8]  ; 1000.0f
                     //   004E96DF  0F 87 1C390000     JA -> out-of-line Kill block
                     patch{0x004E96DF, 0x0F, 0x90, {.values_are_8bit = true}},
                     patch{0x004E96E0, 0x87, 0x90, {.values_are_8bit = true}},
                     patch{0x004E96E1, 0x1C, 0x90, {.values_are_8bit = true}},
                     patch{0x004E96E2, 0x39, 0x90, {.values_are_8bit = true}},
                     patch{0x004E96E3, 0x00, 0x90, {.values_are_8bit = true}},
                     patch{0x004E96E4, 0x00, 0x90, {.values_are_8bit = true}},
                  },
            },

            patch_set{
               .name = "Reverb Restore On Map Exit",
               .patches =
                  {
                     // See the modtools list for why bit 3 has to be re-armed here.
                     // Steam.  Byte-identical to GOG at the site; only the address
                     // differs.
                     //
                     //   00538B55  8B 06        MOV EAX,[ESI]
                     //   00538B59  83 E0 FE     AND EAX,0xFFFFFFFE
                     //   00538B5C  83 C8 06     OR  EAX,0x6
                     //   00538B5F  6A 00        PUSH 0x0
                     //   00538B61  89 06        MOV [ESI],EAX
                     patch{0x00538B5C, 0x6A06C883, 0x6A0EC883}, // OR EAX,0x6 -> OR EAX,0xE
                  },
            },

         },
   },
};
