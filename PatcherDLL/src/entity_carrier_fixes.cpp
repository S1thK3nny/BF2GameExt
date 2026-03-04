#include "pch.h"
#include "entity_carrier_fixes.hpp"

#include <detours.h>

// =============================================================================
// EntityCarrier / EntityCarrierClass bug fixes
//
// Bugs fixed:
//   1. EntityCarrierClass::SetProperty — no bounds check on mCargoCount before
//      writing to mCargoInfo[mCargoCount].  At count==4, the write destination
//      is exactly &mCargoCount (self-corruption); at count==5 it hits
//      mSoundCargoPickup.  Fix: early-return for cargo-node properties when
//      the array is already full.
//
//   2. EntityCarrier::AttachCargo — no slotIdx bounds check; an out-of-range
//      index walks past the end of mCargoSlots[4] into adjacent struct fields.
//      Fix: early-return when slotIdx >= 4.
//
//   3. EntityCarrier::AttachCargo — cargo pointer (param_3) is used for a
//      vtable call before any null check, causing an immediate crash if null.
//      Fix: early-return when cargo == null.
//
//   4. EntityCarrier::DetachCargo — same missing slotIdx bounds check.
//      Fix: early-return when slotIdx >= 4.
//
// All other original logic is preserved by calling the trampoline installed by
// Detours.  Addresses are BF2_modtools-specific (Ghidra imagebase 0x400000).
// =============================================================================

static constexpr uintptr_t kUnrelocatedBase = 0x400000u;

static inline void* resolve(uintptr_t exe_base, uintptr_t unrelocated_addr)
{
   return (void*)((unrelocated_addr - kUnrelocatedBase) + exe_base);
}

// Game's printf-style debug logger — FUN_007e3d50, __cdecl
typedef void (__cdecl* GameLog_t)(const char* fmt, ...);
static GameLog_t get_gamelog()
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   return (GameLog_t)((0x7E3D50 - kUnrelocatedBase) + base);
}

// ---------------------------------------------------------------------------
// EntityCarrier / EntityFlyer struct offsets (from struct_base)
// ---------------------------------------------------------------------------

static constexpr int          kMaxCargo              = 4;
static constexpr uintptr_t    kMCargoCount_offset    = 0x11c0; // EntityCarrierClass::mCargoCount
static constexpr unsigned int kCargoNodeName_Hash    = 0x3e2c4da4u;
static constexpr unsigned int kCargoNodeOffset_Hash  = 0x910a89fcu;

// EntityCarrier instance offsets from ECX (= this in Update, = struct_base + 0x240).
static constexpr uintptr_t kState_offset        = 0x364;  // int: 0=landed 1=ascending 2=flying 3=landing 4=dying 5=dead
static constexpr uintptr_t kProgress_offset     = 0x368;  // float: takeoff/landing progress (0..1)
static constexpr uintptr_t kLandedHeight_offset = 0x3C0;  // float: hover floor / landing threshold
static constexpr uintptr_t kGroundDist_offset   = 0x490;  // float: last measured ground distance
static constexpr uintptr_t kCargoSlots_offset   = 0x1b90; // CargoSlot[4] (stride 0x14)
static constexpr uintptr_t kCargoSlot_ObjPtr    = 0x0C;   // slot+0xC = object pointer
static constexpr uintptr_t kClass_offset        = 0x42C;  // EntityCarrierClass*

// EntityFlyerClass offsets (from class pointer)
static constexpr uintptr_t kClassLandedHt_off   = 0x8F4;  // float: -(bbox_Y_min)
static constexpr uintptr_t kClassTakeoffHt_off  = 0x8E0;  // float: TakeoffHeight ODF property

// ---------------------------------------------------------------------------
// EntityCarrierClass::SetProperty
//   __thiscall(EntityCarrierClass* this, unsigned int hash, const char* value)
//   Ghidra VA: 0x004D7210
// ---------------------------------------------------------------------------

static constexpr uintptr_t kSetProperty_addr = 0x004D7210;

using fn_SetProperty_t = void(__fastcall*)(void* ecx, void* edx,
                                           unsigned int hash, const char* value);
static fn_SetProperty_t original_SetProperty = nullptr;

static void __fastcall hooked_SetProperty(void* ecx, void* /*edx*/,
                                          unsigned int hash, const char* value)
{
   if (hash == kCargoNodeName_Hash || hash == kCargoNodeOffset_Hash) {
      int count = *(int*)((char*)ecx + kMCargoCount_offset);
      if (count >= kMaxCargo) return; // array full — silently ignore extra nodes
   }
   original_SetProperty(ecx, nullptr, hash, value);
}

