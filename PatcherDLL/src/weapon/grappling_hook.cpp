#include "pch.h"
#include "grappling_hook.hpp"

#include <detours.h>
#include <cmath>

// =============================================================================
// Grappling Hook Fix — v6
//
// The engine's original grapple arrival code writes to a soldier field that
// causes irreversible 3P model invisibility. We redirect the soldier handle
// to a dummy buffer so the engine never touches the real soldier, and
// implement custom pull logic.
//
// Features:
//   - Custom pull with configurable speed (ODF: PullSpeed)
//   - Slingshot mechanic: press jump mid-pull to launch with momentum
//   - Rope cable rendering using OrdnanceTowCable's spline pipeline
//   - Configurable max range (ODF: MaxRange on ordnance)
// =============================================================================

static constexpr uintptr_t kUnrelocatedBase = 0x400000u;

static inline void* resolve(uintptr_t exe_base, uintptr_t unrelocated_addr)
{
   return (void*)((unrelocated_addr - kUnrelocatedBase) + exe_base);
}

typedef void(__cdecl* GameLog_t)(const char* fmt, ...);
static GameLog_t get_gamelog()
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   return (GameLog_t)((0x7E3D50 - kUnrelocatedBase) + base);
}

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------

static constexpr uintptr_t kUpdate_addr       = 0x0060f380;
static constexpr uintptr_t kDtor_addr         = 0x0060ef90;
static constexpr uintptr_t kRemoveBody_addr   = 0x0042ac60;
static constexpr uintptr_t kAddItemBody_addr  = 0x0042dd00;
static constexpr uintptr_t kRttiHash_addr     = 0x00b7e098;
static constexpr uintptr_t kFUN005297b0_addr  = 0x005297b0;

static constexpr uintptr_t kCheckFire_addr    = 0x0062c760;
static constexpr uintptr_t kTriggerUpdate_addr = 0x00562dd0; // Trigger::Update(this, dt, buttonDown)
static constexpr uintptr_t kOrdRender_addr     = 0x0060fb80; // OrdnanceGrapplingHook RSO Render
static constexpr uintptr_t kSetProperty_addr   = 0x0060EC60; // OrdnanceGrapplingHookClass::SetProperty
static constexpr uintptr_t kHashString_addr    = 0x007E1B70; // PblHash::PblHash(const char*)

// Spline/cable rendering (same pipeline as OrdnanceTowCable)
static constexpr uintptr_t kSplineBuild_addr   = 0x0083e720;
static constexpr uintptr_t kCableRender_addr   = 0x006d2370;
static constexpr uintptr_t kVecScale_addr      = 0x004294b0;

// Ordnance offsets
static constexpr int kOrd_SoldierPtr  = 0x54;
static constexpr int kOrd_SoldierKey  = 0x58;
static constexpr int kOrd_Position    = 0x48;   // PblVector3 (hook attachment point)
static constexpr int kOrd_State       = 0x12C;
static constexpr int kOrd_ClassPtr    = 0x30;   // OrdnanceClass* pointer

// Soldier offsets (from struct_base)
// The confirmed runtime offsets were found by hooking Trigger::Update
// (0x00562dd0) and matching trigger pointers during gameplay.
// Confirmed triggers: +0x284=Jump, +0x2A8=ThrustFwd, +0x2AC=ThrustBack,
//                     +0x2B0=StrafeLeft, +0x2B4=StrafeRight
static constexpr int kSol_CollBody    = 0x0C;
static constexpr int kSol_Position    = 0x120;  // PblVector3 world position
static constexpr int kSol_HandleKey   = 0x204;
static constexpr int kSol_FlagByte    = 0x489;  // animation state flags
static constexpr int kSol_JumpTrigger = 0x284;  // Controllable::mControlJump (runtime-confirmed)
static constexpr int kSol_VelocityX   = 0x4EC;
static constexpr int kSol_VelocityY   = 0x4F0;
static constexpr int kSol_VelocityZ   = 0x4F4;

