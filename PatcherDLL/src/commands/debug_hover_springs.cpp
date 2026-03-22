#include "pch.h"
#include "debug_hover_springs.hpp"
#include "debug_weapon_ranges.hpp"
#include "../debug_command_registry.hpp"

#include <detours.h>

// =============================================================================
// RenderHoverSprings
//
// Per-spring-body debug visualization for EntityHover vehicles.
// Caches spring data during PostCollisionUpdate, draws in both play
// mode and freecam via separate hooks.
// =============================================================================

static constexpr uintptr_t kBase = 0x400000u;
static inline void* res(uintptr_t exe_base, uintptr_t addr) {
   return (void*)((addr - kBase) + exe_base);
}

// ---------------------------------------------------------------------------
// Engine function types
// ---------------------------------------------------------------------------

typedef void(__cdecl* DrawLine3D_t)(float, float, float, float, float, float, uint32_t);
typedef void(__cdecl* DrawSphere_t)(float, float, float, float, uint32_t);
typedef void(__cdecl* Printf3D_t)(const float*, const char*, ...);
typedef int(__fastcall* FindBody_t)(int collModel, void* edx, int bodyId);
typedef void(__fastcall* GetWorldXform_t)(int bodyPtr, void* edx, float* out, const float* entMat, int flags);
typedef float(__fastcall* GetRadius_t)(int bodyPtr, void* edx);

// ---------------------------------------------------------------------------
// Resolved pointers
// ---------------------------------------------------------------------------

static DrawLine3D_t    s_drawLine3D   = nullptr;
static DrawSphere_t    s_drawSphere   = nullptr;
static Printf3D_t     s_printf3D     = nullptr;
static FindBody_t      s_findBody     = nullptr;
static GetWorldXform_t s_getWorldXform = nullptr;
static GetRadius_t     s_getRadius    = nullptr;

// Addresses (confirmed in Ghidra)
static constexpr uintptr_t kPostCollUpdate  = 0x00514490;
static constexpr uintptr_t kFreeCamUpdate   = 0x004ae1b0; // FreeCamera::Update
static constexpr uintptr_t kDrawLine3D      = 0x007e96b0;
static constexpr uintptr_t kDrawSphere      = 0x007ea240;
static constexpr uintptr_t kPrintf3D        = 0x007e9fd0;
static constexpr uintptr_t kFindBody        = 0x00435830;
static constexpr uintptr_t kGetWorldXform   = 0x00428a20;
static constexpr uintptr_t kGetRadius       = 0x00428260;

// EntityHover struct offsets (this = ECX in PostCollisionUpdate)
static constexpr int kOff_CollModel   = 0x54;
static constexpr int kOff_EntMatrix   = 0xF0;
static constexpr int kOff_Class       = 0x4C4;
static constexpr int kOff_SpringBase  = 0x1CA0;
static constexpr int kSpringStride    = 0x14;

// EntityHoverClass offsets
static constexpr int kCls_BodyInfo    = 0xDE8;
static constexpr int kCls_BodyStride  = 0x20;
static constexpr int kCls_NumBodies   = 0xEA8;
static constexpr int kBody_Id         = 0x00;
static constexpr int kBody_SpringLen  = 0x04;
static constexpr int kBody_VelF       = 0x08;
static constexpr int kBody_OmXF      = 0x10;
static constexpr int kBody_OmZF      = 0x18;

// ---------------------------------------------------------------------------
// Console toggle
// ---------------------------------------------------------------------------

static bool s_enabled = false;

// ---------------------------------------------------------------------------
// Spring data cache — filled by PostCollisionUpdate, drawn by both hooks
// ---------------------------------------------------------------------------

struct CachedBody {
   float pos[3];
   float radius;
   float compression;
   float springLen;
   float velF, omXF, omZF;
};

struct CachedHover {
   bool  active;
   int   numBodies;
   CachedBody bodies[6];
};