// ---------------------------------------------------------------------------
// EntityCarrier::AttachCargo
//   __thiscall(EntityCarrier* this, int slotIdx, void* cargo)
//   Ghidra VA: 0x004D81F0
// ---------------------------------------------------------------------------

static constexpr uintptr_t kAttachCargo_addr = 0x004D81F0;

using fn_AttachCargo_t = void(__fastcall*)(void* ecx, void* edx,
                                           int slotIdx, void* cargo);
static fn_AttachCargo_t original_AttachCargo = nullptr;

static void __fastcall hooked_AttachCargo(void* ecx, void* /*edx*/,
                                          int slotIdx, void* cargo)
{
   if ((unsigned int)slotIdx >= (unsigned int)kMaxCargo) return; // OOB slot
   if (!cargo) return;                                            // null cargo
   original_AttachCargo(ecx, nullptr, slotIdx, cargo);
}

// ---------------------------------------------------------------------------
// EntityCarrier::DetachCargo
//   __thiscall(EntityCarrier* this, int slotIdx)
//   Ghidra VA: 0x004D8350
// ---------------------------------------------------------------------------

static constexpr uintptr_t kDetachCargo_addr = 0x004D8350;

using fn_DetachCargo_t = void(__fastcall*)(void* ecx, void* edx, int slotIdx);
static fn_DetachCargo_t original_DetachCargo = nullptr;

static void __fastcall hooked_DetachCargo(void* ecx, void* /*edx*/, int slotIdx)
{
   if ((unsigned int)slotIdx >= (unsigned int)kMaxCargo) return; // OOB slot
   original_DetachCargo(ecx, nullptr, slotIdx);
}

// ---------------------------------------------------------------------------
// EntityFlyer::TakeOff — block landing interruption
//   __thiscall(EntityFlyer* this_inner)    (this_inner = struct_base)
//   Ghidra VA: 0x004F8B70      (thunk at 0x00408602)
//
// Fix: if entity is already in state 3 (LANDING), return early.
// ---------------------------------------------------------------------------

static constexpr uintptr_t kTakeOff_addr = 0x004F8B70;
static constexpr uintptr_t kInner_mFlightState = 0x5A4;
static constexpr uintptr_t kEntityCarrier_vtable = 0x00A3A670;

// World position offsets from struct_base (inner base).
static constexpr uintptr_t kInner_mPosX = 0x120;
static constexpr uintptr_t kInner_mPosY = 0x124;
static constexpr uintptr_t kInner_mPosZ = 0x128;

using fn_TakeOff_t = void(__fastcall*)(void* ecx, void* edx);
static fn_TakeOff_t original_TakeOff = nullptr;
static void* g_carrierVtable = nullptr; // resolved at install time

// Per-carrier transform snapshot, taken at TakeOff time.
// The movement controller overwrites the entity's full transform (position +
// rotation) to a flight-path start point that doesn't match the carrier's
// current pose.  This causes the carrier to visibly jump and rotate for ~1s
// until the path converges.  We save the pre-TakeOff transform and restore
// it each tick during ASCENDING, allowing only Y (altitude) to change.
//
// Entity transform layout at struct_base (16 bytes per row):
//   +0x100: rotation row 0 (right axis)
//   +0x110: rotation row 1 (forward axis / direction vector)
//   +0x120: X, Y, Z position  (+0x12C likely padding/W)
struct CarrierPosSnapshot {
   void*  ecx;           // inner-base pointer (struct_base), 0 = unused
   float  savedX;        // X position at TakeOff time
   float  savedZ;        // Z position at TakeOff time
   float  rotRow0[4];    // struct_base+0x100..+0x10F
   float  rotRow1[4];    // struct_base+0x110..+0x11F
   bool   active;        // true while correction is active
};
static constexpr int kMaxTrackedCarriers = 4;
static CarrierPosSnapshot g_takeoffPos[kMaxTrackedCarriers] = {};