// Defaults (overridable via ODF)
static constexpr float kDefaultPullSpeed   = 15.0f;
static constexpr float kDefaultMaxRange    = 0.0f;  // 0 = unlimited (LifeSpan controls)

static constexpr int kState_Pulling   = 2;
static constexpr float kArrivalDist   = 3.5f;
static constexpr float kMaxPullTime   = 10.0f;
static constexpr int kMaxStuckFrames  = 30;

// ---------------------------------------------------------------------------
// Function types
// ---------------------------------------------------------------------------

typedef unsigned int (__fastcall* fn_Update_t)(void* ecx, void* edx, float dt);
typedef void (__fastcall* fn_Dtor_t)(void* ecx, void* edx);
typedef unsigned int (__cdecl* fn_RemoveBody_t)(void* body);
typedef unsigned int (__cdecl* fn_AddItemBody_t)(void* body);
typedef bool (__fastcall* fn_IsRtti_t)(void* ecx, void* edx, uint32_t hash);
typedef void (__fastcall* fn_005297b0_t)(void* soldier, void* edx, int value);
typedef unsigned int (__fastcall* fn_CheckFire_t)(void* weapon, void* edx);
typedef void (__fastcall* fn_TriggerUpdate_t)(uint32_t* trigger, void* edx, uint32_t dt, char buttonDown);
typedef void (__fastcall* fn_OrdRender_t)(void* rso, void* edx, uint32_t p2, uint32_t p3, uint32_t p4);
typedef void (__fastcall* fn_SetProperty_t)(void* ecx, void* edx, uint32_t hash, const char* value);
typedef uint32_t (__cdecl* fn_HashString_t)(const char*);

// Spline builder: __thiscall — ECX = output, RET 0x14 cleans 5 stack params
typedef void (__fastcall* fn_SplineBuild_t)(float* coefs_out, void* edx, float* start, float* end, float* tan_start, float* tan_end, float length);
typedef void (__cdecl* fn_CableRender_t)(float* coefs, uint32_t param2, uint32_t param3, float cable_width);
typedef float* (__cdecl* fn_VecScale_t)(float* out, float scalar, float* vec);

// ---------------------------------------------------------------------------
// Resolved pointers
// ---------------------------------------------------------------------------

static fn_Update_t      original_Update    = nullptr;
static fn_Dtor_t        original_Dtor      = nullptr;
static fn_RemoveBody_t  fn_RemoveBody      = nullptr;
static fn_AddItemBody_t fn_AddItemBody     = nullptr;
static fn_005297b0_t    original_005297b0  = nullptr;
static fn_CheckFire_t   original_CheckFire = nullptr;
static fn_TriggerUpdate_t original_TriggerUpdate = nullptr;
static fn_OrdRender_t   original_OrdRender = nullptr;
static fn_SetProperty_t original_SetProperty = nullptr;
static fn_HashString_t  fn_HashString      = nullptr;
static fn_SplineBuild_t fn_SplineBuild     = nullptr;
static fn_CableRender_t fn_CableRender     = nullptr;
static fn_VecScale_t    fn_VecScale        = nullptr;
static uint32_t*        g_rttiHashPtr      = nullptr;

// ---------------------------------------------------------------------------
// ODF property hashes (computed at install time)
// ---------------------------------------------------------------------------

static uint32_t g_hashPullSpeed    = 0;
static uint32_t g_hashGrappleRange = 0;

// ODF-configurable values (stored per class, but only one grapple class exists)
static float g_odfPullSpeed = kDefaultPullSpeed;
static float g_odfMaxRange  = kDefaultMaxRange;

// ---------------------------------------------------------------------------
// Hook: OrdnanceGrapplingHookClass::SetProperty
// ---------------------------------------------------------------------------

