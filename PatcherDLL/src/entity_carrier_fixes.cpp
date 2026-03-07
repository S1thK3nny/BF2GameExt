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
// Custom carrier flight system
// ---------------------------------------------------------------------------
//
// PROBLEM
//   Vanilla carriers use progress-based interpolation for landing. The
//   carrier's position is lerped between a flight-path waypoint and the
//   VehiclePad based on a progress float (1.0 → 0.0). This causes the
//   carrier to land far from the pad because the interpolation start
//   point is wherever the carrier happens to be on the flight path —
//   not aligned with the pad's X/Z coordinates.
//
//   After cargo drop, vanilla's ascent (state 1) is purely vertical
//   progress-based interpolation with no forward movement. The carrier
//   rises straight up, then snaps to a flight-path position when it
//   reaches state 2. This looks unnatural.
//
// SOLUTION
//   We override the carrier's position during two phases:
//
//   1. DESCENT — quadratic easing toward the pad
//   2. ASCENT  — forward displacement while vanilla handles altitude
//
//   Everything else (AI direction, cargo attach/detach, state machine,
//   landing condition, flight altitude) stays vanilla.
//
// ─── PHASE 1: DESCENT (state 3 — LANDING) ───
//
//   When the carrier enters state 3, we capture its world position as
//   the interpolation start point. Each frame we compute:
//
//     t = clamp(elapsed / LandingTime, 0, 1)
//
//     X = lerp(startX, padX, t)          — linear horizontal
//     Z = lerp(startZ, padZ, t)          — linear horizontal
//     Y = lerp(startY, targetY, t*t)     — ease-in (t^2) vertical
//
//   targetY = padY + instanceLandedHt - 1.0
//
//   The ease-in on Y delays most of the altitude drop to the end of
//   the descent. This is critical because vanilla's landing condition
//   fires when groundDistance < LandedHeight. With ease-in, the carrier
//   crosses that threshold at t≈0.97, ensuring it's directly above the
//   pad when it lands. Ease-out (1-(1-t)^2) would cross at t≈0.74,
//   landing the carrier hundreds of units away.
//
//   We use the INSTANCE LandedHeight (ECX+0x3C0), not the class value.
//   The instance value includes the cargo bounding box and matches what
//   vanilla's landing condition actually checks.
//
// ─── PHASE 2: ASCENT (state 1 — ASCENDING, post-drop only) ───
//
//   After cargo drop (state 3 → 0 → TakeOff → state 1), vanilla's
//   state 1 raises the carrier to flight altitude via progress-based
//   Y interpolation. This is purely vertical — no forward movement.
//
//   We fix this by saving the carrier's heading (rotation row 1) at
//   TakeOff time and applying forward displacement to X/Z each frame:
//
//     if elapsed <= 3s:
//       displacement = TakeoffSpeed * elapsed^2 / 6    (ramp up)
//     else:
//       displacement = TakeoffSpeed * 1.5 + TakeoffSpeed * (elapsed - 3)
//
//   The speed ramps from 0 to TakeoffSpeed over ~3 seconds, simulating
//   the carrier regaining momentum after the stop-and-drop. After the
//   ramp, it continues at constant TakeoffSpeed.
//
//   We also restore the carrier's rotation and X/Z from a snapshot
//   taken at TakeOff time. This prevents the visual "jump" caused by
//   vanilla's movement controller teleporting the carrier to a flight-
//   path start point during state 1.
//
//   For the FIRST takeoff (pre-drop, carrier flying to the pad), only
//   X and rotation are locked from the snapshot. Z is left free so
//   vanilla's flight path drives forward movement naturally.
//
//   For POST-DROP takeoff, both X and Z are set from the snapshot +
//   our forward displacement. This gives smooth forward flight.
//
//   Re-landing prevention: if vanilla tries to set state 3 during
//   post-drop ascent, we force it back to state 1.
//
// ─── CARRIER LIFECYCLE ───
//
//   1. VehicleSpawn spawns carrier + cargo → state 3 (LANDING)
//      - UpdateSpawn assigns a CarrierFlightOverride slot
//      - Additional cargo spawned for multi-cargo carriers
//
//   2. Carrier descends (our PHASE 1) → lands → state 0 (LANDED)
//      - Vanilla's landing condition (gndDist < landedHt) triggers
//      - Cargo is detached (slot 0 by vanilla, slots 1-3 by us)
//
//   3. CalculateDest calls TakeOff → state 1 (ASCENDING)
//      - Our hooked TakeOff saves transform snapshot
//      - Post-drop: captures forward direction, activates PHASE 2
//
//   4. Carrier ascends (vanilla Y + our forward displacement)
//      - Vanilla's progress interpolation raises Y to TakeoffHeight
//      - Our forward displacement moves X/Z along the saved heading
//
//   5. Ascent completes → state 2 (FLYING)
//      - CalculateDest sees state 2 → destroys the carrier
//      - Safety despawn timer runs as fallback
//
// ─── ODF PROPERTIES ───
//
//   Set these in the carrier's ODF (EntityFlyer section):
//
//   | Property      | Offset | What it controls                              |
//   |---------------|--------|-----------------------------------------------|
//   | LandingTime   | +0x8EC | Descent duration (seconds). How long the      |
//   |               |        | carrier takes to fly from spawn point to pad. |
//   | TakeoffTime   | +0x8E4 | Ascent duration (seconds). How long the       |
//   |               |        | carrier takes to rise to flight altitude.      |
//   | TakeoffSpeed  | +0x8E8 | Post-drop forward speed (units/sec). How fast |
//   |               |        | the carrier flies away after dropping cargo.   |
//   |               |        | Speed ramps from 0 to this value over ~3s.    |
//   | TakeoffHeight | +0x8E0 | Flight altitude (units above pad Y). The      |
//   |               |        | carrier ascends to padY + TakeoffHeight.      |
//   | MinSpeed      | +0x88C | Vanilla flight speed. Controls spawn distance |
//   |               |        | (MinSpeed * LandingTime = distance from pad). |
//   |               |        | NOT used by our system for post-drop speed.   |
//   | LandingSpeed  | +0x8F0 | Not used by our system.                       |
//   | LandedHeight  | +0x8F4 | Auto-calculated from model bounding box.      |
//   |               |        | NOT an ODF property. Affects when vanilla's    |
//   |               |        | landing condition triggers (gndDist < this).   |
//
// ─── SLOT TRACKING ───
//
//   Three parallel arrays track per-carrier state:
//
//   - g_flightOverride[8]: pad position, class params, descent/ascent
//     state, forward direction, elapsed timers, despawn timer.
//     Assigned in UpdateSpawn, freed on carrier death.
//
//   - g_takeoffPos[8]: transform snapshot (X, Z, rotation rows) saved
//     at TakeOff time. Restored each frame during state 1 to prevent
//     visual jumping from the movement controller.
//
//   - g_animOverride[8]: animation progress override for the render hook.
//
//   All arrays are cleaned of stale entries (dead carriers) via
//   vtable validation whenever a new carrier spawns. This handles
//   carriers destroyed outside our Update hook (e.g., by CalculateDest)
//   and prevents stale data from interfering on match restart.
// ---------------------------------------------------------------------------

