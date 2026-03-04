#include "pch.h"

#include "lod_limit_patch.hpp"
#include "cfile.hpp"
#include "lua_hooks.hpp"

#include <detours.h>
#include <string.h>
#include <math.h>

// ============================================================================
// 1. Per-build addresses (VA - 0x400000)
// ============================================================================

struct lod_addrs {
   // Build identification
   uintptr_t id_file_offset;
   uint64_t  id_expected;

   // --- Near-scene: FLRenderer::Init PUSH imm32 patch sites ---
   // Each is the offset of the imm32 within a PUSH instruction (VA+1 of the PUSH opcode).
   // Class 0 "model"
   struct { uintptr_t count, costLOD0, costLOD1, costLOD2, costLOD3; } cls0;
   // Class 1 "bigModel"
   struct { uintptr_t count, costLOD0, costLOD1, costLOD2, costLOD3; } cls1;
   // Class 2 "soldier" (count is PUSH imm8 — not patchable, only costs)
   struct { uintptr_t costLOD0, costLOD2, costLOD3; } cls2;
   // Class 3 "hugeModel" normal
   struct { uintptr_t count, costLOD0, costLOD1, costLOD2; } cls3n;
   // Class 3 "hugeModel" uber
   struct { uintptr_t count, costLOD0, costLOD1, costLOD2; } cls3u;
   // Shared costLOD3 for class 3 (both modes)
   uintptr_t cls3_costLOD3;

   // --- Far-scene: SetupStaticWorld binary patches ---
   uintptr_t far_heap_alloc_size;   // imm32 for pool allocation (modtools: PUSH size, Steam: MOV EAX count)
   uint32_t  far_heap_alloc_old;    // expected old value (modtools: 0x1008, Steam: 0x201)
   uint32_t  far_heap_alloc_new;    // new replacement value (modtools: 0x8008, Steam: 0x1001)
   uintptr_t far_heap_max_count;    // MOV dword imm32 for mMaxCount

   // --- Far-scene: RenderFarObjects Detours hook ---
   uintptr_t fn_RenderFarObjects;

   // Functions called by RenderFarObjects
   uintptr_t fn_GetMinScreenSize;
   uintptr_t fn_GetFadeAdjustWithZoom;
   uintptr_t fn_GetCamera;
   uintptr_t fn_GetNearSceneFadeStart;
   uintptr_t fn_GetFarSceneRange;
   uintptr_t fn_PblHeapPop;

   // Globals accessed by RenderFarObjects
   uintptr_t g_sFarSceneObjects;
   uintptr_t g_gRenderCount;
   uintptr_t g_sMaxNumFarObjects;
   uintptr_t g_sRedSceneCameraPos;    // float[3] (x, y, z)
   uintptr_t g_sRedSceneCameraFront;  // float[3] (x, y, z)

   // Camera zoom offset within RedCamera struct
   uint32_t camera_zoom_offset;

   // RedSceneObject struct offsets
   uint32_t obj_sphere_pos;      // Sphere.mRenderPosition (float[3])
   uint32_t obj_sphere_radius;   // Sphere.mRenderRadius (float)
   uint32_t obj_pLodData;        // _pLodData (ptr)
   uint32_t obj_renderFlags;     // _uiRenderFlags (uint)

   // RedLodData struct offsets
   uint32_t lod_class;           // _LodClass (int)
   uint32_t lod_lowRezFlags;     // mLowRezFlags (byte)
};

