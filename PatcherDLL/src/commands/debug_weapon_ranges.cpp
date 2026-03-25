#include "pch.h"
#include "debug_weapon_ranges.hpp"
#include "debug_command_registry.hpp"

#include <detours.h>
#include <cmath>

// =============================================================================
// ShowWeaponRanges
//
// Per-soldier weapon range visualization for AI debugging.
// Caches active weapon range data during PostCollisionUpdate,
// draws in both play mode and freecam.
// =============================================================================

static constexpr uintptr_t kBase = 0x400000u;
static inline void* res(uintptr_t exe_base, uintptr_t addr) {
   return (void*)((addr - kBase) + exe_base);
}

// ---------------------------------------------------------------------------
// Engine function types
// ---------------------------------------------------------------------------

typedef void(__cdecl* DrawLine3D_t)(float, float, float, float, float, float, uint32_t);
typedef void(__cdecl* Printf3D_t)(const float*, const char*, ...);

// ---------------------------------------------------------------------------
// Resolved pointers
// ---------------------------------------------------------------------------

static DrawLine3D_t s_drawLine3D = nullptr;
static Printf3D_t  s_printf3D   = nullptr;

static constexpr uintptr_t kDrawLine3D = 0x007e96b0;
static constexpr uintptr_t kPrintf3D   = 0x007e9fd0;

// Hook targets
static constexpr uintptr_t kSoldierPCU = 0x00530B20;  // EntitySoldier::PostCollisionUpdate

// ---------------------------------------------------------------------------
// EntitySoldier struct offsets (from PCU's ECX = struct_base + 0x258)
// ---------------------------------------------------------------------------

static constexpr int kPCU_to_struct   = 0x258;  // ECX - this = struct_base
static constexpr int kStruct_PosX     = 0x120;  // world position (matrix translation)
static constexpr int kStruct_PosY     = 0x124;
static constexpr int kStruct_PosZ     = 0x128;
static constexpr int kStruct_Entity   = 0x240;  // entity = struct_base + 0x240

// Offsets from entity pointer
static constexpr int kEnt_WeaponArray = 0x4F0;  // Weapon*[8]
static constexpr int kEnt_ActiveSlot  = 0x512;  // uint8 active weapon slot

// Weapon offsets
static constexpr int kWpn_Class       = 0x060;  // WeaponClass*

// WeaponClass offsets (PDB-confirmed)
static constexpr int kWC_MinRange     = 0x100;  // float mMinRange
static constexpr int kWC_OptimalRange = 0x104;  // float mOptimalRange
static constexpr int kWC_MaxRange     = 0x108;  // float mMaxRange

// ---------------------------------------------------------------------------
// Console toggle
// ---------------------------------------------------------------------------

static bool s_enabled = false;

// ---------------------------------------------------------------------------
// Range data cache — filled by PostCollisionUpdate, drawn by both hooks
// ---------------------------------------------------------------------------

struct CachedRanges {
   bool  active;
   float pos[3];
   float minRange, optimalRange, maxRange;
   float innerBand, outerBand;
};

static constexpr int kMaxCached = 8;
static CachedRanges s_cache[kMaxCached];
static int   s_cacheCount    = 0;
static DWORD s_lastClearTick = 0;

// ---------------------------------------------------------------------------
// Color helpers (RedColor: b | g<<8 | r<<16 | a<<24 on little-endian x86)
// ---------------------------------------------------------------------------

static uint32_t make_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
   return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24);
}

// ---------------------------------------------------------------------------
// Draw a flat circle on the XZ plane using line segments
// ---------------------------------------------------------------------------

static void draw_circle_xz(float cx, float cy, float cz, float radius,
                            uint32_t color, int segments = 24)
{
   if (radius <= 0.01f) return;
   constexpr float PI2 = 6.283185307f;
   float step = PI2 / (float)segments;
   float px = cx + radius;
   float pz = cz;
   for (int i = 1; i <= segments; ++i) {
      float angle = step * (float)i;
      float nx = cx + radius * cosf(angle);
      float nz = cz + radius * sinf(angle);
      s_drawLine3D(px, cy, pz, nx, cy, nz, color);
      px = nx;
      pz = nz;
   }
}

// ---------------------------------------------------------------------------
// Cache population — reads weapon range data from a live EntitySoldier
// ---------------------------------------------------------------------------

