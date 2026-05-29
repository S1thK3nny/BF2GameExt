#include "pch.h"
#include "flyer_strafe_mode.hpp"
#include "core/resolve.hpp"
#include "core/game_addrs.hpp"

#include <cstring>
#include <cstdlib>
#include <detours.h>

// =============================================================================
// Flyer Strafe Mode — implementation
//
// See flyer_strafe_mode.hpp for the ODF interface.
//
// Two hooks:
//   1. EntityFlyerClass::SetProperty (0x004FA310) — parse the new
//      "FlyerStrafeMode" ODF property and flag the class.
//   2. EntityFlyer::Update (0x004fc930) — for flagged, locally-player-piloted
//      flyers, suppress the turn->roll path and add lateral strafe velocity.
//
// All offsets are on the "Update base" (ECX of EntityFlyer::Update =
// struct_base + 0x240) unless noted.  See memory: flyer_player_control_system.
// =============================================================================

// ---- EntityFlyer::Update (Update base) offsets ----------------------------
static constexpr uintptr_t kFly_ControlMode  = 0x54;   // byte; bit0: alt control mode (turn axis -> throttle)
static constexpr uintptr_t kFly_ControlTurn  = 0x84;   // float mControlTurn (drives roll/yaw = "rolling")
static constexpr uintptr_t kFly_PlayerId     = 0xD4;   // int; 0 == local human player
static constexpr uintptr_t kFly_Velocity     = 0x340;  // PblVector3 world-space linear velocity
static constexpr uintptr_t kFly_RollState1   = 0x36C;  // float bank (ASCENDING / state 1)
static constexpr uintptr_t kFly_FlightState  = 0x364;  // int 0..5
static constexpr uintptr_t kFly_RollState2   = 0x3C8;  // float roll value (FLYING / state 2)
static constexpr uintptr_t kFly_ClassPtr     = 0x42C;  // EntityFlyerClass*

// Right (lateral) basis vector lives on struct_base+0xF0 = Update base - 0x150.
static constexpr intptr_t  kFly_RightVecRel  = -0x150; // x; y at +4, z at +8

// ---- EntityFlyerClass float offsets ---------------------------------------
static constexpr uintptr_t kCls_StrafeSpeed     = 0x89C;
static constexpr uintptr_t kCls_StrafeRollAngle = 0x8C4;

// ---------------------------------------------------------------------------
// ODF key hash — PblHash::_MakeHash (0x007E1B70): FNV-1a, forced lowercase.
// ---------------------------------------------------------------------------
static constexpr unsigned int makeHash(const char* s)
{
   unsigned int h = 0x811C9DC5u;
   if (!s) return 0u;
   for (; *s; ++s)
      h = (h ^ (unsigned int)((unsigned char)*s | 0x20u)) * 0x01000193u;
   return h;
}

// "FlyerStrafeMode" (the | 0x20 lowercasing makes case irrelevant).
static const unsigned int kFlyerStrafeMode_Hash = makeHash("FlyerStrafeMode");

// ---------------------------------------------------------------------------
// Per-class flag table (class pointers that opted in).
// ---------------------------------------------------------------------------
static constexpr int kMaxClasses = 32;
static void* g_strafeClasses[kMaxClasses] = {};

static bool classEnabled(void* classPtr)
{
   if (!classPtr) return false;
   for (int i = 0; i < kMaxClasses; i++)
      if (g_strafeClasses[i] == classPtr) return true;
   return false;
}

static void setClassEnabled(void* classPtr, bool on)
{
   if (!classPtr) return;
   if (on) {
      for (int i = 0; i < kMaxClasses; i++)
         if (g_strafeClasses[i] == classPtr) return; // already present
      for (int i = 0; i < kMaxClasses; i++)
         if (!g_strafeClasses[i]) { g_strafeClasses[i] = classPtr; return; }
   } else {
      for (int i = 0; i < kMaxClasses; i++)
         if (g_strafeClasses[i] == classPtr) { g_strafeClasses[i] = nullptr; return; }
   }
}

// ---------------------------------------------------------------------------
// Hook: EntityFlyerClass::SetProperty
//   __thiscall(EntityFlyerClass* this, unsigned int hash, const char* value)
// ---------------------------------------------------------------------------
using fn_FlyerSetProperty_t = void(__fastcall*)(void* ecx, void* edx,
                                                unsigned int hash, const char* value);
static fn_FlyerSetProperty_t original_FlyerSetProperty = nullptr;