// clang-format off
static const lod_addrs MODTOOLS = {
   .id_file_offset = 0x62b59c,
   .id_expected    = 0x746163696c707041,

   .cls0 = { 0x41d456, 0x41d451, 0x41d44c, 0x41d447, 0x41d442 },
   .cls1 = { 0x41d3c1, 0x41d3bc, 0x41d3b7, 0x41d3b2, 0x41d3ad },
   .cls2 = { 0x41d388, 0x41d380, 0x41d37b },
   .cls3n = { 0x41d427, 0x41d422, 0x41d41d, 0x41d418 },
   .cls3u = { 0x41d411, 0x41d40c, 0x41d407, 0x41d402 },
   .cls3_costLOD3 = 0x41d3fb,

   .far_heap_alloc_size = 0x3f07bc,
   .far_heap_alloc_old  = 0x1008,
   .far_heap_alloc_new  = 0x8008,
   .far_heap_max_count  = 0x3f07cf,

   .fn_RenderFarObjects     = 0x3efa10,
   .fn_GetMinScreenSize     = 0x415150,
   .fn_GetFadeAdjustWithZoom = 0x41afb0,
   .fn_GetCamera            = 0x4057f0,
   .fn_GetNearSceneFadeStart = 0x41ae80,
   .fn_GetFarSceneRange     = 0x41af40,
   .fn_PblHeapPop           = 0x3ef360,

   .g_sFarSceneObjects      = 0x92bcd0,
   .g_gRenderCount          = 0x92bcbc,
   .g_sMaxNumFarObjects     = 0x6e22f4,
   .g_sRedSceneCameraPos    = 0x92bcd8,
   .g_sRedSceneCameraFront  = 0x92bd34,

   .camera_zoom_offset = 0x140,

   .obj_sphere_pos    = 0x30,
   .obj_sphere_radius = 0x3C,
   .obj_pLodData      = 0x40,
   .obj_renderFlags   = 0x44,

   .lod_class       = 0x14,
   .lod_lowRezFlags = 0x18,
};

static const lod_addrs STEAM = {
   .id_file_offset = 0x39f834,   // RVA of "Application" string (not a raw file offset)
   .id_expected    = 0x746163696c707041,

   .cls0 = { 0x2BC8CA, 0x2BC8C5, 0x2BC8C0, 0x2BC8BB, 0x2BC8B6 },
   .cls1 = { 0x2BC832, 0x2BC82D, 0x2BC828, 0x2BC823, 0x2BC81E },
   .cls2 = { 0x2BC7F2, 0x2BC7EB, 0x2BC7E6 },
   .cls3n = { 0x2BC898, 0x2BC893, 0x2BC88E, 0x2BC889 },
   .cls3u = { 0x2BC882, 0x2BC87D, 0x2BC878, 0x2BC873 },
   .cls3_costLOD3 = 0x2BC86C,

   .far_heap_alloc_size = 0x2E4289,   // MOV EAX, 0x201 (count before *8)
   .far_heap_alloc_old  = 0x201,
   .far_heap_alloc_new  = 0x1001,  // * 8 at runtime = 0x8008
   .far_heap_max_count  = 0x2E42AE,

   .fn_RenderFarObjects      = 0x2E2ED0,
   .fn_GetMinScreenSize      = 0x317370,
   .fn_GetFadeAdjustWithZoom = 0x2BC640,
   .fn_GetCamera             = 0x2B1130,
   .fn_GetNearSceneFadeStart = 0x2BC710,
   .fn_GetFarSceneRange      = 0x2BC650,
   .fn_PblHeapPop            = 0x2E2A90,

   .g_sFarSceneObjects     = 0x5A0888,
   .g_gRenderCount         = 0x5A0878,
   .g_sMaxNumFarObjects    = 0x3DF954,
   .g_sRedSceneCameraPos   = 0x5A08B0,
   .g_sRedSceneCameraFront = 0x5A090C,

   .camera_zoom_offset = 0x140,

   .obj_sphere_pos    = 0x30,
   .obj_sphere_radius = 0x3C,
   .obj_pLodData      = 0x40,
   .obj_renderFlags   = 0x44,

   .lod_class       = 0x14,
   .lod_lowRezFlags = 0x18,
};

