#include "pch.h"
#include "grapple_fix.hpp"

#include <detours.h>
#include <cmath>

// =============================================================================
// Grappling Hook Fix — v3 (custom pull + slingshot momentum)
//
// The engine's FUN_005297b0 camera timer reset causes irreversible 3P model
// invisibility. We block it and implement custom pull logic. The soldier
// handle is redirected to a dummy buffer so the engine never touches the
// real soldier.
//
// Slingshot mechanic: switching weapon mid-pull cancels the grapple but
// preserves momentum — the soldier launches with the pull velocity.
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
static constexpr uintptr_t kOrdRender_addr     = 0x0060fb80; // OrdnanceGrapplingHook RSO Render(this=ord+0x98, p2, p3, p4)
static constexpr uintptr_t kSplineBuild_addr   = 0x0083e720; // Cubic Hermite spline coefficient builder
static constexpr uintptr_t kCableRender_addr   = 0x006d2370; // Cable triangle strip renderer
static constexpr uintptr_t kVecScale_addr      = 0x004294b0; // PblVector3 scale(out, scalar, in)

// Ordnance offsets
static constexpr int kOrd_SoldierPtr  = 0x54;
static constexpr int kOrd_SoldierKey  = 0x58;
static constexpr int kOrd_Position    = 0x48;   // PblVector3 (hook attachment point)
static constexpr int kOrd_State       = 0x12C;

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

static constexpr int kState_Pulling   = 2;
static constexpr float kArrivalDist   = 3.5f;
static constexpr float kPullSpeed     = 15.0f;
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
// RET 0x14 = cleans 5 dwords. 1st param (coefs_out) is in ECX (__thiscall-like).
// Remaining 5 stack params: start, end, tan_start, tan_end, length (float).
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
static fn_SplineBuild_t fn_SplineBuild     = nullptr;
static fn_CableRender_t fn_CableRender     = nullptr;
static fn_VecScale_t    fn_VecScale        = nullptr;
static uint32_t*        g_rttiHashPtr      = nullptr;

// ---------------------------------------------------------------------------
// FUN_005297b0 hook — blocks camera timer reset that causes 3P invisibility
// ---------------------------------------------------------------------------