static void cache_soldier(char* ecx)
{
   char* structBase = ecx - kPCU_to_struct;

   float posX = *(float*)(structBase + kStruct_PosX);
   float posY = *(float*)(structBase + kStruct_PosY);
   float posZ = *(float*)(structBase + kStruct_PosZ);

   // entity pointer (embedded at struct_base + 0x240)
   char* entity = structBase + kStruct_Entity;

   // Active weapon slot
   uint8_t activeSlot = *(uint8_t*)(entity + kEnt_ActiveSlot);
   if (activeSlot > 7) return;

   // Weapon pointer from entity-side array
   void* weapon = *(void**)(entity + kEnt_WeaponArray + activeSlot * 4);
   if (!weapon) return;

   // WeaponClass
   char* wc = *(char**)((char*)weapon + kWpn_Class);
   if (!wc) return;

   float minR = *(float*)(wc + kWC_MinRange);
   float optR = *(float*)(wc + kWC_OptimalRange);
   float maxR = *(float*)(wc + kWC_MaxRange);

   // Skip if all ranges are zero (no weapon data)
   if (maxR <= 0.f && optR <= 0.f && minR <= 0.f) return;

   // Compute AI comfort band:
   //   inner = OptimalRange - (OptimalRange - MinRange) * 0.2
   //   outer = OptimalRange + (MaxRange - OptimalRange) * 0.2
   float inner = optR - (optR - minR) * 0.2f;
   float outer = optR + (maxR - optR) * 0.2f;

   // Clear cache once per frame (tick-based)
   DWORD now = GetTickCount();
   if (now != s_lastClearTick) {
      s_lastClearTick = now;
      for (int i = 0; i < kMaxCached; ++i) s_cache[i].active = false;
      s_cacheCount = 0;
   }

   if (s_cacheCount >= kMaxCached) return;

   CachedRanges& cr = s_cache[s_cacheCount++];
   cr.active       = true;
   cr.pos[0]       = posX;
   cr.pos[1]       = posY;
   cr.pos[2]       = posZ;
   cr.minRange     = minR;
   cr.optimalRange = optR;
   cr.maxRange     = maxR;
   cr.innerBand    = inner;
   cr.outerBand    = outer;
}

// ---------------------------------------------------------------------------
// Drawing — renders all cached range circles
// ---------------------------------------------------------------------------

static void draw_all_cached()
{
   uint32_t colRed    = make_color(255, 60,  60);   // MinRange
   uint32_t colGreen  = make_color(60,  255, 60);   // OptimalRange
   uint32_t colBlue   = make_color(60,  120, 255);  // MaxRange
   uint32_t colYellow = make_color(255, 255, 60);   // AI band

   for (int i = 0; i < kMaxCached; ++i) {
      CachedRanges& cr = s_cache[i];
      if (!cr.active) continue;

      float x = cr.pos[0], y = cr.pos[1] + 0.3f, z = cr.pos[2];

      // Range circles (raised slightly above ground to avoid z-fighting)
      if (cr.minRange > 0.01f)
         draw_circle_xz(x, y, z, cr.minRange, colRed, 20);
      draw_circle_xz(x, y, z, cr.optimalRange, colGreen, 32);
      draw_circle_xz(x, y, z, cr.maxRange, colBlue, 28);

      // AI comfort band
      if (cr.innerBand > 0.01f)
         draw_circle_xz(x, y, z, cr.innerBand, colYellow, 16);
      draw_circle_xz(x, y, z, cr.outerBand, colYellow, 16);

      // Labels at cardinal directions on each ring
      float minLabelR = cr.minRange > 0.01f ? cr.minRange : 1.f;
      float lm[3] = { x + minLabelR, y + 1.f, z };
      s_printf3D(lm, "Min %.0f", cr.minRange);

      float lo[3] = { x, y + 1.f, z + cr.optimalRange };
      s_printf3D(lo, "Optimal %.0f", cr.optimalRange);

      float lx[3] = { x - cr.maxRange, y + 1.f, z };
      s_printf3D(lx, "Max %.0f", cr.maxRange);

      float lb[3] = { x, y + 1.f, z - cr.outerBand };
      s_printf3D(lb, "Band %.0f-%.0f", cr.innerBand, cr.outerBand);
   }
}

// ---------------------------------------------------------------------------
// Hook 1: EntitySoldier::PostCollisionUpdate — cache + draw during play
//
// Signature: void __thiscall PCU(float* outParam, float dt)
// Returns via RET 0x8 (2 stack params, callee-cleanup).
// ---------------------------------------------------------------------------

typedef void(__fastcall* SoldierPCU_t)(void* ecx, void* edx, float* outParam, float dt);
static SoldierPCU_t s_origSoldierPCU = nullptr;

static void __fastcall hooked_SoldierPCU(void* ecx, void* edx, float* outParam, float dt)
{
   s_origSoldierPCU(ecx, edx, outParam, dt);

   if (s_enabled) {
      __try {
         cache_soldier((char*)ecx);
      } __except (EXCEPTION_EXECUTE_HANDLER) {}

      __try {
         draw_all_cached();
      } __except (EXCEPTION_EXECUTE_HANDLER) {}
   }
}