static const lod_addrs GOG = {
   .id_file_offset = 0x3a0698,   // RVA of "Application" string (not a raw file offset)
   .id_expected    = 0x746163696c707041,

   .cls0 = { 0x2BD95A, 0x2BD955, 0x2BD950, 0x2BD94B, 0x2BD946 },
   .cls1 = { 0x2BD8C2, 0x2BD8BD, 0x2BD8B8, 0x2BD8B3, 0x2BD8AE },
   .cls2 = { 0x2BD882, 0x2BD87B, 0x2BD876 },
   .cls3n = { 0x2BD928, 0x2BD923, 0x2BD91E, 0x2BD919 },
   .cls3u = { 0x2BD912, 0x2BD90D, 0x2BD908, 0x2BD903 },
   .cls3_costLOD3 = 0x2BD8FC,

   .far_heap_alloc_size = 0x2E5329,   // MOV EAX, 0x201 (count before *8)
   .far_heap_alloc_old  = 0x201,
   .far_heap_alloc_new  = 0x1001,     // * 8 at runtime = 0x8008
   .far_heap_max_count  = 0x2E534E,

   .fn_RenderFarObjects      = 0x2E3F70,
   .fn_GetMinScreenSize      = 0x318460,
   .fn_GetFadeAdjustWithZoom = 0x2BD6D0,
   .fn_GetCamera             = 0x2B21B0,
   .fn_GetNearSceneFadeStart = 0x2BD7A0,
   .fn_GetFarSceneRange      = 0x2BD6E0,
   .fn_PblHeapPop            = 0x2E3B30,

   .g_sFarSceneObjects     = 0x5A1D28,
   .g_gRenderCount         = 0x5A1D18,
   .g_sMaxNumFarObjects    = 0x3E0954,
   .g_sRedSceneCameraPos   = 0x5A1D50,
   .g_sRedSceneCameraFront = 0x5A1DAC,

   .camera_zoom_offset = 0x140,

   .obj_sphere_pos    = 0x30,
   .obj_sphere_radius = 0x3C,
   .obj_pLodData      = 0x40,
   .obj_renderFlags   = 0x44,

   .lod_class       = 0x14,
   .lod_lowRezFlags = 0x18,
};
// clang-format on

// ============================================================================
// 2. New limits (8x vanilla)
// ============================================================================

// Near-scene heap counts
static constexpr uint32_t NEW_CLS0_COUNT       = 2048;    // 200 * ~10x
static constexpr uint32_t NEW_CLS0_COST_LOD0   = 400000;  // 50000 * 8
static constexpr uint32_t NEW_CLS0_COST_LOD1   = 24000;   // 3000 * 8
static constexpr uint32_t NEW_CLS0_COST_LOD2   = 24000;   // 3000 * 8
static constexpr uint32_t NEW_CLS0_COST_LOD3   = 320000;  // 40000 * 8

static constexpr uint32_t NEW_CLS1_COUNT       = 4096;    // 600 * ~7x
static constexpr uint32_t NEW_CLS1_COST_LOD0   = 800000;  // 100000 * 8
static constexpr uint32_t NEW_CLS1_COST_LOD1   = 176000;  // 22000 * 8
static constexpr uint32_t NEW_CLS1_COST_LOD2   = 176000;  // 22000 * 8
static constexpr uint32_t NEW_CLS1_COST_LOD3   = 320000;  // 40000 * 8

static constexpr uint32_t NEW_CLS2_COST_LOD0   = 144000;  // 18000 * 8
static constexpr uint32_t NEW_CLS2_COST_LOD2   = 8000;    // 1000 * 8
static constexpr uint32_t NEW_CLS2_COST_LOD3   = 320000;  // 40000 * 8

static constexpr uint32_t NEW_CLS3N_COUNT      = 2400;    // 300 * 8
static constexpr uint32_t NEW_CLS3N_COST       = 8000;    // 1000 * 8

static constexpr uint32_t NEW_CLS3U_COUNT      = 8192;    // 1500 * ~5x
static constexpr uint32_t NEW_CLS3U_COST       = 80000;   // 10000 * 8

static constexpr uint32_t NEW_CLS3_COST_LOD3   = 320000;  // 40000 * 8

// Far-scene
static constexpr uint32_t NEW_FAR_HEAP_MAX     = 4096;
static constexpr uint32_t NEW_FAR_HEAP_ALLOC   = NEW_FAR_HEAP_MAX * 8 + 8;  // 0x8008
static constexpr int      NEW_FAR_MAX_OBJECTS  = 2048;
static constexpr int      NEW_FAR_MAX_CLASS2   = 80;

// ============================================================================
// 3. Helper: patch a 4-byte immediate at a given offset
// ============================================================================

static int patch_imm32(uintptr_t exe_base, uintptr_t offset, uint32_t old_val, uint32_t new_val,
                       cfile& log, const char* name)
{
   if (!offset) return 0;  // address not available for this build

   uint32_t* addr = (uint32_t*)(offset + exe_base);
   if (*addr != old_val) {
      log.printf("  WARNING: %s mismatch at %p (expected 0x%X, got 0x%X)\n",
                 name, addr, old_val, *addr);
      return 0;
   }
   *addr = new_val;
   return 1;
}

