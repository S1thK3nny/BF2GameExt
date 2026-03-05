#include "pch.h"
#include "entity_carrier_fixes.hpp"

#include <cmath>
#include <detours.h>

// =============================================================================
// EntityCarrier / EntityCarrierClass bug fixes
//
// IMPORTANT: EntityCarrier meshes MUST have at least one collision primitive
// (p_ shape) in the .msh file.  EntityFlyerClass::SetProperty (0x004FA310)
// auto-generates a "main_body" capsule/cylinder from the model's bounding box
// if zero primitives are found.  This fallback collision is usually wrong
// (oversized cylinder) and causes visual/physics issues during flight.
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

static constexpr int kMaxTrackedCarriers = 8;

// ---------------------------------------------------------------------------
// Custom carrier flight system — quadratic descent/ascent with range trigger
// ---------------------------------------------------------------------------

// EntityFlyerClass offsets for flight parameters
static constexpr uintptr_t kClassTakeoffTime_off   = 0x8E4;  // float: TakeoffTime (ascent duration)
static constexpr uintptr_t kClassLandingTime_off   = 0x8EC;  // float: LandingTime (descent duration)
static constexpr uintptr_t kClassForwardSpeed_off  = 0x88C;  // float: MinSpeed (ODF MinSpeed, used by vanilla as flight speed)

// EntityFlyer::InitiateLanding — thiscall(struct_base), sets state=3
static constexpr uintptr_t kInitiateLanding_addr   = 0x004f1380;
using fn_InitiateLanding_t = void(__fastcall*)(void* ecx, void* edx);
static fn_InitiateLanding_t original_InitiateLanding = nullptr;

// EntityCarrier::Kill — thiscall(struct_base), destroys carrier
static constexpr uintptr_t kCarrierKill_addr       = 0x004D8400;
using fn_CarrierKill_t = void(__fastcall*)(void* ecx, void* edx);
static fn_CarrierKill_t g_CarrierKill = nullptr;

static constexpr float kDefaultDespawnDelay = 15.0f;
static constexpr float kMinDuration         = 2.0f;

struct CarrierFlightOverride {
   void*  structBase;        // carrier identity (struct_base ptr), nullptr = unused

   // Pad info (from VehicleSpawn at spawn time)
   float  padX, padY, padZ;

   // Class params (cached at spawn from EntityFlyerClass)
   float  flightAltitude;    // TakeoffHeight (+0x8E0) — cruise altitude above pad
   float  ascentDuration;    // TakeoffTime (+0x8E4) — ascent duration (seconds)
   float  descentDuration;   // LandingTime (+0x8EC) — descent duration (seconds)
   float  forwardSpeed;      // MinSpeed (+0x88C) — vanilla flight speed, used for post-drop forward displacement
   float  landedHt;          // LandedHeight (+0x8F4) — bbox threshold

   // Descent state (state 3)
   float  landStartX, landStartY, landStartZ;
   float  landTargetY;       // padY + landedHt - 1.0
   float  landElapsed;
   bool   landActive;

   // Post-drop ascent state (state 1, with forward movement)
   float  ascentElapsed;
   float  fwdDirX, fwdDirZ;  // forward direction at takeoff (normalized XZ)
   bool   ascentActive;

   // Despawn timer (after ascent completes)
   float  despawnTimer;      // seconds remaining, -1 = inactive
   bool   despawnActive;

   // State tracking
   int    lastState;
   bool   cargoDropped;      // true after first landing cycle completes
};
static CarrierFlightOverride g_flightOverride[kMaxTrackedCarriers] = {};

// Cargo slot offsets from struct_base (inner base)
static constexpr uintptr_t kInner_mCargoSlot0Obj = 0x1DDC; // first cargo slot object ptr
static constexpr uintptr_t kCargoSlotStride      = 0x14;   // stride between cargo slots

// Key struct_base (inner base) offsets used by multiple hooks
static constexpr uintptr_t kInner_mFlightState = 0x5A4;    // int: flight state
static constexpr uintptr_t kInner_mClass       = 0x66C;    // EntityCarrierClass*

// EntityFlyerClass offsets (from class pointer)
static constexpr uintptr_t kClassLandedHt_off   = 0x8F4;  // float: -(bbox_Y_min)
static constexpr uintptr_t kClassTakeoffHt_off  = 0x8E0;  // float: TakeoffHeight ODF property

// ---------------------------------------------------------------------------
// VehicleSpawn offsets (from UpdateSpawn's ECX)
// ---------------------------------------------------------------------------
static constexpr uintptr_t kVS_SpawnCount   = 0x7C;   // int: max active vehicles from this pad
static constexpr uintptr_t kVS_SpawnClass   = 0x90;   // EntityClass*[8] — cargo class per team
static constexpr uintptr_t kVS_UseCarrier   = 0xD0;   // bool[8] — carrier mode per team
static constexpr uintptr_t kVS_ListSentinel = 0xD8;   // linked list sentinel node
static constexpr uintptr_t kVS_TrackerCount = 0xE8;   // int: active VehicleTracker count
static constexpr uintptr_t kVS_CarrierPtr   = 0xEC;   // carrier entity ptr (PblHandle)
static constexpr uintptr_t kVS_CarrierGen   = 0xF0;   // carrier entity generation
static constexpr uintptr_t kVS_Team         = 0xF8;    // int: current vehicle team (1-based)
static constexpr uintptr_t kVS_PadTransform = 0x30;    // PblMatrix (16 floats)

// MemoryPool::Allocate — thiscall(MemoryPool*, uint size) → void*
static constexpr uintptr_t kMemPoolAlloc_addr       = 0x00802300;
typedef void* (__thiscall* fn_MemPoolAlloc_t)(void* pool, unsigned int size);
static fn_MemPoolAlloc_t g_MemPoolAlloc = nullptr;

