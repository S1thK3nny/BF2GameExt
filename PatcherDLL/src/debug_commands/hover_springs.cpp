#include "pch.h"
#include "hover_springs.hpp"
#include "weapon_ranges.hpp"
#include "command_registry.hpp"

#include <detours.h>

// =============================================================================
// HoverSprings
//
// Per-spring-body debug visualization for EntityHover vehicles.
// Caches spring data during PostCollisionUpdate, draws in both play
// mode and freecam via separate hooks.
// =============================================================================

// Hook target addresses (modtools-specific)
static constexpr uintptr_t kPostCollUpdate  = 0x00514490;
static constexpr uintptr_t kFreeCamUpdate   = 0x004ae1b0; // FreeCamera::Update

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

static uint32_t compression_color(float c) {
   if (c < 0.f) c = 0.f;
   if (c > 1.f) c = 1.f;
   return DebugCommand::makeColor((uint8_t)(c * 255.f), (uint8_t)((1.f - c) * 255.f), 0);
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

      int bodyPtr = DebugCommand::findBody(collModel, nullptr, bodyId);
      if (!bodyPtr) {
         cb.radius = 0.f;
         cb.pos[0] = cb.pos[1] = cb.pos[2] = 0.f;
         continue;
      }

      float xform[16] = {};
      DebugCommand::getWorldXform(bodyPtr, nullptr, xform, entMat, 0);
      cb.pos[0] = xform[12];
      cb.pos[1] = xform[13];
      cb.pos[2] = xform[14];
      cb.radius = DebugCommand::getRadius(bodyPtr, nullptr);
      if (cb.radius < 0.05f) cb.radius = 0.1f;
   }
}

// ---------------------------------------------------------------------------
// Drawing — renders all cached hovers
// ---------------------------------------------------------------------------

static void draw_all_cached()
{
   uint32_t colWhite = DebugCommand::makeColor(255, 255, 255);

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
         DebugCommand::drawSphere(bx, by, bz, cb.radius, col);

         // White line = spring length (from sphere center)
         DebugCommand::drawLine3D(bx, by, bz, bx, by - cb.springLen, bz, colWhite);

         // Colored fill line = compression
         if (cb.compression > 0.01f) {
            float fill = cb.springLen * cb.compression;
            DebugCommand::drawLine3D(bx + 0.05f, by, bz, bx + 0.05f, by - fill, bz, col);
         }

         // Labels (2 lines per body: 12 per hover, fits 5 hovers in the 64 limit)
         int lenW = (int)cb.springLen;
         int lenD = ((int)(cb.springLen * 10.f)) % 10;
         if (lenD < 0) lenD = -lenD;

         float tp0[3] = { bx, by + cb.radius + 0.5f, bz };
         float tp1[3] = { bx, by + cb.radius + 0.3f, bz };
         DebugCommand::printf3D(tp0, "S%d Len=%d.%d", i, lenW, lenD);
         DebugCommand::printf3D(tp1, "V%d P%d R%d",
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
   WeaponRanges::freecamTick();
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------

void HoverSprings::install(uintptr_t exe_base)
{
   s_origPostCollUpdate = (PostCollUpdate_t)resolve(exe_base, kPostCollUpdate);
   s_origFreeCamUpdate  = (FreeCamUpdate_t) resolve(exe_base, kFreeCamUpdate);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)s_origPostCollUpdate, hooked_PostCollUpdate);
   DetourAttach(&(PVOID&)s_origFreeCamUpdate,  hooked_FreeCamUpdate);
   DetourTransactionCommit();
}

void HoverSprings::lateInit()
{
   DebugCommandRegistry::addBool("RenderHoverSprings", &s_enabled);
}

void HoverSprings::uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (s_origPostCollUpdate) DetourDetach(&(PVOID&)s_origPostCollUpdate, hooked_PostCollUpdate);
   if (s_origFreeCamUpdate)  DetourDetach(&(PVOID&)s_origFreeCamUpdate,  hooked_FreeCamUpdate);
   DetourTransactionCommit();
}