// ---------------------------------------------------------------------------
// Refresh cache by scanning the character array directly.
// Used in freecam where SoldierPCU doesn't fire.
// ---------------------------------------------------------------------------

static constexpr uintptr_t kCharArrayPtr = 0xB93A08;  // *(uintptr_t*) = charArray base
static constexpr uintptr_t kMaxCharsPtr  = 0xB939F4;  // *(int*)       = max character count
static constexpr int kCharStride         = 0x1B0;
static constexpr int kChar_Intermediate  = 0x148;

static uintptr_t s_exeBase = 0;

static void refresh_cache_from_char_array()
{
   if (!s_exeBase) return;

   auto addr = [](uintptr_t a) -> uintptr_t { return (a - kBase) + s_exeBase; };

   uintptr_t arrayBase = *(uintptr_t*)addr(kCharArrayPtr);
   int maxChars = *(int*)addr(kMaxCharsPtr);
   if (!arrayBase || maxChars <= 0) return;

   // Build into temp buffer — only commit if we find at least one character
   CachedRanges temp[kMaxCached] = {};
   int tempCount = 0;

   for (int i = 0; i < maxChars && tempCount < kMaxCached; ++i) {
      __try {
         uintptr_t slot = arrayBase + (uintptr_t)i * kCharStride;
         void* intermediate = *(void**)(slot + kChar_Intermediate);
         if (!intermediate) continue;

         char* ctrl = (char*)intermediate + 0x018;
         char* entity = *(char**)(ctrl + 0x290);
         if (!entity) continue;

         char* structBase = entity - kStruct_Entity;

         float posX = *(float*)(structBase + kStruct_PosX);
         float posY = *(float*)(structBase + kStruct_PosY);
         float posZ = *(float*)(structBase + kStruct_PosZ);

         uint8_t activeSlot = *(uint8_t*)(entity + kEnt_ActiveSlot);
         if (activeSlot > 7) continue;

         void* weapon = *(void**)(entity + kEnt_WeaponArray + activeSlot * 4);
         if (!weapon) continue;

         char* wc = *(char**)((char*)weapon + kWpn_Class);
         if (!wc) continue;

         float minR = *(float*)(wc + kWC_MinRange);
         float optR = *(float*)(wc + kWC_OptimalRange);
         float maxR = *(float*)(wc + kWC_MaxRange);
         if (maxR <= 0.f && optR <= 0.f && minR <= 0.f) continue;

         float inner = optR - (optR - minR) * 0.2f;
         float outer = optR + (maxR - optR) * 0.2f;

         CachedRanges& cr = temp[tempCount++];
         cr.active       = true;
         cr.pos[0]       = posX;
         cr.pos[1]       = posY;
         cr.pos[2]       = posZ;
         cr.minRange     = minR;
         cr.optimalRange = optR;
         cr.maxRange     = maxR;
         cr.innerBand    = inner;
         cr.outerBand    = outer;
      } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
   }

   // Only commit if we found valid characters — otherwise keep stale cache
   if (tempCount > 0) {
      memcpy(s_cache, temp, sizeof(s_cache));
      s_cacheCount = tempCount;
   }
}

// ---------------------------------------------------------------------------
// Freecam tick — called from the shared FreeCamera::Update hook
// TODO: Weapon switches don't update in freecam. refresh_cache_from_char_array
//       tries to re-read the character array but the pointer chain fails silently.
//       Need to find the correct freecam-safe pointer path to the active weapon.
// ---------------------------------------------------------------------------

void debug_weapon_ranges_freecam_tick()
{
   if (!s_enabled) return;

   __try {
      refresh_cache_from_char_array();
   } __except (EXCEPTION_EXECUTE_HANDLER) {}

   __try {
      draw_all_cached();
   } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// Install / lateInit / uninstall
// ---------------------------------------------------------------------------

void debug_weapon_ranges_install(uintptr_t exe_base)
{
   s_exeBase    = exe_base;
   s_drawLine3D = (DrawLine3D_t)res(exe_base, kDrawLine3D);
   s_printf3D   = (Printf3D_t) res(exe_base, kPrintf3D);

   s_origSoldierPCU = (SoldierPCU_t)res(exe_base, kSoldierPCU);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)s_origSoldierPCU, hooked_SoldierPCU);
   DetourTransactionCommit();
}

void debug_weapon_ranges_late_init()
{
   DebugCommandRegistry::addBool("showweaponranges", &s_enabled);
}

void debug_weapon_ranges_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (s_origSoldierPCU) DetourDetach(&(PVOID&)s_origSoldierPCU, hooked_SoldierPCU);
   DetourTransactionCommit();
}