static constexpr int kMaxHovers = 16;
static CachedHover s_cache[kMaxHovers];
static int s_cacheWrite = 0;       // next write slot (round-robin)
static DWORD s_lastClearTick = 0;  // for per-frame cache clear

// ---------------------------------------------------------------------------
// Color helpers (RedColor: r | g<<8 | b<<16 | a<<24 on little-endian x86)
// ---------------------------------------------------------------------------

// RedColor is BGRA in memory (engine byte order)
static uint32_t make_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
   return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24);
}

static uint32_t compression_color(float c) {
   if (c < 0.f) c = 0.f;
   if (c > 1.f) c = 1.f;
   return make_color((uint8_t)(c * 255.f), (uint8_t)((1.f - c) * 255.f), 0);
}

// ---------------------------------------------------------------------------
// Cache population — reads spring data from a live EntityHover
// ---------------------------------------------------------------------------

static void cache_hover(char* hover)
{
   char* cls = *(char**)(hover + kOff_Class);
   if (!cls) return;

   int numBodies = *(int*)(cls + kCls_NumBodies);
   if (numBodies <= 0 || numBodies > 6) return;

   int collModel = *(int*)(hover + kOff_CollModel);
   if (!collModel) return;

   const float* entMat = (const float*)(hover + kOff_EntMatrix);

   // Clear cache once per frame (coarse tick-based)
   DWORD now = GetTickCount();
   if (now != s_lastClearTick) {
      s_lastClearTick = now;
      for (int i = 0; i < kMaxHovers; ++i) s_cache[i].active = false;
      s_cacheWrite = 0;
   }

   // Find or allocate cache slot
   int slot = s_cacheWrite;
   if (slot >= kMaxHovers) return;
   s_cacheWrite++;

   CachedHover& ch = s_cache[slot];
   ch.active = true;
   ch.numBodies = numBodies;

   for (int i = 0; i < numBodies; ++i) {
      char* bi = cls + kCls_BodyInfo + i * kCls_BodyStride;
      int bodyId = *(int*)(bi + kBody_Id);

      CachedBody& cb = ch.bodies[i];
      cb.springLen   = *(float*)(bi + kBody_SpringLen);
      cb.velF        = *(float*)(bi + kBody_VelF);
      cb.omXF        = *(float*)(bi + kBody_OmXF);
      cb.omZF        = *(float*)(bi + kBody_OmZF);
      cb.compression = *(float*)(hover + kOff_SpringBase + i * kSpringStride);

      int bodyPtr = s_findBody(collModel, nullptr, bodyId);
      if (!bodyPtr) {
         cb.radius = 0.f;
         cb.pos[0] = cb.pos[1] = cb.pos[2] = 0.f;
         continue;
      }

      float xform[16] = {};
      s_getWorldXform(bodyPtr, nullptr, xform, entMat, 0);
      cb.pos[0] = xform[12];
      cb.pos[1] = xform[13];
      cb.pos[2] = xform[14];
      cb.radius = s_getRadius(bodyPtr, nullptr);
      if (cb.radius < 0.05f) cb.radius = 0.1f;
   }
}

// ---------------------------------------------------------------------------
// Drawing — renders all cached hovers
// ---------------------------------------------------------------------------