static void __fastcall hooked_SetProperty(void* ecx, void* /*edx*/,
                                          uint32_t hash, const char* value)
{
   if (hash == g_hashPullSpeed && g_hashPullSpeed != 0 && value && value[0]) {
      g_odfPullSpeed = (float)atof(value);
      if (g_odfPullSpeed < 0.1f) g_odfPullSpeed = kDefaultPullSpeed;
      // Still pass to base class in case it uses a property with the same hash
      original_SetProperty(ecx, nullptr, hash, value);
      return;
   }
   if (hash == g_hashGrappleRange && g_hashGrappleRange != 0 && value && value[0]) {
      g_odfMaxRange = (float)atof(value);
      if (g_odfMaxRange < 0.0f) g_odfMaxRange = 0.0f;
      original_SetProperty(ecx, nullptr, hash, value);
      return;
   }
   original_SetProperty(ecx, nullptr, hash, value);
}

// ---------------------------------------------------------------------------
// Hook: FUN_005297b0 — blocks soldier field write that causes 3P invisibility
// ---------------------------------------------------------------------------

static void __fastcall hooked_005297b0(void* soldier, void* /*edx*/, int value)
{
   if (value != 0) {
      return;
   }
   original_005297b0(soldier, nullptr, value);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool    g_grappleActive  = false;
static bool    g_wasPulling     = false;
static void*   g_soldierPtr     = nullptr;
static int     g_soldierKey     = 0;
static float   g_pullTimer      = 0.0f;
static float   g_lastDist       = 999999.0f;
static int     g_stuckFrames    = 0;
static uint8_t g_savedFlagByte  = 0;
static bool    g_arrivedClean   = false;
static bool    g_slingshotRequested = false;

// Current pull direction (normalized) — used for slingshot momentum
static float g_pullDirX = 0.0f;
static float g_pullDirY = 0.0f;
static float g_pullDirZ = 0.0f;

// Hook head position — cached during Update for the render hook
static float g_hookPosX = 0.0f;
static float g_hookPosY = 0.0f;
static float g_hookPosZ = 0.0f;

// Fire origin — cached when grapple first fires, for max range check
static float g_fireOriginX = 0.0f;
static float g_fireOriginY = 0.0f;
static float g_fireOriginZ = 0.0f;

// Dummy soldier buffer
static uint8_t g_dummySoldier[0x500] = {};

// ---------------------------------------------------------------------------
// Hook: Trigger::Update — intercepts jump button input at the source.
// ---------------------------------------------------------------------------

static void __fastcall hooked_TriggerUpdate(uint32_t* trigger, void* /*edx*/, uint32_t dt, char buttonDown)
{
   if (g_grappleActive && g_soldierPtr && g_wasPulling && buttonDown) {
      uint32_t* jumpTriggerPtr = (uint32_t*)((char*)g_soldierPtr + kSol_JumpTrigger);
      if (trigger == jumpTriggerPtr) {
         g_slingshotRequested = true;
      }
   }
   original_TriggerUpdate(trigger, nullptr, dt, buttonDown);
}

// ---------------------------------------------------------------------------
// Hook: OrdnanceGrapplingHook RSO Render — draws hook model + rope cable.
// ---------------------------------------------------------------------------

static void __fastcall hooked_OrdRender(void* rso, void* /*edx*/, uint32_t p2, uint32_t p3, uint32_t p4)
{
   original_OrdRender(rso, nullptr, p2, p3, p4);

   if (!g_grappleActive || !g_soldierPtr)
      return;
   if (g_hookPosX == 0.0f && g_hookPosY == 0.0f && g_hookPosZ == 0.0f)
      return;

   __try {
      float* solPos = (float*)((char*)g_soldierPtr + kSol_Position);
      // Get cable start from weapon's mFirePointMatrix (hp_fire world position).
      // Same approach as SetBarrelFireOrigin: weapon+0x50 = mFirePointMatrix.trans
      float startPos[3] = { solPos[0], solPos[1] + 1.5f, solPos[2] };  // fallback
      {
         uint8_t slotIdx = *(uint8_t*)((char*)g_soldierPtr + 0x752);
         if (slotIdx < 8) {
            void* weapon = *(void**)((char*)g_soldierPtr + 0x730 + slotIdx * 4);
            if (weapon) {
               float* firePos = (float*)((char*)weapon + 0x50);
               uint32_t raw = *(uint32_t*)&firePos[0];
               if (raw != 0 && raw != 0xCDCDCDCD) {
                  startPos[0] = firePos[0];
                  startPos[1] = firePos[1];
                  startPos[2] = firePos[2];
               }
            }
         }
      }
      float hookPos[3] = { g_hookPosX, g_hookPosY, g_hookPosZ };

      float startTangent[3] = { 0.0f, 0.0f, 0.0f };
      float upDir[3] = { 0.0f, 1.0f, 0.0f };
      float endTangent[3];
      fn_VecScale(endTangent, -15.0f, upDir);

      float coefs[12];
      fn_SplineBuild(coefs, nullptr, startPos, hookPos, startTangent, endTangent, 1.0f);
      fn_CableRender(coefs, p2, p3, 0.3f);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// Hook: WeaponGrapplingHook::CheckFire
// ---------------------------------------------------------------------------

static unsigned int __fastcall hooked_CheckFire(void* weapon, void* /*edx*/)
{
   return original_CheckFire(weapon, nullptr);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static float distance_to_hook(void* soldierPtr, float* hookPos)
{
   float* solPos = (float*)((char*)soldierPtr + kSol_Position);
   float dx = hookPos[0] - solPos[0];
   float dy = hookPos[1] - solPos[1];
   float dz = hookPos[2] - solPos[2];
   return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void move_soldier_toward(void* soldierPtr, float* hookPos, float dt)
{
   float* solPos = (float*)((char*)soldierPtr + kSol_Position);

   float dx = hookPos[0] - solPos[0];
   float dy = hookPos[1] - solPos[1];
   float dz = hookPos[2] - solPos[2];

   float dist = sqrtf(dx * dx + dy * dy + dz * dz);
   if (dist < 0.01f) return;

   g_pullDirX = dx / dist;
   g_pullDirY = dy / dist;
   g_pullDirZ = dz / dist;

   float move = g_odfPullSpeed * dt;
   if (move > dist) move = dist;

   float factor = move / dist;
   solPos[0] += dx * factor;
   solPos[1] += dy * factor;
   solPos[2] += dz * factor;

   float* sphereCenter = (float*)((char*)soldierPtr + 0xC4);
   sphereCenter[0] = solPos[0]; sphereCenter[1] = solPos[1]; sphereCenter[2] = solPos[2];

   float* collPos = (float*)((char*)soldierPtr + 0x7C);
   collPos[0] = solPos[0]; collPos[1] = solPos[1]; collPos[2] = solPos[2];

   *(float*)((char*)soldierPtr + kSol_VelocityX) = 0.0f;
   *(float*)((char*)soldierPtr + kSol_VelocityY) = 0.0f;
   *(float*)((char*)soldierPtr + kSol_VelocityZ) = 0.0f;
}

static constexpr float kSlingshotMultiplier = 1.8f;

static void apply_slingshot(void* soldierPtr)
{
   float speed = g_odfPullSpeed * kSlingshotMultiplier;
   float vel[3] = {
      g_pullDirX * speed,
      g_pullDirY * speed,
      g_pullDirZ * speed
   };

   __try {
      void** vtable = *(void***)soldierPtr;
      typedef void (__fastcall* SetVel_t)(void* ecx, void* edx, float* v);
      SetVel_t fnSetVel = (SetVel_t)vtable[0x48 / 4];
      fnSetVel(soldierPtr, nullptr, vel);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      *(float*)((char*)soldierPtr + kSol_VelocityX) = vel[0];
      *(float*)((char*)soldierPtr + kSol_VelocityY) = vel[1];
      *(float*)((char*)soldierPtr + kSol_VelocityZ) = vel[2];
   }
}

// ---------------------------------------------------------------------------
// Hook: OrdnanceGrapplingHook destructor body
// ---------------------------------------------------------------------------

static void __fastcall hooked_Dtor(void* ecx, void* /*edx*/)
{
   char* ord = (char*)ecx;

   void* savedHandle = *(void**)(ord + kOrd_SoldierPtr);
   int   savedKey    = *(int*)(ord + kOrd_SoldierKey);

   if (savedHandle && g_soldierPtr) {
      memset(g_dummySoldier, 0, sizeof(g_dummySoldier));
      *(int*)(g_dummySoldier + kSol_HandleKey) = savedKey;
      *(void**)g_dummySoldier = *(void**)g_soldierPtr;
      *(void**)(ord + kOrd_SoldierPtr) = g_dummySoldier;
   }

   original_Dtor(ecx, nullptr);

   *(void**)(ord + kOrd_SoldierPtr) = savedHandle;
   *(int*)(ord + kOrd_SoldierKey) = savedKey;

   if (!g_grappleActive || !g_soldierPtr) {
      g_grappleActive = false;
      return;
   }

   __try {
      void* soldierPtr = g_soldierPtr;
      if (*(int*)((char*)soldierPtr + kSol_HandleKey) != g_soldierKey)
         goto done;

      void** vtable = *(void***)soldierPtr;
      if (!vtable || !g_rttiHashPtr) goto done;
      bool isSoldier = ((fn_IsRtti_t)vtable[0])(soldierPtr, nullptr, *g_rttiHashPtr);
      if (!isSoldier) goto done;

      *(uint8_t*)((char*)soldierPtr + kSol_FlagByte) = g_savedFlagByte;

      void* collBody = (char*)soldierPtr + kSol_CollBody;
      fn_RemoveBody(collBody);
      fn_AddItemBody(collBody);

      if (!g_arrivedClean && (g_pullDirX != 0 || g_pullDirY != 0 || g_pullDirZ != 0)) {
         apply_slingshot(soldierPtr);
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      get_gamelog()("[Grapple] Exception in cleanup\n");
   }

done:
   g_grappleActive = false;
   g_wasPulling = false;
   g_arrivedClean = false;
   g_soldierPtr = nullptr;
   g_soldierKey = 0;
}

// ---------------------------------------------------------------------------
// Hook: OrdnanceGrapplingHook::Update
// ---------------------------------------------------------------------------

static unsigned int __fastcall hooked_Update(void* ecx, void* /*edx*/, float dt)
{
   char* ord = (char*)ecx;

   int   stateBefore = *(int*)(ord + kOrd_State);
   void* soldierPtr  = *(void**)(ord + kOrd_SoldierPtr);
   int   soldierKey  = *(int*)(ord + kOrd_SoldierKey);

   // Track the soldier across frames
   if (soldierPtr && !g_grappleActive) {
      g_soldierPtr = soldierPtr;
      g_soldierKey = soldierKey;
      g_grappleActive = true;
      g_wasPulling = false;
      g_arrivedClean = false;
      g_slingshotRequested = false;
      g_pullTimer = 0.0f;
      g_lastDist = 999999.0f;
      g_stuckFrames = 0;
      g_pullDirX = g_pullDirY = g_pullDirZ = 0.0f;
      g_hookPosX = g_hookPosY = g_hookPosZ = 0.0f;
      __try {
         g_savedFlagByte = *(uint8_t*)((char*)soldierPtr + kSol_FlagByte);
         // Cache fire origin for max range check
         float* solPos = (float*)((char*)soldierPtr + kSol_Position);
         g_fireOriginX = solPos[0];
         g_fireOriginY = solPos[1];
         g_fireOriginZ = solPos[2];
      } __except (EXCEPTION_EXECUTE_HANDLER) {}
   }

   // Fix RSO vtable: the constructor sets 0x00A50E98 (with the grapple render
   // at slot 19) but something post-construction overwrites it to 0x00A50D40
   // (base class vtable without the grapple render). Force the correct vtable.
   {
      void** rsoVtable = *(void***)(ord + 0x98);
      uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
      void* correctVtable = (void*)((0x00A50E98 - kUnrelocatedBase) + base);
      if (rsoVtable != (void**)correctVtable) {
         *(void**)(ord + 0x98) = correctVtable;
      }
   }

   // Force collision-enabled flag bit0 on
   __try {
      void* field130 = *(void**)(ord + 0x130);
      if (field130) {
         uint8_t* flagPtr = *(uint8_t**)((char*)field130 + 0x74);
         if (flagPtr && (*flagPtr & 1) == 0) {
            *flagPtr |= 1;
         }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {}

   // Redirect soldier handle to dummy buffer
   void* savedHandle = *(void**)(ord + kOrd_SoldierPtr);
   int   savedKey    = *(int*)(ord + kOrd_SoldierKey);

   memset(g_dummySoldier, 0, sizeof(g_dummySoldier));
   *(int*)(g_dummySoldier + kSol_HandleKey) = savedKey;
   if (g_soldierPtr) {
      *(void**)g_dummySoldier = *(void**)g_soldierPtr;
   }

   *(void**)(ord + kOrd_SoldierPtr) = g_dummySoldier;

   unsigned int result = original_Update(ecx, nullptr, dt);

   // Restore real handle
   *(void**)(ord + kOrd_SoldierPtr) = savedHandle;
   *(int*)(ord + kOrd_SoldierKey) = savedKey;

   int stateAfter = *(int*)(ord + kOrd_State);

   // Cache hook position every frame for the render hook
   {
      float* hookPos = (float*)(ord + kOrd_Position);
      g_hookPosX = hookPos[0];
      g_hookPosY = hookPos[1];
      g_hookPosZ = hookPos[2];
   }

   // --- Safety checks ---

   if (g_grappleActive && g_soldierPtr) {
      __try {
         if (*(int*)((char*)g_soldierPtr + kSol_HandleKey) != g_soldierKey)
            return 0;
         int solState = *(int*)((char*)g_soldierPtr + 0x754);
         if (solState >= 10)
            return 0;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         return 0;
      }
   }

   // State 4 (failure/retraction): kill immediately
   if (stateAfter == 4)
      return 0;

   // Max range check during flight (before pull)
   if (g_odfMaxRange > 0.0f && stateAfter != kState_Pulling && g_grappleActive) {
      float dx = g_hookPosX - g_fireOriginX;
      float dy = g_hookPosY - g_fireOriginY;
      float dz = g_hookPosZ - g_fireOriginZ;
      float rangeSq = dx*dx + dy*dy + dz*dz;
      if (rangeSq > g_odfMaxRange * g_odfMaxRange)
         return 0;
   }

   // Track if we're in pull state (for slingshot detection in destructor)
   if (stateAfter == kState_Pulling) {
      g_wasPulling = true;
   }

   // Custom pull logic during state 2
   if (stateAfter == kState_Pulling && g_soldierPtr) {
      g_pullTimer += dt;

      __try {
         float* hookPos = (float*)(ord + kOrd_Position);
         float dist = distance_to_hook(g_soldierPtr, hookPos);

         uint8_t* flagPtr = (uint8_t*)((char*)g_soldierPtr + kSol_FlagByte);

         // Slingshot: jump during pull
         if (g_slingshotRequested && g_pullTimer > 0.2f) {
            *flagPtr = g_savedFlagByte;
            return 0;
         }

         move_soldier_toward(g_soldierPtr, hookPos, dt);

         // Stuck detection — tolerance scales with pull speed to avoid
         // false positives at low speeds (at 1 m/s, per-frame movement
         // is ~0.017m which would always fail a fixed 0.1m check)
         float stuckTolerance = g_odfPullSpeed * dt * 0.5f;
         if (stuckTolerance < 0.01f) stuckTolerance = 0.01f;
         if (dist >= g_lastDist - stuckTolerance)
            g_stuckFrames++;
         else
            g_stuckFrames = 0;
         g_lastDist = dist;

         // Arrival / stuck / timeout (timeout scales so slow pulls aren't cut short)
         float maxTime = (g_odfMaxRange > 0.0f)
            ? (g_odfMaxRange / g_odfPullSpeed) + 2.0f
            : kMaxPullTime;
         if (dist < kArrivalDist || g_stuckFrames > kMaxStuckFrames || g_pullTimer > maxTime) {
            *flagPtr = g_savedFlagByte;
            g_arrivedClean = true;
            return 0;
         }
      }
      __except (EXCEPTION_EXECUTE_HANDLER) {
         get_gamelog()("[Grapple] EXCEPTION in pull logic\n");
         return 0;
      }
   }

   return result;
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void grapple_fix_install(uintptr_t exe_base)
{
   fn_RemoveBody    = (fn_RemoveBody_t) resolve(exe_base, kRemoveBody_addr);
   fn_AddItemBody   = (fn_AddItemBody_t)resolve(exe_base, kAddItemBody_addr);
   g_rttiHashPtr    = (uint32_t*)       resolve(exe_base, kRttiHash_addr);
   original_Update  = (fn_Update_t)     resolve(exe_base, kUpdate_addr);
   original_Dtor    = (fn_Dtor_t)       resolve(exe_base, kDtor_addr);
   original_005297b0  = (fn_005297b0_t)  resolve(exe_base, kFUN005297b0_addr);
   original_CheckFire = (fn_CheckFire_t) resolve(exe_base, kCheckFire_addr);
   original_TriggerUpdate = (fn_TriggerUpdate_t) resolve(exe_base, kTriggerUpdate_addr);
   original_OrdRender = (fn_OrdRender_t) resolve(exe_base, kOrdRender_addr);
   original_SetProperty = (fn_SetProperty_t) resolve(exe_base, kSetProperty_addr);
   fn_HashString    = (fn_HashString_t)  resolve(exe_base, kHashString_addr);
   fn_SplineBuild   = (fn_SplineBuild_t) resolve(exe_base, kSplineBuild_addr);
   fn_CableRender   = (fn_CableRender_t) resolve(exe_base, kCableRender_addr);
   fn_VecScale      = (fn_VecScale_t)    resolve(exe_base, kVecScale_addr);

   // Compute property hashes
   g_hashPullSpeed    = fn_HashString("PullSpeed");
   g_hashGrappleRange = fn_HashString("GrappleRange");

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_Update,        hooked_Update);
   DetourAttach(&(PVOID&)original_Dtor,          hooked_Dtor);
   DetourAttach(&(PVOID&)original_005297b0,      hooked_005297b0);
   DetourAttach(&(PVOID&)original_CheckFire,     hooked_CheckFire);
   DetourAttach(&(PVOID&)original_TriggerUpdate, hooked_TriggerUpdate);
   DetourAttach(&(PVOID&)original_OrdRender,     hooked_OrdRender);
   DetourAttach(&(PVOID&)original_SetProperty,   hooked_SetProperty);
   DetourTransactionCommit();
}

void grapple_fix_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_Update)        DetourDetach(&(PVOID&)original_Update,        hooked_Update);
   if (original_Dtor)          DetourDetach(&(PVOID&)original_Dtor,          hooked_Dtor);
   if (original_005297b0)      DetourDetach(&(PVOID&)original_005297b0,      hooked_005297b0);
   if (original_CheckFire)     DetourDetach(&(PVOID&)original_CheckFire,     hooked_CheckFire);
   if (original_TriggerUpdate) DetourDetach(&(PVOID&)original_TriggerUpdate, hooked_TriggerUpdate);
   if (original_OrdRender)     DetourDetach(&(PVOID&)original_OrdRender,     hooked_OrdRender);
   if (original_SetProperty)   DetourDetach(&(PVOID&)original_SetProperty,   hooked_SetProperty);
   DetourTransactionCommit();
}