// ============================================================================
// 4. Near-scene patches
// ============================================================================

static int apply_near_scene_patches(uintptr_t exe_base, const lod_addrs& a, cfile& log)
{
   int count = 0;

   log.printf("  Class 0 (model):\n");
   count += patch_imm32(exe_base, a.cls0.count,   0xC8,    NEW_CLS0_COUNT,     log, "count");
   count += patch_imm32(exe_base, a.cls0.costLOD0, 0xC350,  NEW_CLS0_COST_LOD0, log, "costLOD0");
   count += patch_imm32(exe_base, a.cls0.costLOD1, 0xBB8,   NEW_CLS0_COST_LOD1, log, "costLOD1");
   count += patch_imm32(exe_base, a.cls0.costLOD2, 0xBB8,   NEW_CLS0_COST_LOD2, log, "costLOD2");
   count += patch_imm32(exe_base, a.cls0.costLOD3, 0x9C40,  NEW_CLS0_COST_LOD3, log, "costLOD3");

   log.printf("  Class 1 (bigModel):\n");
   count += patch_imm32(exe_base, a.cls1.count,   0x258,   NEW_CLS1_COUNT,     log, "count");
   count += patch_imm32(exe_base, a.cls1.costLOD0, 0x186A0, NEW_CLS1_COST_LOD0, log, "costLOD0");
   count += patch_imm32(exe_base, a.cls1.costLOD1, 0x55F0,  NEW_CLS1_COST_LOD1, log, "costLOD1");
   count += patch_imm32(exe_base, a.cls1.costLOD2, 0x55F0,  NEW_CLS1_COST_LOD2, log, "costLOD2");
   count += patch_imm32(exe_base, a.cls1.costLOD3, 0x9C40,  NEW_CLS1_COST_LOD3, log, "costLOD3");

   log.printf("  Class 2 (soldier) — costs only:\n");
   count += patch_imm32(exe_base, a.cls2.costLOD0, 0x4650, NEW_CLS2_COST_LOD0, log, "costLOD0");
   count += patch_imm32(exe_base, a.cls2.costLOD2, 0x3E8,  NEW_CLS2_COST_LOD2, log, "costLOD2");
   count += patch_imm32(exe_base, a.cls2.costLOD3, 0x9C40, NEW_CLS2_COST_LOD3, log, "costLOD3");

   log.printf("  Class 3 (hugeModel) normal:\n");
   count += patch_imm32(exe_base, a.cls3n.count,   0x12C, NEW_CLS3N_COUNT, log, "count");
   count += patch_imm32(exe_base, a.cls3n.costLOD0, 0x3E8, NEW_CLS3N_COST,  log, "costLOD0");
   count += patch_imm32(exe_base, a.cls3n.costLOD1, 0x3E8, NEW_CLS3N_COST,  log, "costLOD1");
   count += patch_imm32(exe_base, a.cls3n.costLOD2, 0x3E8, NEW_CLS3N_COST,  log, "costLOD2");

   log.printf("  Class 3 (hugeModel) uber:\n");
   count += patch_imm32(exe_base, a.cls3u.count,   0x5DC,  NEW_CLS3U_COUNT, log, "count");
   count += patch_imm32(exe_base, a.cls3u.costLOD0, 0x2710, NEW_CLS3U_COST,  log, "costLOD0");
   count += patch_imm32(exe_base, a.cls3u.costLOD1, 0x2710, NEW_CLS3U_COST,  log, "costLOD1");
   count += patch_imm32(exe_base, a.cls3u.costLOD2, 0x2710, NEW_CLS3U_COST,  log, "costLOD2");

   log.printf("  Class 3 shared costLOD3:\n");
   count += patch_imm32(exe_base, a.cls3_costLOD3, 0x9C40, NEW_CLS3_COST_LOD3, log, "costLOD3");

   return count;
}

// ============================================================================
// 5. Far-scene binary patches (SetupStaticWorld)
// ============================================================================