static void draw_all_cached()
{
   uint32_t colWhite = make_color(255, 255, 255);

   for (int h = 0; h < kMaxHovers; ++h) {
      CachedHover& ch = s_cache[h];
      if (!ch.active) continue;

      for (int i = 0; i < ch.numBodies; ++i) {
         CachedBody& cb = ch.bodies[i];
         if (cb.radius <= 0.f && cb.pos[0] == 0.f && cb.pos[1] == 0.f && cb.pos[2] == 0.f)
            continue;

         float bx = cb.pos[0], by = cb.pos[1], bz = cb.pos[2];
         uint32_t col = compression_color(cb.compression);

         // Sphere at body position
         s_drawSphere(bx, by, bz, cb.radius, col);

         // White line = spring length (from sphere center)
         s_drawLine3D(bx, by, bz, bx, by - cb.springLen, bz, colWhite);

         // Colored fill line = compression
         if (cb.compression > 0.01f) {
            float fill = cb.springLen * cb.compression;
            s_drawLine3D(bx + 0.05f, by, bz, bx + 0.05f, by - fill, bz, col);
         }

         // Labels (2 lines per body: 12 per hover, fits 5 hovers in the 64 limit)
         int lenW = (int)cb.springLen;
         int lenD = ((int)(cb.springLen * 10.f)) % 10;
         if (lenD < 0) lenD = -lenD;

         float tp0[3] = { bx, by + cb.radius + 0.5f, bz };
         float tp1[3] = { bx, by + cb.radius + 0.3f, bz };
         s_printf3D(tp0, "S%d Len=%d.%d", i, lenW, lenD);
         s_printf3D(tp1, "V%d P%d R%d",
                    (int)cb.velF, (int)cb.omXF, (int)cb.omZF);
      }
   }
}

// ---------------------------------------------------------------------------
// Hook 1: EntityHover::PostCollisionUpdate — cache + draw during play
// ---------------------------------------------------------------------------

typedef int(__fastcall* PostCollUpdate_t)(void* ecx, void* edx, float dt);
static PostCollUpdate_t s_origPostCollUpdate = nullptr;

static int __fastcall hooked_PostCollUpdate(void* ecx, void* edx, float dt)
{
   int result = s_origPostCollUpdate(ecx, edx, dt);

   if (s_enabled) {
      __try {
         cache_hover((char*)ecx);
      } __except (EXCEPTION_EXECUTE_HANDLER) {}

      // Draw during play mode
      __try {
         draw_all_cached();
      } __except (EXCEPTION_EXECUTE_HANDLER) {}
   }

   return result;
}

// ---------------------------------------------------------------------------
// Hook 2: FreeCamera::Update — draw cached data during freecam
// ---------------------------------------------------------------------------

typedef void(__fastcall* FreeCamUpdate_t)(void* ecx, void* edx, float dt);
static FreeCamUpdate_t s_origFreeCamUpdate = nullptr;

static void __fastcall hooked_FreeCamUpdate(void* ecx, void* edx, float dt)
{
   s_origFreeCamUpdate(ecx, edx, dt);

   if (s_enabled) {
      __try {
         draw_all_cached();
      } __except (EXCEPTION_EXECUTE_HANDLER) {}
   }

   // Other commands that need freecam drawing
   debug_weapon_ranges_freecam_tick();
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------

void debug_hover_springs_install(uintptr_t exe_base)
{
   s_drawLine3D    = (DrawLine3D_t)     res(exe_base, kDrawLine3D);
   s_drawSphere    = (DrawSphere_t)     res(exe_base, kDrawSphere);
   s_printf3D      = (Printf3D_t)      res(exe_base, kPrintf3D);
   s_findBody      = (FindBody_t)       res(exe_base, kFindBody);
   s_getWorldXform = (GetWorldXform_t)  res(exe_base, kGetWorldXform);
   s_getRadius     = (GetRadius_t)      res(exe_base, kGetRadius);

   s_origPostCollUpdate = (PostCollUpdate_t)res(exe_base, kPostCollUpdate);
   s_origFreeCamUpdate  = (FreeCamUpdate_t) res(exe_base, kFreeCamUpdate);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)s_origPostCollUpdate, hooked_PostCollUpdate);
   DetourAttach(&(PVOID&)s_origFreeCamUpdate,  hooked_FreeCamUpdate);
   DetourTransactionCommit();
}

void debug_hover_springs_late_init()
{
   DebugCommandRegistry::addBool("RenderHoverSprings", &s_enabled);
}

void debug_hover_springs_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (s_origPostCollUpdate) DetourDetach(&(PVOID&)s_origPostCollUpdate, hooked_PostCollUpdate);
   if (s_origFreeCamUpdate)  DetourDetach(&(PVOID&)s_origFreeCamUpdate,  hooked_FreeCamUpdate);
   DetourTransactionCommit();
}