// EntityFlyerClass offsets for flight parameters
static constexpr uintptr_t kClassTakeoffTime_off   = 0x8E4;  // float: TakeoffTime (ascent duration)
static constexpr uintptr_t kClassTakeoffSpeed_off  = 0x8E8;  // float: TakeoffSpeed (post-drop forward speed)
static constexpr uintptr_t kClassLandingTime_off   = 0x8EC;  // float: LandingTime (descent duration)

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
   float  forwardSpeed;      // TakeoffSpeed (+0x8E8) — post-drop forward displacement speed
   float  landedHt;          // LandedHeight (+0x8F4) — bbox threshold

   // Descent state (state 3)
   float  landStartX, landStartY, landStartZ;
   float  landTargetY;       // padY + landedHt - 1.0
   float  landElapsed;
   bool   landActive;

   // Cargo team save/restore (disable spawning while carried)
   int    savedCargoTeam[4]; // team per cargo slot, -1 = not saved

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

// Per-carrier animation progress override (used by render hook)
struct CarrierAnimOverride {
   void* structBase;    // carrier struct_base (inner), nullptr = unused
   DWORD startTick;     // GetTickCount() at activation
   float duration;      // seconds to play from start → end
   float startProg;     // progress value at activation
   float endProg;       // progress value at end
   bool  active;
};
static CarrierAnimOverride g_animOverride[kMaxTrackedCarriers] = {};

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

   // Save cargo's team and set to 0 to disable spawning while carried.
   // ECX = struct_base in AttachCargo.
   __try {
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_flightOverride[i].structBase != ecx) continue;

         int* teamBits = (int*)((char*)cargo + 0x234);
         int team = (*teamBits >> 4) & 0xF;
         g_flightOverride[i].savedCargoTeam[slotIdx] = team;

         if (team != 0) {
            // SetTeam(0) via vtable[36]
            typedef void (__thiscall* SetTeam_t)(void* entity, int team);
            void** vtbl = *(void***)cargo;
            ((SetTeam_t)vtbl[36])(cargo, 0);
            // Clear team bits
            *teamBits = *teamBits & ~0xFF0; // clear bits 4-11

            auto fn = get_gamelog();
            if (fn) fn("[Carrier:%p] AttachCargo slot=%d: saved team %d, set to 0\n",
                       ecx, slotIdx, team);
         }
         break;
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}
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

   // Restore cargo team that was saved at attach time.
   if (hadCargo && cargoObj) {
      __try {
         for (int i = 0; i < kMaxTrackedCarriers; i++) {
            if (g_flightOverride[i].structBase != ecx) continue;
            int savedTeam = g_flightOverride[i].savedCargoTeam[slotIdx];
            if (savedTeam > 0) {
               typedef void (__thiscall* SetTeam_t)(void* entity, int team);
               void** vtbl = *(void***)cargoObj;
               ((SetTeam_t)vtbl[36])(cargoObj, savedTeam);
               int* teamBits = (int*)((char*)cargoObj + 0x234);
               *teamBits = *teamBits ^ (((savedTeam << 4) ^ *teamBits) & 0xF0);
               *teamBits = *teamBits ^ (((savedTeam << 8) ^ *teamBits) & 0xF00);

               auto fn = get_gamelog();
               if (fn) fn("[Carrier:%p] DetachCargo slot=%d: restored team %d\n",
                          ecx, slotIdx, savedTeam);
            }
            g_flightOverride[i].savedCargoTeam[slotIdx] = -1;
            break;
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}
   }

   // Activate animation override on first cargo slot drop — progress starts at 1.0
   // (fully deployed) and will be driven down toward 0.0 by the Update hook.
   if (hadCargo && slotIdx == 0) {
      int slot = -1;
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_animOverride[i].structBase == ecx || g_animOverride[i].structBase == nullptr) {
            slot = i; break;
         }
      }
      if (slot < 0) slot = 0;

      // Read animation duration from the takeoff anim's nFrames (class+0x87c -> +8)
      float dur = 3.0f; // fallback
      __try {
         void* classPtr = *(void**)((char*)ecx + kInner_mClass);
         if (classPtr) {
            void* takeoffAnim = *(void**)((char*)classPtr + 0x87c);
            if (takeoffAnim) {
               unsigned short nFrames = *(unsigned short*)((char*)takeoffAnim + 8);
               if (nFrames > 0) dur = (float)nFrames / 30.0f;
            }
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      g_animOverride[slot].structBase = ecx;
      g_animOverride[slot].startTick = GetTickCount();
      g_animOverride[slot].duration = dur;
      g_animOverride[slot].startProg = 0.0f;    // start at first frame
      g_animOverride[slot].endProg = 1.0f;      // end at last frame
      g_animOverride[slot].active = true;

      auto fn = get_gamelog();
      if (fn) fn("[Carrier:%p] Anim override activated (0.0 -> 1.0 over %.2fs)\n", ecx, dur);
   }
}