static int apply_far_scene_patches(uintptr_t exe_base, const lod_addrs& a, cfile& log)
{
   int count = 0;

   count += patch_imm32(exe_base, a.far_heap_alloc_size, a.far_heap_alloc_old, a.far_heap_alloc_new,
                        log, "far heap alloc");
   count += patch_imm32(exe_base, a.far_heap_max_count, 0x200, NEW_FAR_HEAP_MAX, log,
                        "far heap maxCount");

   return count;
}

// ============================================================================
// 6. RenderFarObjects Detours hook
// ============================================================================

// --- Resolved function pointers ---
using fn_RenderFarObjects    = void(__cdecl*)();
using fn_GetMinScreenSize    = float(__cdecl*)();
using fn_GetFadeAdjustZoom   = bool(__cdecl*)();
using fn_GetCamera           = char*(__cdecl*)();
using fn_GetNearFadeStart    = float(__cdecl*)();
using fn_GetFarRange         = float(__cdecl*)();
using fn_HeapPop             = void(__thiscall*)(void* heap);

// Virtual render call: __thiscall(this, int mode, float opacity, uint flags)
using fn_SceneObjectRender   = void(__thiscall*)(char* self, int mode, float opacity, uint32_t flags);

static fn_RenderFarObjects  original_RenderFarObjects = nullptr;
static fn_GetMinScreenSize  game_GetMinScreenSize     = nullptr;
static fn_GetFadeAdjustZoom game_GetFadeAdjustZoom    = nullptr;
static fn_GetCamera         game_GetCamera            = nullptr;
static fn_GetNearFadeStart  game_GetNearFadeStart     = nullptr;
static fn_GetFarRange       game_GetFarRange          = nullptr;
static fn_HeapPop           game_HeapPop              = nullptr;

// --- Resolved globals ---
struct PblHeapRecord { float key; char* obj; };
struct PblHeap { int mCount; int mMaxCount; PblHeapRecord* mPool; };

static PblHeap** g_sFarSceneObjects  = nullptr;
static int*      g_gRenderCount      = nullptr;
static int*      g_sMaxNumFarObjects = nullptr;
static float*    g_camPos            = nullptr;  // float[3]
static float*    g_camFront          = nullptr;  // float[3]

// --- Resolved struct offsets ---
static uint32_t s_cameraZoomOff  = 0;
static uint32_t s_objSpherePos   = 0;
static uint32_t s_objSphereRad   = 0;
static uint32_t s_objLodData     = 0;
static uint32_t s_objRenderFlags = 0;
static uint32_t s_lodClass       = 0;
static uint32_t s_lodLowRezFlags = 0;

// --- Static object array (replaces stack-allocated 256-entry array) ---
static char* s_farObjects[NEW_FAR_MAX_OBJECTS];  // 2048 * 4 = 8KB

// --- Constants ---
static constexpr float FADE_DIST    = 20.0f;
static constexpr float FADE_SLOPE   = 0.05f;   // 1/20
static constexpr float SCREEN_COEFF = 1.2520032e-06f;

// --- Inline helpers ---

static inline float obj_pos_x(char* obj)   { return *(float*)(obj + s_objSpherePos); }
static inline float obj_pos_y(char* obj)   { return *(float*)(obj + s_objSpherePos + 4); }
static inline float obj_pos_z(char* obj)   { return *(float*)(obj + s_objSpherePos + 8); }
static inline float obj_radius(char* obj)  { return *(float*)(obj + s_objSphereRad); }
static inline char* obj_loddata(char* obj) { return *(char**)(obj + s_objLodData); }
static inline uint32_t obj_flags(char* obj){ return *(uint32_t*)(obj + s_objRenderFlags); }
static inline int   lod_class(char* ld)    { return *(int*)(ld + s_lodClass); }
static inline uint8_t lod_flags(char* ld)  { return *(uint8_t*)(ld + s_lodLowRezFlags); }

static inline float cam_dot_dist(char* obj, float radius)
{
   float dx = obj_pos_x(obj) - g_camPos[0];
   float dy = obj_pos_y(obj) - g_camPos[1];
   float dz = obj_pos_z(obj) - g_camPos[2];
   return dx * g_camFront[0] + dy * g_camFront[1] + dz * g_camFront[2] + radius;
}

static inline void render_object(char* obj, int mode, float opacity, uint32_t flags)
{
   auto vtable = *(uintptr_t**)obj;
   auto renderFn = (fn_SceneObjectRender)vtable[0x4C / 4];
   renderFn(obj, mode, opacity, flags);
}