static void __fastcall hooked_005297b0(void* soldier, void* /*edx*/, int value)
{
   if (value != 0) {
      // Block entirely — camera timer reset causes 3P invisibility
      return;
   }
   original_005297b0(soldier, nullptr, value);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool    g_grappleActive  = false;
static bool    g_wasPulling     = false;   // was in state 2 before destruction
static void*   g_soldierPtr     = nullptr;
static int     g_soldierKey     = 0;
static float   g_pullTimer      = 0.0f;
static float   g_lastDist       = 999999.0f;
static int     g_stuckFrames    = 0;
static uint8_t g_savedFlagByte  = 0;
static bool    g_arrivedClean   = false;
static bool    g_slingshotRequested = false;
static bool    g_jumpWasPressed    = false;   // previous frame's jump state

// Current pull direction (normalized) — used for slingshot momentum
static float g_pullDirX = 0.0f;
static float g_pullDirY = 0.0f;
static float g_pullDirZ = 0.0f;

// Hook head position — cached during Update for the render hook
static float g_hookPosX = 0.0f;
static float g_hookPosY = 0.0f;
static float g_hookPosZ = 0.0f;

// Dummy soldier buffer
static uint8_t g_dummySoldier[0x500] = {};

// ---------------------------------------------------------------------------
// Hook: Trigger::Update — intercepts jump button input at the source.
//
// PlayerController::Update calls Trigger::Update for each Controllable trigger.
// We check if the trigger pointer matches our soldier's mControlJump and
// capture the raw buttonDown state before it gets consumed.
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
//
// this (ECX) = ordnance + 0x98 (RedSceneObject part).
// Called by the scene system with proper D3D render state.
// Uses the same cubic Hermite spline + billboard strip renderer as
// OrdnanceTowCable (FUN_0083e720 + FUN_006d2370).
// ---------------------------------------------------------------------------

static void __fastcall hooked_OrdRender(void* rso, void* /*edx*/, uint32_t p2, uint32_t p3, uint32_t p4)
{
   // Call original render (draws hook head model)
   original_OrdRender(rso, nullptr, p2, p3, p4);

   // Draw rope whenever grapple is active (flight + pull)
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
         // entity+0x4F0 = Weapon*[8], entity+0x512 = active slot index
         // entity = struct_base + 0x240
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

      // Build cubic Hermite spline between soldier and hook
      float startTangent[3] = { 0.0f, 0.0f, 0.0f };
      float upDir[3] = { 0.0f, 1.0f, 0.0f };
      float endTangent[3];
      fn_VecScale(endTangent, -15.0f, upDir);

      float coefs[12];
      fn_SplineBuild(coefs, nullptr, startPos, hookPos, startTangent, endTangent, 1.0f);

      // Render cable strip — pass RSO render params for D3D context
      // CableRender(coefs, unused, unused, cable_width) — 4 cdecl params
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

   // Store normalized pull direction for slingshot
   g_pullDirX = dx / dist;
   g_pullDirY = dy / dist;
   g_pullDirZ = dz / dist;

   float move = kPullSpeed * dt;
   if (move > dist) move = dist;

   float factor = move / dist;
   solPos[0] += dx * factor;
   solPos[1] += dy * factor;
   solPos[2] += dz * factor;

   // Sync collision body positions for render culling
   float* sphereCenter = (float*)((char*)soldierPtr + 0xC4);
   sphereCenter[0] = solPos[0];
   sphereCenter[1] = solPos[1];
   sphereCenter[2] = solPos[2];

   float* collPos = (float*)((char*)soldierPtr + 0x7C);
   collPos[0] = solPos[0];
   collPos[1] = solPos[1];
   collPos[2] = solPos[2];

   // Zero velocity to counteract gravity
   *(float*)((char*)soldierPtr + kSol_VelocityX) = 0.0f;
   *(float*)((char*)soldierPtr + kSol_VelocityY) = 0.0f;
   *(float*)((char*)soldierPtr + kSol_VelocityZ) = 0.0f;
}

// Apply slingshot momentum via the entity's SetVelocity vtable call.
// This uses the same mechanism as the engine's own velocity system.
static constexpr float kSlingshotMultiplier = 1.8f;

static void apply_slingshot(void* soldierPtr)
{
   float speed = kPullSpeed * kSlingshotMultiplier;
   float vel[3] = {
      g_pullDirX * speed,
      g_pullDirY * speed,
      g_pullDirZ * speed
   };

   // Use vtable[0x12] = SetVelocity(PblVector3*) — same as engine uses
   __try {
      void** vtable = *(void***)soldierPtr;
      typedef void (__fastcall* SetVel_t)(void* ecx, void* edx, float* v);
      SetVel_t fnSetVel = (SetVel_t)vtable[0x48 / 4];
      fnSetVel(soldierPtr, nullptr, vel);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // Fallback: direct write
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

   // Redirect to dummy
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

      // Restore animation flag byte
      *(uint8_t*)((char*)soldierPtr + kSol_FlagByte) = g_savedFlagByte;

      // Restore collision
      void* collBody = (char*)soldierPtr + kSol_CollBody;
      fn_RemoveBody(collBody);
      fn_AddItemBody(collBody);

      // Slingshot: if destroyed during pull WITHOUT proper arrival
      // (weapon switch / cancel), give momentum in pull direction
      if (!g_arrivedClean && (g_pullDirX != 0 || g_pullDirY != 0 || g_pullDirZ != 0)) {
         apply_slingshot(soldierPtr);
         float speed = kPullSpeed * kSlingshotMultiplier;
         get_gamelog()("[Grapple] Slingshot! vel=(%.1f, %.1f, %.1f)\n",
             g_pullDirX * speed, g_pullDirY * speed, g_pullDirZ * speed);
      } else {
         get_gamelog()("[Grapple] Cleanup complete (arrival)\n");
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
      g_jumpWasPressed = true;  // assume pressed to ignore initial state
      g_pullTimer = 0.0f;
      g_lastDist = 999999.0f;
      g_stuckFrames = 0;
      g_pullDirX = g_pullDirY = g_pullDirZ = 0.0f;
      g_hookPosX = g_hookPosY = g_hookPosZ = 0.0f;
      __try {
         g_savedFlagByte = *(uint8_t*)((char*)soldierPtr + kSol_FlagByte);
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

   // --- Safety checks (prevent crashes) ---

   // Check if soldier died or handle went invalid
   if (g_grappleActive && g_soldierPtr) {
      __try {
         if (*(int*)((char*)g_soldierPtr + kSol_HandleKey) != g_soldierKey) {
            get_gamelog()("[Grapple] Soldier handle invalid — killing ordnance\n");
            return 0;
         }
         // Check soldier state — DEAD is typically >= 10
         int solState = *(int*)((char*)g_soldierPtr + 0x754);
         if (solState >= 10) {
            get_gamelog()("[Grapple] Soldier dead (state=%d) — killing ordnance\n", solState);
            return 0;
         }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         return 0;  // can't read soldier, kill ordnance
      }
   }

   // State 4 (failure/retraction): kill immediately since the retraction
   // goes toward the dummy soldier at (0,0,0) and would never arrive.
   if (stateAfter == 4) {
      get_gamelog()("[Grapple] HookFailure retraction — killing ordnance\n");
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
         if (g_pullTimer < dt * 1.5f)
            get_gamelog()("[Grapple] Pull started\n");

         float* hookPos = (float*)(ord + kOrd_Position);
         float dist = distance_to_hook(g_soldierPtr, hookPos);

         // NOTE: Do NOT set 0x1C on flag byte — it suppresses input processing
         // including jump detection. Landing animation will trigger but jump works.
         uint8_t* flagPtr = (uint8_t*)((char*)g_soldierPtr + kSol_FlagByte);

         // Slingshot: g_slingshotRequested is set by the Trigger::Update hook
         // when jump button is pressed (captures raw input before consumption).
         if (g_slingshotRequested && g_pullTimer > 0.2f) {
            *flagPtr = g_savedFlagByte;
            get_gamelog()("[Grapple] Jump during pull — slingshot!\n");
            return 0;  // kill ordnance → destructor applies slingshot
         }

         // Move soldier toward hook
         move_soldier_toward(g_soldierPtr, hookPos, dt);

         // Stuck detection
         if (dist >= g_lastDist - 0.1f) {
            g_stuckFrames++;
         } else {
            g_stuckFrames = 0;
         }
         g_lastDist = dist;

         // Check arrival / stuck / timeout
         if (dist < kArrivalDist || g_stuckFrames > kMaxStuckFrames || g_pullTimer > kMaxPullTime) {
            *flagPtr = g_savedFlagByte;
            g_arrivedClean = true;  // proper arrival, not weapon switch
            get_gamelog()("[Grapple] Arrival! dist=%.1f stuck=%d\n", dist, g_stuckFrames);
            return 0;
         }
      }
      __except (EXCEPTION_EXECUTE_HANDLER) {
         get_gamelog()("[Grapple] EXCEPTION in pull logic!\n");
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
   fn_SplineBuild     = (fn_SplineBuild_t) resolve(exe_base, kSplineBuild_addr);
   fn_CableRender     = (fn_CableRender_t) resolve(exe_base, kCableRender_addr);
   fn_VecScale        = (fn_VecScale_t) resolve(exe_base, kVecScale_addr);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r1 = DetourAttach(&(PVOID&)original_Update,   hooked_Update);
   LONG r2 = DetourAttach(&(PVOID&)original_Dtor,     hooked_Dtor);
   LONG r3 = DetourAttach(&(PVOID&)original_005297b0, hooked_005297b0);
   LONG r4 = DetourAttach(&(PVOID&)original_CheckFire, hooked_CheckFire);
   LONG r5 = DetourAttach(&(PVOID&)original_TriggerUpdate, hooked_TriggerUpdate);
   LONG r6 = DetourAttach(&(PVOID&)original_OrdRender, hooked_OrdRender);
   LONG rc = DetourTransactionCommit();

   get_gamelog()("[Grapple] v5 Installed (Update=%ld Dtor=%ld 005297b0=%ld CF=%ld TU=%ld Rnd=%ld commit=%ld)\n",
                 r1, r2, r3, r4, r5, r6, rc);
}

void grapple_fix_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_Update)   DetourDetach(&(PVOID&)original_Update,   hooked_Update);
   if (original_Dtor)     DetourDetach(&(PVOID&)original_Dtor,     hooked_Dtor);
   if (original_005297b0) DetourDetach(&(PVOID&)original_005297b0, hooked_005297b0);
   if (original_CheckFire) DetourDetach(&(PVOID&)original_CheckFire, hooked_CheckFire);
   if (original_TriggerUpdate) DetourDetach(&(PVOID&)original_TriggerUpdate, hooked_TriggerUpdate);
   if (original_OrdRender) DetourDetach(&(PVOID&)original_OrdRender, hooked_OrdRender);
   DetourTransactionCommit();
}