static void __fastcall hooked_FlyerSetProperty(void* ecx, void* /*edx*/,
                                               unsigned int hash, const char* value)
{
   if (hash == kFlyerStrafeMode_Hash) {
      // Our custom property — consume it, don't forward to the engine.
      bool on = value && (atoi(value) != 0);
      setClassEnabled(ecx, on);
      return;
   }
   original_FlyerSetProperty(ecx, nullptr, hash, value);
}

// ---------------------------------------------------------------------------
// Hook: EntityFlyer::Update
//   __thiscall(EntityFlyer* this, float dt) -> bool (alive)
//   ECX = Update base (struct_base + 0x240)
// ---------------------------------------------------------------------------
using fn_FlyerUpdate_t = bool(__fastcall*)(void* ecx, void* edx, float dt);
static fn_FlyerUpdate_t original_FlyerUpdate = nullptr;

static inline float clampf(float v, float lo, float hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

static bool __fastcall hooked_FlyerUpdate(void* ecx, void* /*edx*/, float dt)
{
   char* base = (char*)ecx;

   // Decide whether to engage strafe mode for this flyer this frame.
   bool engage = false;
   float savedTurn = 0.0f;
   __try {
      void* classPtr = *(void**)(base + kFly_ClassPtr);
      if (classEnabled(classPtr) &&
          *(int*)(base + kFly_PlayerId) == 0 &&          // local human player only
          (*(unsigned char*)(base + kFly_ControlMode) & 1) == 0) { // normal control mode
         // Take over the turn axis: zero it so the vanilla update produces
         // straight, level flight (no roll, no yaw, no turn-coupled lateral).
         float* turn = (float*)(base + kFly_ControlTurn);
         savedTurn = *turn;
         *turn = 0.0f;
         engage = true;
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   bool alive = original_FlyerUpdate(ecx, nullptr, dt);

   if (!engage || !alive) return alive;

   __try {
      // Restore the real turn input so HUD / camera / aiming read it normally.
      *(float*)(base + kFly_ControlTurn) = savedTurn;

      int state = *(int*)(base + kFly_FlightState);
      if (state == 1 || state == 2) { // ASCENDING or FLYING
         void* classPtr = *(void**)(base + kFly_ClassPtr);
         float strafeSpeed = *(float*)((char*)classPtr + kCls_StrafeSpeed);
         float rollAngle   = *(float*)((char*)classPtr + kCls_StrafeRollAngle);

         // Lateral strafe velocity = turn input * StrafeSpeed along the right axis.
         float* rightVec = (float*)(base + kFly_RightVecRel);
         float lateral = savedTurn * strafeSpeed;
         float* vel = (float*)(base + kFly_Velocity);
         vel[0] += rightVec[0] * lateral;
         vel[1] += rightVec[1] * lateral;
         vel[2] += rightVec[2] * lateral;

         // Roll: default to level (rollAngle 0 => no rolling).  A non-zero
         // StrafeRollAngle leans the flyer into the strafe; the roll field is
         // an input-normalized value, so clamp to [-1, 1].
         float lean = clampf(savedTurn * rollAngle, -1.0f, 1.0f);
         if (state == 2) *(float*)(base + kFly_RollState2) = lean;
         else            *(float*)(base + kFly_RollState1) = lean;
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   return alive;
}

// ---------------------------------------------------------------------------
// Install / Uninstall / Reset
// ---------------------------------------------------------------------------
void flyer_strafe_mode_install(uintptr_t exe_base)
{
   original_FlyerSetProperty = (fn_FlyerSetProperty_t)
      resolve(exe_base, game_addrs::modtools::flyer_class_set_property);
   original_FlyerUpdate = (fn_FlyerUpdate_t)
      resolve(exe_base, game_addrs::modtools::flyer_update);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_FlyerSetProperty, hooked_FlyerSetProperty);
   DetourAttach(&(PVOID&)original_FlyerUpdate,      hooked_FlyerUpdate);
   DetourTransactionCommit();
}

void flyer_strafe_mode_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_FlyerSetProperty) DetourDetach(&(PVOID&)original_FlyerSetProperty, hooked_FlyerSetProperty);
   if (original_FlyerUpdate)      DetourDetach(&(PVOID&)original_FlyerUpdate,      hooked_FlyerUpdate);
   DetourTransactionCommit();
}

void flyer_strafe_mode_reset()
{
   // Class pointers are invalidated on level change.
   memset(g_strafeClasses, 0, sizeof(g_strafeClasses));
}