// ============================================================================
// 7. Hooked RenderFarObjects
// ============================================================================

static void __cdecl hooked_RenderFarObjects()
{
   PblHeap* heap = *g_sFarSceneObjects;
   if (!heap) return;

   // Get minimum screen-size threshold
   float minScreen = game_GetMinScreenSize();
   float screenThreshold = minScreen * minScreen * SCREEN_COEFF;

   // Cap sMaxNumFarObjects
   int maxObjects = *g_sMaxNumFarObjects;
   if (maxObjects >= NEW_FAR_MAX_OBJECTS) {
      maxObjects = NEW_FAR_MAX_OBJECTS;
      *g_sMaxNumFarObjects = maxObjects;
   }

   // Pop objects from heap into static array
   int objectCount = 0;
   int maxClass2 = NEW_FAR_MAX_CLASS2;

   while (objectCount < maxObjects && heap->mCount > 0) {
      char* obj = heap->mPool[1].obj;
      game_HeapPop(heap);

      // Check class-2 limit
      char* ld = obj_loddata(obj);
      if (lod_class(ld) == 2) {
         if (maxClass2 < 1) {
            // Reject: don't store, don't increment
            continue;
         }
         maxClass2--;
      }

      s_farObjects[objectCount++] = obj;
   }

   // Clear heap
   heap->mCount = 0;

   // Update global render count
   *g_gRenderCount += objectCount;

   // Get camera zoom
   float cameraZoom = 1.0f;
   if (game_GetFadeAdjustZoom()) {
      char* camera = game_GetCamera();
      cameraZoom = *(float*)(camera + s_cameraZoomOff);
   }

   float nearFadeStart = game_GetNearFadeStart() * cameraZoom;
   float farRange      = game_GetFarRange();

   // === Forward pass (front-to-back): immediate render for near/far overlap ===
   for (int i = 0; i < objectCount; i++) {
      char* obj = s_farObjects[i];
      if (!obj) continue;

      float radius = obj_radius(obj);
      float dist   = cam_dot_dist(obj, radius);
      float farEdge = farRange + radius;

      // Cull: too far
      if (dist >= farEdge) {
         s_farObjects[i] = nullptr;
         continue;
      }

      // Cull: too small on screen
      if (radius * radius / (dist * dist) < screenThreshold) {
         s_farObjects[i] = nullptr;
         continue;
      }

      // Check if in the far-edge fade zone (within 20 units of far boundary)
      float fadeZoneStart = farEdge - FADE_DIST;
      if (dist < fadeZoneStart) {
         // Not in far fade zone — check near fade zone for immediate render
         char* ld = obj_loddata(obj);
         uint8_t flags = lod_flags(ld);

         if (flags & 2) {
            // Has crossfade flag — render if within overlap zone
            if (dist >= nearFadeStart && dist < nearFadeStart + FADE_DIST) {
               // In near/far overlap — leave for reverse pass with fade
               continue;
            }
            if (dist >= nearFadeStart) {
               // Past near fade start — render immediately
               render_object(obj, 3, 1.0f, obj_flags(obj) | 0x20000);
               s_farObjects[i] = nullptr;
            }
         } else {
            // No crossfade — render if past near fade start
            if (dist >= nearFadeStart) {
               render_object(obj, 3, 1.0f, obj_flags(obj) | 0x20000);
               s_farObjects[i] = nullptr;
            } else {
               s_farObjects[i] = nullptr;  // before near start, cull
            }
         }
      }
      // Objects in the far fade zone are left for the reverse pass
   }

   // === Reverse pass (back-to-front): fade-blended render ===
   for (int i = objectCount - 1; i >= 0; i--) {
      char* obj = s_farObjects[i];
      if (!obj) continue;

      float radius = obj_radius(obj);
      float dist   = cam_dot_dist(obj, radius);

      // Near fade: ramp up from nearFadeStart
      float fadeScale = (dist - nearFadeStart) * FADE_SLOPE;
      if (fadeScale >= 1.0f) fadeScale = 1.0f;

      // Far fade: ramp down near far boundary
      float farEdge = farRange + radius;
      float farFadeStart = farEdge - FADE_DIST;
      if (dist > farFadeStart) {
         fadeScale *= (farEdge - dist) * FADE_SLOPE;
      }

      render_object(obj, 3, fadeScale, obj_flags(obj) | 0x20000);
   }
}