// ---------------------------------------------------------------------------
// EntityFlyer render — FUN_004f6970
//   __thiscall(void* this, uint param2, float param3, uint param4)
//   this = struct_base + 0x94
//
// Override the progress float for the render call when an animation override
// is active.  The vanilla render function uses progress to compute the
// animation frame: frame = (nFrames - 1) * progress.
// ---------------------------------------------------------------------------

static constexpr uintptr_t kFlyerRender_addr = 0x004f6970;
static constexpr uintptr_t kRender_thisToBase = 0x94;

using fn_FlyerRender_t = void(__fastcall*)(void* ecx, void* edx,
                                           unsigned int param2, float param3, unsigned int param4);
static fn_FlyerRender_t original_FlyerRender = nullptr;

// Visibility JZ bypass — NOP/restore the 6-byte JZ at 0x004f6999
// that skips the entire render when the bounding sphere fails frustum culling.
static unsigned char* s_visJzAddr = nullptr;

// RayHit call NOP — disable the two downward RayHit calls in EntityFlyer::Update
// (states 1 and 3) that read terrain surface normals and cause the carrier to
// swirl/wobble over uneven terrain.  Each is a 5-byte CALL instruction.
// Only applied when the carrier is above a height threshold so terrain alignment
// still works near the ground for natural landing.
static unsigned char* s_rayHitCall1 = nullptr;  // state 1: 0x004fe8cd
static unsigned char* s_rayHitCall2 = nullptr;  // state 3: 0x004feae2