static void __fastcall hooked_TakeOff(void* ecx, void* /*edx*/)
{
   __try {
      void* vtable = *(void**)ecx;
      if (vtable == g_carrierVtable) {
         int state = *(int*)((char*)ecx + kInner_mFlightState);
         if (state == 3) {
            auto fn = get_gamelog();
            if (fn) fn("[Carrier] TakeOff BLOCKED — entity is LANDING (state 3)\n");
            return;
         }

         // Save full transform before TakeOff: rotation + position X/Z.
         // The movement controller will overwrite the entity transform to a
         // flight-path start point; we restore everything except Y each tick.
         if (state == 0) {
            char* p = (char*)ecx;
            int slot = -1;
            for (int i = 0; i < kMaxTrackedCarriers; i++) {
               if (g_takeoffPos[i].ecx == ecx || g_takeoffPos[i].ecx == nullptr) {
                  slot = i; break;
               }
            }
            if (slot < 0) slot = 0;
            g_takeoffPos[slot].ecx = ecx;
            g_takeoffPos[slot].savedX = *(float*)(p + kInner_mPosX);
            g_takeoffPos[slot].savedZ = *(float*)(p + kInner_mPosZ);
            memcpy(g_takeoffPos[slot].rotRow0, p + 0x100, 16);
            memcpy(g_takeoffPos[slot].rotRow1, p + 0x110, 16);
            g_takeoffPos[slot].active = true;
         }

         original_TakeOff(ecx, nullptr);
         return;
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   original_TakeOff(ecx, nullptr);
}

// ---------------------------------------------------------------------------
// EntityCarrier::Update — diagnostic logging
//   __thiscall(EntityCarrier* this, float dt)
//   Ghidra VA: 0x004D7FE0
// ---------------------------------------------------------------------------

static constexpr uintptr_t kCarrierUpdate_addr = 0x004D7FE0;

using fn_CarrierUpdate_t = bool(__fastcall*)(void* ecx, void* edx, float dt);
static fn_CarrierUpdate_t original_CarrierUpdate = nullptr;

static int  g_diagLastState = -1;
static int  g_diagFrameCount = 0;

static bool __fastcall hooked_CarrierUpdate(void* ecx, void* /*edx*/, float dt)
{
   char* inner = (char*)ecx - 0x240;

   // Pre-Update: restore saved transform (rotation + X position).
   // The movement controller overwrites the entity's full transform each tick
   // to a flight-path position.  We restore rotation and X; let Y change
   // (ascending) and Z change (forward movement along path).
   __try {
      void* carrierInner = (void*)inner;
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_takeoffPos[i].ecx == carrierInner && g_takeoffPos[i].active) {
            int state = *(int*)((char*)ecx + kState_offset);
            if (state != 1) {
               g_takeoffPos[i].active = false;
               g_takeoffPos[i].ecx = nullptr;
            } else {
               memcpy(inner + 0x100, g_takeoffPos[i].rotRow0, 16);
               memcpy(inner + 0x110, g_takeoffPos[i].rotRow1, 16);
               *(float*)(inner + kInner_mPosX) = g_takeoffPos[i].savedX;
            }
            break;
         }
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   bool alive = original_CarrierUpdate(ecx, nullptr, dt);
   if (!alive) return false;

   // Post-Update: restore again in case original Update overwrote transform.
   __try {
      void* carrierInner = (void*)inner;
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_takeoffPos[i].ecx == carrierInner && g_takeoffPos[i].active) {
            int state = *(int*)((char*)ecx + kState_offset);
            if (state != 1) {
               g_takeoffPos[i].active = false;
               g_takeoffPos[i].ecx = nullptr;
            } else {
               memcpy(inner + 0x100, g_takeoffPos[i].rotRow0, 16);
               memcpy(inner + 0x110, g_takeoffPos[i].rotRow1, 16);
               *(float*)(inner + kInner_mPosX) = g_takeoffPos[i].savedX;
            }
            break;
         }
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   // Diagnostic: state transitions
   __try {
      int   state = *(int*)((char*)ecx + kState_offset);
      float posX = *(float*)(inner + kInner_mPosX);
      float posY = *(float*)(inner + kInner_mPosY);
      float posZ = *(float*)(inner + kInner_mPosZ);

      if (state != g_diagLastState) {
         auto fn = get_gamelog();
         if (fn) {
            static const char* sn[] = {"LANDED","ASCENDING","FLYING","LANDING","DYING","DEAD"};
            const char* snOld = (g_diagLastState >= 0 && g_diagLastState <= 5) ? sn[g_diagLastState] : "???";
            const char* snNew = (state >= 0 && state <= 5) ? sn[state] : "???";
            fn("[Carrier] *** TRANSITION %d(%s)->%d(%s) *** pos=(%.2f,%.2f,%.2f)\n",
               g_diagLastState, snOld, state, snNew, posX, posY, posZ);
         }
         g_diagLastState = state;
      }

      void* cargo = *(void**)((char*)ecx + kCargoSlots_offset + kCargoSlot_ObjPtr);
      if (cargo) {
         g_diagFrameCount++;
         if (g_diagFrameCount % 120 == 0) {
            float progress   = *(float*)((char*)ecx + kProgress_offset);
            float groundDist = *(float*)((char*)ecx + kGroundDist_offset);
            float landedHt   = *(float*)((char*)ecx + kLandedHeight_offset);
            static const char* stateNames[] = {
               "LANDED", "ASCENDING", "FLYING", "LANDING", "DYING", "DEAD"
            };
            const char* snm = (state >= 0 && state <= 5) ? stateNames[state] : "???";
            auto fn = get_gamelog();
            if (fn) {
               fn("[Carrier] state=%d(%s) prog=%.3f gndDist=%.2f pos=(%.2f,%.2f,%.2f) landedHt=%.2f\n",
                  state, snm, progress, groundDist, posX, posY, posZ, landedHt);
            }
         }
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   return alive;
}

// ---------------------------------------------------------------------------
// EntityCarrier::UpdateLandedHeight — diagnostic logging
//   __thiscall(EntityCarrier* this_inner)    (this_inner = struct_base, NOT +0x240)
//   Ghidra VA: 0x004D8130      (thunk at 0x004126DE)
// ---------------------------------------------------------------------------

static constexpr uintptr_t kUpdateLandedHeight_addr = 0x004D8130;

static constexpr uintptr_t kInner_mLandedHt    = 0x600;  // float
static constexpr uintptr_t kInner_mClass       = 0x66C;  // EntityCarrierClass*
static constexpr uintptr_t kInner_mCargoCount  = 0x1E20; // int

using fn_UpdateLandedHeight_t = void(__fastcall*)(void* ecx, void* edx);
static fn_UpdateLandedHeight_t original_UpdateLandedHeight = nullptr;

static void __fastcall hooked_UpdateLandedHeight(void* ecx, void* /*edx*/)
{
   float beforeHt = -999.f;
   __try {
      beforeHt = *(float*)((char*)ecx + kInner_mLandedHt);
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   original_UpdateLandedHeight(ecx, nullptr);

   __try {
      float afterHt = *(float*)((char*)ecx + kInner_mLandedHt);
      int cargoCount = *(int*)((char*)ecx + kInner_mCargoCount);
      void* cls = *(void**)((char*)ecx + kInner_mClass);
      float classHt = -1.f;
      if (cls) {
         __try {
            classHt = *(float*)((char*)cls + kClassLandedHt_off);
         } __except(EXCEPTION_EXECUTE_HANDLER) {}
      }

      if (cargoCount > 0) {
         auto fn = get_gamelog();
         if (fn) {
            fn("[Carrier:ULH] classHt=%.2f before=%.2f after=%.2f "
               "cargoCount=%d delta=%.2f\n",
               classHt, beforeHt, afterHt, cargoCount, afterHt - classHt);
         }
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void entity_carrier_fixes_install(uintptr_t exe_base)
{
   original_SetProperty    = (fn_SetProperty_t)  resolve(exe_base, kSetProperty_addr);
   original_AttachCargo    = (fn_AttachCargo_t)   resolve(exe_base, kAttachCargo_addr);
   original_DetachCargo    = (fn_DetachCargo_t)   resolve(exe_base, kDetachCargo_addr);
   original_TakeOff        = (fn_TakeOff_t)       resolve(exe_base, kTakeOff_addr);
   g_carrierVtable         = resolve(exe_base, kEntityCarrier_vtable);
   original_CarrierUpdate  = (fn_CarrierUpdate_t) resolve(exe_base, kCarrierUpdate_addr);
   original_UpdateLandedHeight = (fn_UpdateLandedHeight_t)resolve(exe_base, kUpdateLandedHeight_addr);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SetProperty,       hooked_SetProperty);
   DetourAttach(&(PVOID&)original_AttachCargo,       hooked_AttachCargo);
   DetourAttach(&(PVOID&)original_DetachCargo,       hooked_DetachCargo);
   DetourAttach(&(PVOID&)original_TakeOff,           hooked_TakeOff);
   DetourAttach(&(PVOID&)original_CarrierUpdate,     hooked_CarrierUpdate);
   DetourAttach(&(PVOID&)original_UpdateLandedHeight, hooked_UpdateLandedHeight);
   DetourTransactionCommit();
}

void entity_carrier_fixes_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_SetProperty)        DetourDetach(&(PVOID&)original_SetProperty,        hooked_SetProperty);
   if (original_AttachCargo)        DetourDetach(&(PVOID&)original_AttachCargo,        hooked_AttachCargo);
   if (original_DetachCargo)        DetourDetach(&(PVOID&)original_DetachCargo,        hooked_DetachCargo);
   if (original_TakeOff)            DetourDetach(&(PVOID&)original_TakeOff,            hooked_TakeOff);
   if (original_CarrierUpdate)      DetourDetach(&(PVOID&)original_CarrierUpdate,      hooked_CarrierUpdate);
   if (original_UpdateLandedHeight) DetourDetach(&(PVOID&)original_UpdateLandedHeight, hooked_UpdateLandedHeight);
   DetourTransactionCommit();
}