// ============================================================================
// 8. Detours install / uninstall
// ============================================================================

static bool g_hooks_installed = false;

static bool install_hook(uintptr_t exe_base, const lod_addrs& a, cfile& log)
{
   if (!a.fn_RenderFarObjects) {
      log.printf("LodLimit: RenderFarObjects address not available, skipping hook\n");
      return true;
   }

   // Resolve function pointers
   original_RenderFarObjects = (fn_RenderFarObjects)(a.fn_RenderFarObjects + exe_base);
   game_GetMinScreenSize     = (fn_GetMinScreenSize)(a.fn_GetMinScreenSize + exe_base);
   game_GetFadeAdjustZoom    = (fn_GetFadeAdjustZoom)(a.fn_GetFadeAdjustWithZoom + exe_base);
   game_GetCamera            = (fn_GetCamera)(a.fn_GetCamera + exe_base);
   game_GetNearFadeStart     = (fn_GetNearFadeStart)(a.fn_GetNearSceneFadeStart + exe_base);
   game_GetFarRange          = (fn_GetFarRange)(a.fn_GetFarSceneRange + exe_base);
   game_HeapPop              = (fn_HeapPop)(a.fn_PblHeapPop + exe_base);

   // Resolve globals
   g_sFarSceneObjects  = (PblHeap**)(a.g_sFarSceneObjects + exe_base);
   g_gRenderCount      = (int*)(a.g_gRenderCount + exe_base);
   g_sMaxNumFarObjects = (int*)(a.g_sMaxNumFarObjects + exe_base);
   g_camPos            = (float*)(a.g_sRedSceneCameraPos + exe_base);
   g_camFront          = (float*)(a.g_sRedSceneCameraFront + exe_base);

   // Resolve struct offsets
   s_cameraZoomOff  = a.camera_zoom_offset;
   s_objSpherePos   = a.obj_sphere_pos;
   s_objSphereRad   = a.obj_sphere_radius;
   s_objLodData     = a.obj_pLodData;
   s_objRenderFlags = a.obj_renderFlags;
   s_lodClass       = a.lod_class;
   s_lodLowRezFlags = a.lod_lowRezFlags;

   // Install Detours hook
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_RenderFarObjects, hooked_RenderFarObjects);
   LONG result = DetourTransactionCommit();

   if (result != NO_ERROR) {
      log.printf("LodLimit: Detours commit FAILED (%ld)\n", result);
      return false;
   }

   g_hooks_installed = true;
   log.printf("LodLimit: RenderFarObjects hook installed (max %d far objects)\n", NEW_FAR_MAX_OBJECTS);
   return true;
}

static void uninstall_hook()
{
   if (!g_hooks_installed) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_RenderFarObjects)
      DetourDetach(&(PVOID&)original_RenderFarObjects, hooked_RenderFarObjects);
   DetourTransactionCommit();

   g_hooks_installed = false;
}

// ============================================================================
// 9. Public API
// ============================================================================

bool patch_lod_limits(uintptr_t exe_base)
{
   cfile log{"BF2GameExt.log", "a"};
   if (!log) return false;

   log.printf("\n--- LOD Limit Increase ---\n");

   static const lod_addrs* builds[] = { &MODTOOLS, &STEAM, &GOG };

   for (const lod_addrs* build : builds) {
      char* id_addr = (char*)(build->id_file_offset + exe_base);
      if (memcmp(id_addr, &build->id_expected, sizeof(build->id_expected)) != 0)
         continue;

      log.printf("LodLimit: identified build\n");

      int nearCount = apply_near_scene_patches(exe_base, *build, log);
      log.printf("LodLimit: %d near-scene patches applied\n", nearCount);

      int farCount = apply_far_scene_patches(exe_base, *build, log);
      log.printf("LodLimit: %d far-scene patches applied\n", farCount);

      if (!install_hook(exe_base, *build, log))
         return false;

      return true;
   }

   log.printf("LodLimit: no matching build found, skipping\n");
   return true;
}

void unpatch_lod_limits()
{
   uninstall_hook();
}