static void visJzInit() {
   if (!s_visJzAddr) {
      uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
      s_visJzAddr = (unsigned char*)((0x004f6999 - kUnrelocatedBase) + base);
   }
}

static void rayHitInit() {
   if (!s_rayHitCall1) {
      uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
      s_rayHitCall1 = (unsigned char*)((0x004fe8cd - kUnrelocatedBase) + base);
      s_rayHitCall2 = (unsigned char*)((0x004feae2 - kUnrelocatedBase) + base);
   }
}

// Replace a 5-byte CALL with FLD1 (D9 E8) + 3×NOP.
// FLD1 pushes 1.0 onto FPU stack so the subsequent FMUL (×1024)
// produces a large ground distance (1024), preventing terrain normal influence.
static void rayHitNop(unsigned char saved1[5], unsigned char saved2[5]) {
   if (!s_rayHitCall1) return;
   static const unsigned char patch[5] = { 0xD9, 0xE8, 0x90, 0x90, 0x90 };
   DWORD oldProt;
   if (VirtualProtect(s_rayHitCall1, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
      memcpy(saved1, s_rayHitCall1, 5);
      memcpy(s_rayHitCall1, patch, 5);
      VirtualProtect(s_rayHitCall1, 5, oldProt, &oldProt);
   }
   if (VirtualProtect(s_rayHitCall2, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
      memcpy(saved2, s_rayHitCall2, 5);
      memcpy(s_rayHitCall2, patch, 5);
      VirtualProtect(s_rayHitCall2, 5, oldProt, &oldProt);
   }
}

static void rayHitRestore(const unsigned char saved1[5], const unsigned char saved2[5]) {
   if (!s_rayHitCall1) return;
   DWORD oldProt;
   if (VirtualProtect(s_rayHitCall1, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
      memcpy(s_rayHitCall1, saved1, 5);
      VirtualProtect(s_rayHitCall1, 5, oldProt, &oldProt);
   }
   if (VirtualProtect(s_rayHitCall2, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
      memcpy(s_rayHitCall2, saved2, 5);
      VirtualProtect(s_rayHitCall2, 5, oldProt, &oldProt);
   }
}

static void visJzNop(unsigned char savedOut[6]) {
   if (!s_visJzAddr) return;
   DWORD oldProt;
   if (VirtualProtect(s_visJzAddr, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
      memcpy(savedOut, s_visJzAddr, 6);
      memset(s_visJzAddr, 0x90, 6);
      VirtualProtect(s_visJzAddr, 6, oldProt, &oldProt);
   }
}

static void visJzRestore(const unsigned char saved[6]) {
   if (!s_visJzAddr) return;
   DWORD oldProt;
   if (VirtualProtect(s_visJzAddr, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
      memcpy(s_visJzAddr, saved, 6);
      VirtualProtect(s_visJzAddr, 6, oldProt, &oldProt);
   }
}

// Render hook — installed via Detour on FUN_004f6970.
// Intercepts ALL flyer renders but only applies animation override to carriers
// (identified by matching g_animOverride structBase).
static void __fastcall hooked_FlyerRender(void* ecx, void* /*edx*/,
                                          unsigned int param2, float param3, unsigned int param4)
{
   char* structBase = (char*)ecx - kRender_thisToBase;

   // Find active animation override for this carrier
   int overrideSlot = -1;
   for (int i = 0; i < kMaxTrackedCarriers; i++) {
      if (g_animOverride[i].structBase == (void*)structBase && g_animOverride[i].active) {
         overrideSlot = i;
         break;
      }
   }

   if (overrideSlot >= 0) {
      CarrierAnimOverride& ov = g_animOverride[overrideSlot];
      float elapsed = (float)(GetTickCount() - ov.startTick) / 1000.0f;
      float t = (ov.duration > 0.0f) ? elapsed / ov.duration : 1.0f;
      if (t > 1.0f) t = 1.0f;

      float animProg = ov.startProg + (ov.endProg - ov.startProg) * t;

      // Diagnostic: bounding sphere + world pos (limited to first 5 calls)
      static int renderDiagCount = 0;
      bool doDiag = (renderDiagCount < 5);
      if (doDiag) {
         renderDiagCount++;
         __try {
            // ecx+0x30 = bounding sphere center? ecx+0x3c = radius? (used by IsReflected)
            float bsX = *(float*)((char*)ecx + 0x30);
            float bsY = *(float*)((char*)ecx + 0x34);
            float bsZ = *(float*)((char*)ecx + 0x38);
            float bsR = *(float*)((char*)ecx + 0x3c);
            // World position for comparison (struct_base + 0x120/124/128)
            float wX = *(float*)(structBase + 0x120);
            float wY = *(float*)(structBase + 0x124);
            float wZ = *(float*)(structBase + 0x128);
            auto fn = get_gamelog();
            if (fn) fn("[RenderDiag:%p] bs=(%.1f,%.1f,%.1f) r=%.2f worldPos=(%.1f,%.1f,%.1f) lod=%u flags=0x%X\n",
                       structBase, bsX, bsY, bsZ, bsR, wX, wY, wZ, param2, param4);
         } __except(EXCEPTION_EXECUTE_HANDLER) {}
      }

      // Override progress, ref17dc (nFrames source), and network delta.
      float* progSlot = (float*)(structBase + 0x5A8);
      float* netDeltaSlot = (float*)(structBase + 0x1D00);
      void** ref17dcSlot = (void**)(structBase + 0x1870);
      float savedProg = 0.0f;
      float savedNetDelta = 0.0f;
      void* savedRef17dc = nullptr;
      bool  didFixRef17dc = false;
      __try {
         savedProg = *progSlot;
         *progSlot = animProg;
         savedNetDelta = *netDeltaSlot;
         *netDeltaSlot = 0.0f;
         savedRef17dc = *ref17dcSlot;
         // Always force ref17dc to the takeoff anim — it may point to the
         // wrong animation (e.g. landing anim) causing incorrect nFrames.
         {
            void* classPtr = *(void**)(structBase + kInner_mClass);
            if (classPtr) {
               void* takeoffAnim = *(void**)((char*)classPtr + 0x87c);
               if (takeoffAnim && *ref17dcSlot != takeoffAnim) {
                  *ref17dcSlot = takeoffAnim;
                  didFixRef17dc = true;
               }
            }
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // Bypass visibility/frustum cull + force LOD 0 (skinned mesh) for animation.
      unsigned char jzSaved[6];
      visJzNop(jzSaved);
      original_FlyerRender(ecx, nullptr, 0, param3, param4);
      visJzRestore(jzSaved);

      __try {
         *progSlot = savedProg;
         *netDeltaSlot = savedNetDelta;
         if (didFixRef17dc) *ref17dcSlot = savedRef17dc;
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // Keep override active at endProg — don't deactivate.
      // The override is cleaned up when the carrier is destroyed.
      return;
   }

   // For tracked carriers (even without active anim override), bypass the
   // frustum/visibility cull.  The carrier's bounding sphere is often too small
   // or offset, causing flicker when viewed from behind or up close.
   bool isTrackedCarrier = false;
   for (int i = 0; i < kMaxTrackedCarriers; i++) {
      if (g_flightOverride[i].structBase == (void*)structBase) {
         isTrackedCarrier = true;
         break;
      }
   }
   if (isTrackedCarrier) {
      // During descent (state 3, before cargo drop), force progress=0 so the
      // carrier shows frame 0 (closed/folded).  Vanilla's progress goes 1→0
      // during landing, which plays the animation backwards — we don't want that.
      float* progSlot = (float*)(structBase + 0x5A8);
      float savedProg = 0.0f;
      bool didOverrideProg = false;
      __try {
         int flightState = *(int*)(structBase + kInner_mFlightState);
         if (flightState == 3) {
            savedProg = *progSlot;
            *progSlot = 0.0f;
            didOverrideProg = true;
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      unsigned char jzSaved[6];
      visJzNop(jzSaved);
      original_FlyerRender(ecx, nullptr, param2, param3, param4);
      visJzRestore(jzSaved);

      if (didOverrideProg) {
         __try { *progSlot = savedProg; } __except(EXCEPTION_EXECUTE_HANDLER) {}
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
                  // Ramp from 0 to forwardSpeed over ~3s, then hold at full speed
                  float spd = g_flightOverride[j].forwardSpeed;
                  float ramp = 3.0f; // seconds to reach full speed
                  float t   = g_flightOverride[j].ascentElapsed;
                  float dist;
                  if (t <= ramp) {
                     dist = spd * t * t / (2.0f * ramp); // accelerating
                  } else {
                     dist = spd * ramp / 2.0f + spd * (t - ramp); // constant
                  }
                  *(float*)(inner + kInner_mPosX) = g_takeoffPos[i].savedX + g_flightOverride[j].fwdDirX * dist;
                  *(float*)(inner + kInner_mPosZ) = g_takeoffPos[i].savedZ + g_flightOverride[j].fwdDirZ * dist;
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

   // NOP the downward RayHit calls when the carrier is high above its pad.
   // This prevents terrain surface normals from causing heading/pitch wobble
   // at altitude.  Near the ground (within 2× landedHt), leave RayHit active
   // so the carrier aligns naturally to terrain for landing.
   bool didNopRayHit = false;
   unsigned char rhSaved1[5] = {}, rhSaved2[5] = {};
   __try {
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_flightOverride[i].structBase != (void*)inner) continue;
         float posY = *(float*)(inner + kInner_mPosY);
         float padY = g_flightOverride[i].padY;
         float landedHt = g_flightOverride[i].landedHt;
         float heightAbovePad = posY - padY;
         // NOP raycasts when more than 2× landedHt above pad (or at least 10 units)
         float threshold = (landedHt * 2.0f > 10.0f) ? landedHt * 2.0f : 10.0f;
         if (heightAbovePad > threshold) {
            rayHitNop(rhSaved1, rhSaved2);
            didNopRayHit = true;
         }
         break;
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   bool alive = original_CarrierUpdate(ecx, nullptr, dt);

   if (didNopRayHit) {
      rayHitRestore(rhSaved1, rhSaved2);
   }

   if (!alive) {
      // Clean up animation override for destroyed carriers
      for (int i = 0; i < kMaxTrackedCarriers; i++) {
         if (g_animOverride[i].structBase == (void*)inner) {
            g_animOverride[i] = {};
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
                  float spd = g_flightOverride[j].forwardSpeed;
                  float ramp = 3.0f;
                  float t   = g_flightOverride[j].ascentElapsed;
                  float dist;
                  if (t <= ramp) {
                     dist = spd * t * t / (2.0f * ramp);
                  } else {
                     dist = spd * ramp / 2.0f + spd * (t - ramp);
                  }
                  *(float*)(inner + kInner_mPosX) = g_takeoffPos[i].savedX + g_flightOverride[j].fwdDirX * dist;
                  *(float*)(inner + kInner_mPosZ) = g_takeoffPos[i].savedZ + g_flightOverride[j].fwdDirZ * dist;
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

   // Sync bounding sphere to FINAL world position (after all position overrides).
   // The engine does not update the scene-object bounding sphere for carriers,
   // leaving it at the spawn position.  Even with the correct center, the
   // default radius (~23 units) is too small, causing frustum-cull flicker.
   __try {
      *(float*)(inner + 0xC4) = *(float*)(inner + kInner_mPosX);
      *(float*)(inner + 0xC8) = *(float*)(inner + kInner_mPosY);
      *(float*)(inner + 0xCC) = *(float*)(inner + kInner_mPosZ);
      if (*(float*)(inner + 0xD0) < 80.0f) *(float*)(inner + 0xD0) = 80.0f;
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

         // Also evict stale snapshot entries (same logic: dead carrier = bad vtable)
         for (int i = 0; i < kMaxTrackedCarriers; i++) {
            if (g_takeoffPos[i].ecx == nullptr) continue;
            __try {
               void* vtbl = *(void**)g_takeoffPos[i].ecx;
               if (vtbl != g_carrierVtable) {
                  g_takeoffPos[i] = {};
               }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
               g_takeoffPos[i] = {};
            }
         }

         // Also evict stale animation override entries
         for (int i = 0; i < kMaxTrackedCarriers; i++) {
            if (g_animOverride[i].structBase == nullptr) continue;
            __try {
               void* vtbl = *(void**)g_animOverride[i].structBase;
               if (vtbl != g_carrierVtable) {
                  g_animOverride[i] = {};
               }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
               g_animOverride[i] = {};
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
         for (int c = 0; c < kMaxCargo; c++) g_flightOverride[loSlot].savedCargoTeam[c] = -1;

         // Cache class params from EntityFlyerClass
         void* classPtr = *(void**)(carrierStructBase + kInner_mClass);
         if (classPtr) {
            float takeoffHt  = *(float*)((char*)classPtr + kClassTakeoffHt_off);
            float takeoffTm  = *(float*)((char*)classPtr + kClassTakeoffTime_off);
            float takeoffSpd = *(float*)((char*)classPtr + kClassTakeoffSpeed_off);
            float landingTm  = *(float*)((char*)classPtr + kClassLandingTime_off);
            float landHt     = *(float*)((char*)classPtr + kClassLandedHt_off);

            g_flightOverride[loSlot].flightAltitude  = takeoffHt;
            g_flightOverride[loSlot].ascentDuration   = (takeoffTm > kMinDuration) ? takeoffTm : kMinDuration;
            g_flightOverride[loSlot].descentDuration  = (landingTm > kMinDuration) ? landingTm : kMinDuration;
            g_flightOverride[loSlot].forwardSpeed     = (takeoffSpd > 1.0f) ? takeoffSpd : 1.0f;
            g_flightOverride[loSlot].landedHt         = landHt;

            if (fn) fn("[Carrier:%p] Flight init: pad=(%.2f,%.2f,%.2f) alt=%.1f ascT=%.1f descT=%.1f takeoffSpd=%.1f lHt=%.2f\n",
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

         // Slot 0 cargo was attached inside original_UpdateSpawn, before the
         // flight override existed.  Save its team and set to 0 now.
         {
            void* cargo0 = *(void**)(carrierStructBase + kInner_mCargoSlot0Obj);
            if (cargo0) {
               __try {
                  int* teamBits = (int*)((char*)cargo0 + 0x234);
                  int team0 = (*teamBits >> 4) & 0xF;
                  g_flightOverride[loSlot].savedCargoTeam[0] = team0;
                  if (team0 != 0) {
                     typedef void (__thiscall* SetTeam_t)(void* entity, int t);
                     void** vtbl = *(void***)cargo0;
                     ((SetTeam_t)vtbl[36])(cargo0, 0);
                     *teamBits = *teamBits & ~0xFF0;
                     if (fn) fn("[Carrier:%p] Slot 0 cargo=%p: saved team %d, set to 0\n",
                                carrierStructBase, cargo0, team0);
                  }
               } __except(EXCEPTION_EXECUTE_HANDLER) {}
            }
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

         // 3b. Save cargo team and set to 0 (disable spawning while carried)
         for (int fi = 0; fi < kMaxTrackedCarriers; fi++) {
            if (g_flightOverride[fi].structBase != (void*)carrierStructBase) continue;
            g_flightOverride[fi].savedCargoTeam[slot] = team;
            ((SetTeam_t)cargoVtbl[36])(cargoEntity, 0);
            *teamBits = *teamBits & ~0xFF0;
            if (fn) fn("[Carrier:%p] Slot %d cargo=%p: saved team %d, set to 0\n",
                       carrierStructBase, slot, cargoEntity, team);
            break;
         }

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
   original_FlyerRender    = (fn_FlyerRender_t)   resolve(exe_base, kFlyerRender_addr);
   visJzInit();
   rayHitInit();
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
   DetourAttach(&(PVOID&)original_UpdateSpawn,       hooked_UpdateSpawn);
   DetourAttach(&(PVOID&)original_FlyerRender,      hooked_FlyerRender);
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
   if (original_UpdateSpawn)        DetourDetach(&(PVOID&)original_UpdateSpawn,        hooked_UpdateSpawn);
   if (original_FlyerRender)        DetourDetach(&(PVOID&)original_FlyerRender,        hooked_FlyerRender);
   DetourTransactionCommit();
}