// VehicleTracker::sMemoryPool global
static constexpr uintptr_t kVehicleTrackerPool_addr = 0x00B9A758;
static void* g_VehicleTrackerPool = nullptr;

// Dropoff animation state tracking (used by SetProperty, DetachCargo, render hooks)
struct CarrierDropoffState {
   void* structBase;    // carrier struct_base, nullptr = unused
   void* dropoffAnim;   // per-instance anim pointer (from this carrier's class bank)
   DWORD startTick;     // GetTickCount() when dropoff started
   float duration;      // animation duration in seconds (numFrames / 30.0)
   bool  active;
   bool  lookupDone;    // true if anim lookup was attempted for this carrier
};
static CarrierDropoffState g_dropoff[kMaxTrackedCarriers] = {};

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
      // Carrier class is being (re)initialized — invalidate cached dropoff anims
      // since animation data from the previous match has been freed.
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         g_dropoff[i].active = false;
         g_dropoff[i].structBase = nullptr;
         g_dropoff[i].dropoffAnim = nullptr;
         g_dropoff[i].lookupDone = false;
      }

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

// ---------------------------------------------------------------------------
// Dropoff animation support
//
// When cargo is dropped, we play the "dropoff" animation from the flyer's
// animation bank VISUALLY during the ascending phase (state 1).  The animation
// pointer at EntityFlyerClass+0x87c (takeoff anim) is swapped to the dropoff
// anim in the render hook for the duration of ascending.  No state machine
// interference — purely visual.
// ---------------------------------------------------------------------------

// ZephyrAnimBank::Find — thiscall on RedAnimation* (animation bank)
static constexpr uintptr_t kZephyrAnimBankFind_addr = 0x00803750;
typedef void* (__thiscall* fn_ZephyrAnimBankFind_t)(void* bank, const char* name);
static fn_ZephyrAnimBankFind_t g_ZephyrAnimBankFind = nullptr;

// EntityFlyerClass offsets for animation data
static constexpr uintptr_t kClassAnimBank_off    = 0x878;  // RedAnimation* (animation bank)
static constexpr uintptr_t kClassTakeoffAnim_off = 0x87c;  // ZephyrAnim* (takeoff anim)

static void __fastcall hooked_DetachCargo(void* ecx, void* /*edx*/, int slotIdx)
{
   if ((unsigned int)slotIdx >= (unsigned int)kMaxCargo) return; // OOB slot

   // Check if this slot has cargo BEFORE calling original (which clears it).
   // ECX in DetachCargo = struct_base (inner base).
   bool hadCargo = false;
   void* cargoObj = nullptr;
   __try {
      cargoObj = *(void**)((char*)ecx + kInner_mCargoSlot0Obj + slotIdx * kCargoSlotStride);
      hadCargo = (cargoObj != nullptr);
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   {
      auto fn = get_gamelog();
      if (fn) fn("[Carrier:%p] DetachCargo slot=%d cargoObj=%p hadCargo=%d\n",
                 ecx, slotIdx, cargoObj, hadCargo ? 1 : 0);
   }

   original_DetachCargo(ecx, nullptr, slotIdx);

   // Start dropoff animation on first cargo slot drop
   if (hadCargo && slotIdx == 0) {
      __try {
         // Find or allocate a dropoff slot for this carrier
         int slot = -1;
         for (int i = 0; i < kMaxTrackedCarriers; i++) {
            if (g_dropoff[i].structBase == ecx || g_dropoff[i].structBase == nullptr) {
               slot = i; break;
            }
         }
         if (slot < 0) slot = 0; // overwrite oldest if full

         g_dropoff[slot].structBase = ecx;

         // Per-instance anim lookup from this carrier's own animation bank
         if (!g_dropoff[slot].lookupDone && g_ZephyrAnimBankFind) {
            g_dropoff[slot].lookupDone = true;
            void* classPtr = *(void**)((char*)ecx + kInner_mClass);
            auto fn2 = get_gamelog();
            if (fn2) fn2("[Carrier:%p] Dropoff anim lookup: classPtr=%p\n", ecx, classPtr);
            if (classPtr) {
               void* bank = *(void**)((char*)classPtr + kClassAnimBank_off);
               if (fn2) fn2("[Carrier:%p]   bank=%p (classPtr+0x%X)\n", ecx, bank, (unsigned)kClassAnimBank_off);
               if (bank) {
                  g_dropoff[slot].dropoffAnim = g_ZephyrAnimBankFind(bank, "dropoff");
                  if (fn2) {
                     fn2("[Carrier:%p]   anim=%p\n", ecx, g_dropoff[slot].dropoffAnim);
                     if (g_dropoff[slot].dropoffAnim) {
                        unsigned short nFrames = *(unsigned short*)((char*)g_dropoff[slot].dropoffAnim + 8);
                        fn2("[Carrier:%p]   numFrames=%u\n", ecx, nFrames);
                     }
                  }
               } else {
                  if (fn2) fn2("[Carrier:%p]   bank is null — no dropoff anim\n", ecx);
               }
            }
         }

         if (g_dropoff[slot].dropoffAnim) {
            unsigned short nf = *(unsigned short*)((char*)g_dropoff[slot].dropoffAnim + 8);
            g_dropoff[slot].startTick = GetTickCount();
            g_dropoff[slot].duration = (float)nf / 30.0f;
            g_dropoff[slot].active = true;
            auto fn = get_gamelog();
            if (fn) fn("[Carrier:%p] Dropoff animation started (dur=%.2fs)\n",
                       ecx, g_dropoff[slot].duration);
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}
   }
}

// ---------------------------------------------------------------------------
// EntityFlyer render — FUN_004f6970
//   __thiscall(void* this, uint param2, float param3, uint param4)
//   this = struct_base + 0x94
//
// During ascending (state 1) after cargo drop, swap the takeoff animation
// pointer with the dropoff animation so the render shows the dropoff.
// ---------------------------------------------------------------------------

static constexpr uintptr_t kFlyerRender_addr = 0x004f6970;
static constexpr uintptr_t kRender_thisToBase = 0x94;

using fn_FlyerRender_t = void(__fastcall*)(void* ecx, void* edx,
                                           unsigned int param2, float param3, unsigned int param4);
static fn_FlyerRender_t original_FlyerRender = nullptr;

static void __fastcall hooked_FlyerRender(void* ecx, void* /*edx*/,
                                          unsigned int param2, float param3, unsigned int param4)
{
   char* structBase = (char*)ecx - kRender_thisToBase;

   // Check if this carrier has an active dropoff animation
   int dropoffSlot = -1;
   __try {
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_dropoff[i].structBase == (void*)structBase && g_dropoff[i].active) {
            dropoffSlot = i;
            break;
         }
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   if (dropoffSlot >= 0 && g_dropoff[dropoffSlot].dropoffAnim) {
      void* thisAnim = g_dropoff[dropoffSlot].dropoffAnim;

      // Compute time-based animation progress (independent of ascent speed)
      DWORD now = GetTickCount();
      float elapsed = (float)(now - g_dropoff[dropoffSlot].startTick) / 1000.0f;
      float animProgress = elapsed / g_dropoff[dropoffSlot].duration;
      if (animProgress > 1.0f) animProgress = 1.0f;

      // Swap both the animation pointer AND the progress float for this render call
      void** animSlot = nullptr;
      void*  savedAnim = nullptr;
      float* progSlot = (float*)(structBase + 0x5A8); // progress float
      float  savedProg = 0.0f;
      __try {
         void* classPtr = *(void**)(structBase + kInner_mClass);
         animSlot = (void**)((char*)classPtr + kClassTakeoffAnim_off);
         savedAnim = *animSlot;
         *animSlot = thisAnim;
         savedProg = *progSlot;
         *progSlot = animProgress;
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      original_FlyerRender(ecx, nullptr, param2, param3, param4);

      // Restore both
      __try {
         if (animSlot) *animSlot = savedAnim;
         *progSlot = savedProg;
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // Deactivate when animation is done
      if (animProgress >= 1.0f) {
         g_dropoff[dropoffSlot].active = false;
         g_dropoff[dropoffSlot].structBase = nullptr;
         auto fn = get_gamelog();
         if (fn) fn("[Carrier:%p] Dropoff animation finished (elapsed=%.2fs)\n", structBase, elapsed);
      }
      return;
   }

   original_FlyerRender(ecx, nullptr, param2, param3, param4);
}

// ---------------------------------------------------------------------------
// EntityFlyer::TakeOff — block landing interruption
//   __thiscall(EntityFlyer* this_inner)    (this_inner = struct_base)
//   Ghidra VA: 0x004F8B70      (thunk at 0x00408602)
//
// Fix: if entity is already in state 3 (LANDING), return early.
// ---------------------------------------------------------------------------

static constexpr uintptr_t kTakeOff_addr = 0x004F8B70;
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
static CarrierPosSnapshot g_takeoffPos[kMaxTrackedCarriers] = {};

static void __fastcall hooked_TakeOff(void* ecx, void* /*edx*/)
{
   __try {
      void* vtable = *(void**)ecx;
      if (vtable == g_carrierVtable) {
         int state = *(int*)((char*)ecx + kInner_mFlightState);
         if (state == 3) {
            auto fn = get_gamelog();
            if (fn) fn("[Carrier:%p] TakeOff BLOCKED — entity is LANDING (state 3)\n", ecx);
            return;
         }

         if (state == 0) {
            char* p = (char*)ecx;

            // Save full transform before TakeOff: rotation + position X/Z.
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

            // Post-drop: also capture forward direction for forward movement during ascent
            for (int j = 0; j < kMaxTrackedCarriers; j++) {
               if (g_flightOverride[j].structBase == ecx && g_flightOverride[j].cargoDropped) {
                  // Forward direction = rotation row 1 (struct_base+0x110)
                  float fwdX = *(float*)(p + 0x110);
                  float fwdZ = *(float*)(p + 0x118);
                  // Normalize XZ
                  float len = sqrtf(fwdX * fwdX + fwdZ * fwdZ);
                  if (len > 0.001f) { fwdX /= len; fwdZ /= len; }
                  g_flightOverride[j].fwdDirX = fwdX;
                  g_flightOverride[j].fwdDirZ = fwdZ;
                  g_flightOverride[j].ascentElapsed = 0.0f;
                  g_flightOverride[j].ascentActive = true;

                  auto fn = get_gamelog();
                  if (fn) fn("[Carrier:%p] Post-drop TakeOff: fwd=(%.3f,%.3f) speed=%.1f\n",
                             ecx, fwdX, fwdZ, g_flightOverride[j].forwardSpeed);
                  break;
               }
            }
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

   // Pre-Update: restore saved transform during ASCENDING (state 1).
   // The movement controller overwrites the entity's full transform each tick
   // to a flight-path position.  We restore rotation and lock X to prevent
   // visual jumping.  Z is left free for vanilla path movement.
   // Post-drop: also override X/Z with forward displacement from landing position.
   __try {
      void* carrierInner = (void*)inner;
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_takeoffPos[i].ecx != carrierInner || !g_takeoffPos[i].active) continue;
         int state = *(int*)((char*)ecx + kState_offset);
         if (state != 1) {
            g_takeoffPos[i].active = false;
            g_takeoffPos[i].ecx = nullptr;
         } else {
            memcpy(inner + 0x100, g_takeoffPos[i].rotRow0, 16);
            memcpy(inner + 0x110, g_takeoffPos[i].rotRow1, 16);
            // Check for post-drop forward displacement
            bool isPostDrop = false;
            for (int j = 0; j < kMaxTrackedCarriers; j++) {
               if (g_flightOverride[j].structBase == carrierInner && g_flightOverride[j].ascentActive) {
                  float offsetX = g_flightOverride[j].fwdDirX * g_flightOverride[j].forwardSpeed * g_flightOverride[j].ascentElapsed;
                  float offsetZ = g_flightOverride[j].fwdDirZ * g_flightOverride[j].forwardSpeed * g_flightOverride[j].ascentElapsed;
                  *(float*)(inner + kInner_mPosX) = g_takeoffPos[i].savedX + offsetX;
                  *(float*)(inner + kInner_mPosZ) = g_takeoffPos[i].savedZ + offsetZ;
                  isPostDrop = true;
                  break;
               }
            }
            if (!isPostDrop) {
               // First takeoff: only lock X, let Z be free for vanilla path
               *(float*)(inner + kInner_mPosX) = g_takeoffPos[i].savedX;
            }
         }
         break;
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   bool alive = original_CarrierUpdate(ecx, nullptr, dt);
   if (!alive) {
      // Clean up dropoff state for destroyed carriers
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_dropoff[i].structBase == (void*)inner) {
            g_dropoff[i].active = false;
            g_dropoff[i].structBase = nullptr;
         }
         if (g_flightOverride[i].structBase == (void*)inner) {
            g_flightOverride[i] = {};
         }
      }
      return false;
   }

   // Post-Update: restore again in case original Update overwrote transform.
   __try {
      void* carrierInner = (void*)inner;
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_takeoffPos[i].ecx != carrierInner || !g_takeoffPos[i].active) continue;
         int state = *(int*)((char*)ecx + kState_offset);
         if (state != 1) {
            g_takeoffPos[i].active = false;
            g_takeoffPos[i].ecx = nullptr;
         } else {
            memcpy(inner + 0x100, g_takeoffPos[i].rotRow0, 16);
            memcpy(inner + 0x110, g_takeoffPos[i].rotRow1, 16);
            bool isPostDrop = false;
            for (int j = 0; j < kMaxTrackedCarriers; j++) {
               if (g_flightOverride[j].structBase == carrierInner && g_flightOverride[j].ascentActive) {
                  float offsetX = g_flightOverride[j].fwdDirX * g_flightOverride[j].forwardSpeed * g_flightOverride[j].ascentElapsed;
                  float offsetZ = g_flightOverride[j].fwdDirZ * g_flightOverride[j].forwardSpeed * g_flightOverride[j].ascentElapsed;
                  *(float*)(inner + kInner_mPosX) = g_takeoffPos[i].savedX + offsetX;
                  *(float*)(inner + kInner_mPosZ) = g_takeoffPos[i].savedZ + offsetZ;
                  isPostDrop = true;
                  break;
               }
            }
            if (!isPostDrop) {
               *(float*)(inner + kInner_mPosX) = g_takeoffPos[i].savedX;
            }
         }
         break;
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   // Custom carrier flight system: range trigger, quadratic descent/ascent, despawn
   __try {
      int state = *(int*)((char*)ecx + kState_offset);

      // Diagnostic: detect carriers with no flight override slot
      bool foundSlot = false;
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_flightOverride[i].structBase == (void*)inner) { foundSlot = true; break; }
      }
      if (!foundSlot && state == 3) {
         // Check if this is actually a carrier (vtable match)
         void* vtable = *(void**)inner;
         if (vtable == g_carrierVtable) {
            static int missLogCount = 0;
            if (missLogCount++ < 5) {
               auto fn = get_gamelog();
               if (fn) {
                  fn("[Carrier:%p] WARNING: no flight override slot! state=%d slots=[%p,%p,%p,%p]\n",
                     inner, state,
                     g_flightOverride[0].structBase, g_flightOverride[1].structBase,
                     g_flightOverride[2].structBase, g_flightOverride[3].structBase);
               }
            }
         }
      }

      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_flightOverride[i].structBase != (void*)inner) continue;

         CarrierFlightOverride& fo = g_flightOverride[i];
         int prevState = fo.lastState;
         fo.lastState = state;

         float posX = *(float*)(inner + kInner_mPosX);
         float posY = *(float*)(inner + kInner_mPosY);
         float posZ = *(float*)(inner + kInner_mPosZ);

         // Per-carrier state transition log
         if (state != prevState) {
            auto fn = get_gamelog();
            if (fn) {
               static const char* sn[] = {"LANDED","ASCENDING","FLYING","LANDING","DYING","DEAD"};
               const char* snOld = (prevState >= 0 && prevState <= 5) ? sn[prevState] : "???";
               const char* snNew = (state >= 0 && state <= 5) ? sn[state] : "???";
               fn("[Carrier:%p] *** %d(%s)->%d(%s) *** pos=(%.2f,%.2f,%.2f)\n",
                  inner, prevState, snOld, state, snNew, posX, posY, posZ);
            }
         }

         // --- Quadratic descent (state 3) ---
         // On state 3 entry: capture start position
         if (state == 3 && prevState != 3 && !fo.landActive) {
            fo.landStartX = posX;
            fo.landStartY = posY;
            fo.landStartZ = posZ;
            fo.landElapsed = 0.0f;
            fo.landActive = true;

            // Use the INSTANCE landedHt (ECX+0x3C0), not the class value.
            // The instance value includes the cargo bounding box and is what
            // vanilla's landing condition (gndDist < landedHt) actually checks.
            float instanceLandedHt = *(float*)((char*)ecx + kLandedHeight_offset);
            fo.landTargetY = fo.padY + instanceLandedHt - 1.0f;

            auto fn = get_gamelog();
            if (fn) fn("[Carrier:%p] Descent: ACTIVE start=(%.2f,%.2f,%.2f) pad=(%.2f,%.2f,%.2f) targetY=%.2f instLH=%.2f dur=%.1fs\n",
                       inner, fo.landStartX, fo.landStartY, fo.landStartZ,
                       fo.padX, fo.padY, fo.padZ, fo.landTargetY, instanceLandedHt, fo.descentDuration);
         }

         // Override position each frame during state 3
         if (state == 3 && fo.landActive) {
            fo.landElapsed += dt;
            float t = fo.landElapsed / fo.descentDuration;
            if (t > 1.0f) t = 1.0f;

            // Ease-in on Y (t^2): slow initial descent, fast near ground.
            // This delays the gndDist < landedHt threshold crossing to t≈0.97,
            // ensuring the carrier lands on the pad, not hundreds of units away.
            // Ease-out (1-(1-t)^2) crossed the threshold at t≈0.74 for high-landedHt carriers.
            float tEased = t * t;

            float newX = fo.landStartX + (fo.padX - fo.landStartX) * t;
            float newY = fo.landStartY + (fo.landTargetY - fo.landStartY) * tEased;
            float newZ = fo.landStartZ + (fo.padZ - fo.landStartZ) * t;

            *(float*)(inner + kInner_mPosX) = newX;
            *(float*)(inner + kInner_mPosY) = newY;
            *(float*)(inner + kInner_mPosZ) = newZ;
         }

         // Deactivate descent when leaving state 3
         if (state != 3 && fo.landActive) {
            fo.landActive = false;
            if (state == 0) fo.cargoDropped = true;
            auto fn = get_gamelog();
            if (fn) fn("[Carrier:%p] Descent: DEACTIVATED (state=%d, cargoDropped=%d)\n",
                       inner, state, fo.cargoDropped ? 1 : 0);
         }

         // --- Post-drop ascent (state 1 with forward movement) ---
         // Vanilla's state 1 ascending handles Y via progress interpolation.
         // We add forward displacement to X/Z in the snapshot restore above.
         // Here we just track elapsed time and prevent re-landing.

         // Increment elapsed time for forward displacement calculation
         if (fo.ascentActive && state == 1) {
            fo.ascentElapsed += dt;
         }

         // Prevent vanilla from re-landing during post-drop ascent
         if (fo.ascentActive && state == 3) {
            *(int*)((char*)ecx + kState_offset) = 1;
            state = 1;
            auto fn = get_gamelog();
            if (fn) fn("[Carrier:%p] Post-drop ascent: blocked re-landing (forced state 3→1)\n", inner);
         }

         // Deactivate when carrier finishes ascending (state 1→2) or dies.
         // Start despawn timer as safety fallback (CalculateDest should kill at state 2).
         if (fo.ascentActive && state != 1 && state != 3) {
            fo.ascentActive = false;
            if (!fo.despawnActive) {
               fo.despawnTimer = kDefaultDespawnDelay;
               fo.despawnActive = true;
            }
            auto fn = get_gamelog();
            if (fn) fn("[Carrier:%p] Post-drop ascent: done (state=%d, elapsed=%.1fs), despawn fallback in %.1fs\n",
                       inner, state, fo.ascentElapsed, kDefaultDespawnDelay);
         }

         // --- Despawn timer ---
         if (fo.despawnActive) {
            fo.despawnTimer -= dt;
            if (fo.despawnTimer <= 0.0f && g_CarrierKill) {
               auto fn = get_gamelog();
               if (fn) fn("[Carrier:%p] Despawn: killing carrier\n", inner);
               fo.despawnActive = false;
               g_CarrierKill((void*)inner, nullptr);
               fo = {};
               break; // carrier is dead, stop processing
            }
         }

         break;
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   // Multi-slot detach: when carrier transitions to LANDED (state 0) from state 3,
   // CalculateDest only detaches slot 0.  We detach slots 1..3 here.
   // Checks if slot 1+ still has cargo — naturally runs only once since slots clear.
   __try {
      int state = *(int*)((char*)ecx + kState_offset);
      if (state == 0) {
         for (int s = 1; s < kMaxCargo; s++) {
            void* cargoObj = *(void**)((char*)inner + kInner_mCargoSlot0Obj + s * kCargoSlotStride);
            if (cargoObj) {
               auto fn = get_gamelog();
               if (fn) fn("[Carrier:%p] Multi-detach: detaching slot %d\n", inner, s);
               original_DetachCargo(inner, nullptr, s);
            }
         }
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   // Diagnostic: per-carrier state transitions (uses flight override's lastState
   // so each carrier logs independently instead of thrashing a shared global)
   __try {
      int   state = *(int*)((char*)ecx + kState_offset);
      float posX = *(float*)(inner + kInner_mPosX);
      float posY = *(float*)(inner + kInner_mPosY);
      float posZ = *(float*)(inner + kInner_mPosZ);

      // Find this carrier's flight override slot for per-carrier lastState
      // (lastState was already updated in the flight system block above,
      //  but prevState captured the value before the update — use that for logging)
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_flightOverride[i].structBase != (void*)inner) continue;
         // The flight system already logged transitions implicitly via lastState.
         // We log state transitions here using the fact that lastState was just updated.
         // Compare current state with what we stored — if lastState was just written
         // to 'state', we can detect transitions by checking prevState == state.
         // But prevState is local to the block above, so we use g_diagLastState
         // only for carriers WITHOUT a flight override. For tracked carriers,
         // transitions are logged in the flight override block already.
         break;
      }

      // Periodic diagnostic (every 120 frames, per-carrier using frame count)
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
               fn("[Carrier:%p] state=%d(%s) prog=%.3f gndDist=%.2f pos=(%.2f,%.2f,%.2f) landedHt=%.2f\n",
                  inner, state, snm, progress, groundDist, posX, posY, posZ, landedHt);
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
// VehicleSpawn::UpdateSpawn — multi-cargo carrier spawning
//   __thiscall(void* vehicleSpawn, float dt)
//   Ghidra VA: 0x00665A50
//
// After the original spawns carrier + 1 cargo (slot 0), we spawn additional
// cargo entities and attach them to slots 1..N-1, creating a VehicleTracker
// for each.  This fills carrier cargo slots based on SpawnCount.
// ---------------------------------------------------------------------------

static constexpr uintptr_t kUpdateSpawn_addr = 0x00665A50;

using fn_UpdateSpawn_t = void(__fastcall*)(void* ecx, void* edx, float dt);
static fn_UpdateSpawn_t original_UpdateSpawn = nullptr;

// Helper: allocate and link a VehicleTracker for a cargo entity
static void CreateTracker(char* vs, void* cargoEntity)
{
   if (!g_MemPoolAlloc || !g_VehicleTrackerPool || !cargoEntity) return;

   // Allocate 0x1C bytes from VehicleTracker::sMemoryPool
   int* tracker = (int*)g_MemPoolAlloc(g_VehicleTrackerPool, 0x1C);
   if (!tracker) return;

   // Zero all 7 dwords
   for (int i = 0; i < 7; i++) tracker[i] = 0;

   char* sentinel = vs + kVS_ListSentinel;

   // Set list pointers
   tracker[0] = (int)sentinel;        // +0x00: list owner
   tracker[1] = (int)sentinel;        // +0x04: list owner
   tracker[3] = (int)tracker;         // +0x0C: self-ptr

   // Set cargo entity reference
   tracker[4] = (int)cargoEntity;                    // +0x10: entity ptr
   tracker[5] = *(int*)((char*)cargoEntity + 0x204); // +0x14: entity generation

   // Link into doubly-linked list at head
   // sentinel+0x08 = first element ptr
   int* sentinelI = (int*)sentinel;
   tracker[2] = sentinelI[2];                     // new->next = old first
   sentinelI[2] = (int)tracker;                   // sentinel->first = new
   *(int*)(tracker[2] + 4) = (int)tracker;        // old_first->prev = new

   // Increment tracker count
   *(int*)(vs + kVS_TrackerCount) = *(int*)(vs + kVS_TrackerCount) + 1;
}

static void __fastcall hooked_UpdateSpawn(void* ecx, void* /*edx*/, float dt)
{
   char* vs = (char*)ecx;

   // Read tracker count BEFORE original call
   int countBefore = 0;
   __try {
      countBefore = *(int*)(vs + kVS_TrackerCount);
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   // Call original — spawns carrier + 1 cargo in slot 0 (if conditions met)
   original_UpdateSpawn(ecx, nullptr, dt);

   // Check if a carrier was just spawned
   __try {
      int countAfter = *(int*)(vs + kVS_TrackerCount);
      if (countAfter <= countBefore) return; // nothing spawned

      int team = *(int*)(vs + kVS_Team);
      int spawnCount = *(int*)(vs + kVS_SpawnCount);
      auto fn = get_gamelog();
      if (fn) fn("[Carrier] UpdateSpawn: spawned! countBefore=%d countAfter=%d team=%d spawnCount=%d\n",
                 countBefore, countAfter, team, spawnCount);

      if (team < 1 || team > 7) return;

      // Only apply to carrier spawns — game indexes by team directly (1-based)
      char useCarrier = *(char*)(vs + kVS_UseCarrier + team);
      if (!useCarrier) {
         if (fn) fn("[Carrier] UpdateSpawn: not a carrier spawn (useCarrier=0)\n");
         return;
      }

      // Get the carrier entity
      void* carrierEntity = *(void**)(vs + kVS_CarrierPtr);
      if (!carrierEntity) {
         if (fn) fn("[Carrier] UpdateSpawn: no carrier entity at +0xEC\n");
         return;
      }
      if (fn) fn("[Carrier] UpdateSpawn: carrierEntity=%p\n", carrierEntity);

      int carrierGen = *(int*)(vs + kVS_CarrierGen);
      if (*(int*)((char*)carrierEntity + 0x204) != carrierGen) {
         if (fn) fn("[Carrier] UpdateSpawn: carrier gen mismatch\n");
         return;
      }

      // Get carrier inner base (for AttachCargo, which expects struct_base as ECX)
      // carrierEntity from VehicleSpawn+0xEC — need to figure out if this is
      // struct_base or inner (struct_base+0x240).
      // Check vtable to determine: EntityCarrier vtable at struct_base[0].
      void* entityVtbl = *(void**)carrierEntity;
      if (fn) fn("[Carrier] UpdateSpawn: entity vtable=%p, expected carrier vtable~=%p\n",
                 entityVtbl, (void*)g_carrierVtable);

      // The entity from VehicleSpawn might already be the inner base (entity+0x240),
      // not the struct_base. If vtable doesn't match carrier vtable, try -0x240.
      char* carrierStructBase = nullptr;
      if ((uintptr_t)entityVtbl == (uintptr_t)g_carrierVtable) {
         carrierStructBase = (char*)carrierEntity;
         if (fn) fn("[Carrier] UpdateSpawn: entity IS struct_base (vtable matches)\n");
      } else {
         // Maybe it's inner base already — check struct_base = entity - 0x240
         char* maybeBase = (char*)carrierEntity - 0x240;
         void* maybeVtbl = *(void**)maybeBase;
         if ((uintptr_t)maybeVtbl == (uintptr_t)g_carrierVtable) {
            carrierStructBase = maybeBase;
            if (fn) fn("[Carrier] UpdateSpawn: entity is inner (+0x240), struct_base=%p\n", maybeBase);
         } else {
            if (fn) fn("[Carrier] UpdateSpawn: can't identify carrier base (vtbl=%p, -0x240 vtbl=%p)\n",
                       entityVtbl, maybeVtbl);
            return;
         }
      }

      // AttachCargo expects struct_base as ECX
      // Inner for class access = struct_base + 0x240

      // Store pad position and cache class params for flight override
      // PblMatrix is row-major 4×4; position = floats at indices 12,13,14
      {
         float* padMtx = (float*)(vs + kVS_PadTransform);
         float pX = padMtx[12], pY = padMtx[13], pZ = padMtx[14];

         // Evict stale slots: if the carrier at a slot's structBase is dead
         // (vtable no longer matches), free the slot for reuse.
         for (int i = 0; i < kMaxTrackedCarriers; i++) {
            if (g_flightOverride[i].structBase == nullptr) continue;
            __try {
               void* vtbl = *(void**)g_flightOverride[i].structBase;
               if (vtbl != g_carrierVtable) {
                  if (fn) fn("[Carrier:%p] Stale slot %d evicted (vtbl=%p)\n",
                             g_flightOverride[i].structBase, i, vtbl);
                  g_flightOverride[i] = {};
               }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
               if (fn) fn("[Carrier:%p] Stale slot %d evicted (access fault)\n",
                          g_flightOverride[i].structBase, i);
               g_flightOverride[i] = {};
            }
         }

         int loSlot = -1;
         for (int i = 0; i < kMaxTrackedCarriers; i++) {
            if (g_flightOverride[i].structBase == (void*)carrierStructBase ||
                g_flightOverride[i].structBase == nullptr) {
               loSlot = i; break;
            }
         }
         if (loSlot < 0) loSlot = 0; // overwrite oldest if full

         // Zero-init the slot, then populate
         g_flightOverride[loSlot] = {};
         g_flightOverride[loSlot].structBase = (void*)carrierStructBase;
         g_flightOverride[loSlot].padX = pX;
         g_flightOverride[loSlot].padY = pY;
         g_flightOverride[loSlot].padZ = pZ;
         g_flightOverride[loSlot].lastState = -1;
         g_flightOverride[loSlot].despawnTimer = -1.0f;

         // Cache class params from EntityFlyerClass
         void* classPtr = *(void**)(carrierStructBase + kInner_mClass);
         if (classPtr) {
            float takeoffHt  = *(float*)((char*)classPtr + kClassTakeoffHt_off);
            float takeoffTm  = *(float*)((char*)classPtr + kClassTakeoffTime_off);
            float landingTm  = *(float*)((char*)classPtr + kClassLandingTime_off);
            float fwdSpeed   = *(float*)((char*)classPtr + kClassForwardSpeed_off);
            float landHt     = *(float*)((char*)classPtr + kClassLandedHt_off);

            g_flightOverride[loSlot].flightAltitude  = takeoffHt;
            g_flightOverride[loSlot].ascentDuration   = (takeoffTm > kMinDuration) ? takeoffTm : kMinDuration;
            g_flightOverride[loSlot].descentDuration  = (landingTm > kMinDuration) ? landingTm : kMinDuration;
            g_flightOverride[loSlot].forwardSpeed     = (fwdSpeed > 1.0f) ? fwdSpeed : 1.0f;
            g_flightOverride[loSlot].landedHt         = landHt;

            if (fn) fn("[Carrier:%p] Flight init: pad=(%.2f,%.2f,%.2f) alt=%.1f ascT=%.1f descT=%.1f spd=%.1f lHt=%.2f\n",
                       carrierStructBase, pX, pY, pZ, takeoffHt, g_flightOverride[loSlot].ascentDuration,
                       g_flightOverride[loSlot].descentDuration, g_flightOverride[loSlot].forwardSpeed,
                       landHt);
         } else {
            // Fallback defaults
            g_flightOverride[loSlot].flightAltitude  = 100.0f;
            g_flightOverride[loSlot].ascentDuration   = 10.0f;
            g_flightOverride[loSlot].descentDuration  = 10.0f;
            g_flightOverride[loSlot].forwardSpeed     = 20.0f;
            g_flightOverride[loSlot].landedHt         = 5.0f;
            if (fn) fn("[Carrier:%p] Flight init: no class ptr, using defaults\n", carrierStructBase);
         }
      }

      // Read carrier class → mCargoCount
      // kInner_mClass (0x66C) is already from struct_base, NOT from inner
      void* carrierClass = *(void**)(carrierStructBase + kInner_mClass);
      if (!carrierClass) { if (fn) fn("[Carrier] UpdateSpawn: no carrier class\n"); return; }
      int cargoCount = *(int*)((char*)carrierClass + kMCargoCount_offset);
      if (fn) fn("[Carrier] UpdateSpawn: carrierClass=%p, cargoCount=%d\n", carrierClass, cargoCount);
      if (cargoCount <= 1) return; // only 1 slot defined, nothing extra to do

      // How many slots can we fill?
      int budget = spawnCount - countAfter; // remaining spawn budget
      int slotsToFill = cargoCount;
      if (slotsToFill > budget + 1) slotsToFill = budget + 1; // +1 because slot 0 already used 1
      if (slotsToFill > kMaxCargo) slotsToFill = kMaxCargo;

      if (slotsToFill <= 1) return; // slot 0 already filled, no budget for more

      // Get cargo spawn class
      void* spawnClass = *(void**)(vs + kVS_SpawnClass + team * 4);
      if (!spawnClass) return;

      // Get pad transform for spawning cargo at the right position
      // We use VehicleSpawn+0x30 (the pad's world transform matrix)
      char* padTransform = vs + kVS_PadTransform;

      if (fn) fn("[Carrier] Multi-cargo: filling %d slots (cargoCount=%d, budget=%d)\n",
                 slotsToFill, cargoCount, budget);

      for (int slot = 1; slot < slotsToFill; slot++) {
         if (fn) fn("[Carrier]   Slot %d: spawning cargo...\n", slot);

         // Spawn cargo entity: spawnClass->vtable[2](transform)
         void** vtbl = *(void***)spawnClass;
         typedef void* (__thiscall* SpawnEntity_t)(void* cls, void* transform);
         SpawnEntity_t spawnFn = (SpawnEntity_t)vtbl[2];
         void* spawned = spawnFn(spawnClass, padTransform);
         if (!spawned) { if (fn) fn("[Carrier]   Slot %d: spawn returned null\n", slot); continue; }

         // Get actual entity: spawned->vtable[9]()
         void** spawnedVtbl = *(void***)spawned;
         typedef void* (__thiscall* GetEntity_t)(void* obj);
         GetEntity_t getEntityFn = (GetEntity_t)spawnedVtbl[9];
         void* cargoEntity = getEntityFn(spawned);
         if (!cargoEntity) { if (fn) fn("[Carrier]   Slot %d: entity is null\n", slot); continue; }

         if (fn) fn("[Carrier]   Slot %d: cargo=%p\n", slot, cargoEntity);

         // Match original UpdateSpawn order exactly:
         // Original: AttachCargo → vtable[36](team) → team bits → vtable[5]

         // 1. Attach cargo to carrier first (original does this before team/activate)
         original_AttachCargo(carrierStructBase, nullptr, slot, cargoEntity);

         // 2. cargo->vtable[36](team) — SetTeam
         void** cargoVtbl = *(void***)cargoEntity;
         typedef void (__thiscall* SetTeam_t)(void* entity, int team);
         ((SetTeam_t)cargoVtbl[36])(cargoEntity, team);

         // 3. Set team bits 0xF0 and 0xF00
         int* teamBits = (int*)((char*)cargoEntity + 0x234);
         *teamBits = *teamBits ^ (((team << 4) ^ *teamBits) & 0xF0);
         *teamBits = *teamBits ^ (((team << 8) ^ *teamBits) & 0xF00);

         // 4. cargo->vtable[5]() — activation (in own __try so tracker still created)
         __try {
            typedef void (__thiscall* Activate_t)(void* entity);
            ((Activate_t)cargoVtbl[5])(cargoEntity);
            if (fn) fn("[Carrier]   Slot %d: activated OK\n", slot);
         } __except(EXCEPTION_EXECUTE_HANDLER) {
            if (fn) fn("[Carrier]   Slot %d: vtable[5] exception (non-fatal)\n", slot);
         }

         // 5. Create VehicleTracker (runs even if activation failed)
         CreateTracker(vs, cargoEntity);

         if (fn) fn("[Carrier]   Slot %d: done!\n", slot);
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {
      auto fn = get_gamelog();
      if (fn) fn("[Carrier] Multi-cargo: exception during extra spawn\n");
   }
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
   g_ZephyrAnimBankFind    = (fn_ZephyrAnimBankFind_t)resolve(exe_base, kZephyrAnimBankFind_addr);
   original_FlyerRender    = (fn_FlyerRender_t)   resolve(exe_base, kFlyerRender_addr);
   original_UpdateSpawn    = (fn_UpdateSpawn_t)   resolve(exe_base, kUpdateSpawn_addr);
   g_MemPoolAlloc          = (fn_MemPoolAlloc_t)  resolve(exe_base, kMemPoolAlloc_addr);
   g_VehicleTrackerPool    = resolve(exe_base, kVehicleTrackerPool_addr);
   original_InitiateLanding = (fn_InitiateLanding_t)resolve(exe_base, kInitiateLanding_addr);
   g_CarrierKill           = (fn_CarrierKill_t)   resolve(exe_base, kCarrierKill_addr);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SetProperty,       hooked_SetProperty);
   DetourAttach(&(PVOID&)original_AttachCargo,       hooked_AttachCargo);
   DetourAttach(&(PVOID&)original_DetachCargo,       hooked_DetachCargo);
   DetourAttach(&(PVOID&)original_TakeOff,           hooked_TakeOff);
   DetourAttach(&(PVOID&)original_CarrierUpdate,     hooked_CarrierUpdate);
   DetourAttach(&(PVOID&)original_UpdateLandedHeight, hooked_UpdateLandedHeight);
   DetourAttach(&(PVOID&)original_FlyerRender,       hooked_FlyerRender);
   DetourAttach(&(PVOID&)original_UpdateSpawn,       hooked_UpdateSpawn);
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
   if (original_FlyerRender)        DetourDetach(&(PVOID&)original_FlyerRender,        hooked_FlyerRender);
   if (original_UpdateSpawn)        DetourDetach(&(PVOID&)original_UpdateSpawn,        hooked_UpdateSpawn);
   DetourTransactionCommit();
}
