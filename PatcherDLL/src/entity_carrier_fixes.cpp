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
// EntityFlyer::Update computes struct_base as ECX - 0x240 (LEA EDI,[EBX-0x240] at 0x4fc98d).
// AttachCargo/UpdateLandedHeight receive struct_base (ECX - 0x240) and use +0x66C etc.
static constexpr uintptr_t kState_offset        = 0x364;  // int: 0=landed 1=ascending 2=flying 3=landing 4=dying 5=dead
static constexpr uintptr_t kProgress_offset     = 0x368;  // float: takeoff/landing progress (0..1)
static constexpr uintptr_t kLandedHeight_offset = 0x3C0;  // float: hover floor / landing threshold
static constexpr uintptr_t kFlightTimer_offset  = 0x3C4;  // float: flight timer
static constexpr uintptr_t kGroundDist_offset   = 0x490;  // float: last measured ground distance
static constexpr uintptr_t kCargoSlots_offset   = 0x1b90; // CargoSlot[4] (stride 0x14) — from Update disasm
static constexpr uintptr_t kCargoSlot_ObjPtr    = 0x0C;   // slot+0xC = object pointer
static constexpr uintptr_t kClass_offset        = 0x42C;  // EntityCarrierClass* (inner+0x66C, inner = ECX-0x240)
static constexpr uintptr_t kOccupantFlags_off   = 0x044;  // byte: (& 3)==3 means has pilot/occupant

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
// The AI carrier behavior (FUN_005af000) calls TakeOff when picking a new
// destination (AI state 4), even if the carrier is mid-landing (entity state 3).
// This forces entity state=1 (ASCENDING), interrupting the descent and creating
// an infinite LANDING→ASCENDING cycle.
//
// Fix: if entity is already in state 3 (LANDING), return early.  The landing
// physics will complete the descent naturally.  Once the carrier lands (state 0),
// the AI state machine will detect it and proceed normally.
// ---------------------------------------------------------------------------

static constexpr uintptr_t kTakeOff_addr = 0x004F8B70;
// Inner-base offset for flight state (struct_base + 0x5A4)
static constexpr uintptr_t kInner_mFlightState = 0x5A4;

using fn_TakeOff_t = void(__fastcall*)(void* ecx, void* edx);
static fn_TakeOff_t original_TakeOff = nullptr;

static void __fastcall hooked_TakeOff(void* ecx, void* /*edx*/)
{
   __try {
      int state = *(int*)((char*)ecx + kInner_mFlightState);
      if (state == 3) {
         // Carrier is landing — don't interrupt.  Let it reach the ground.
         auto fn = get_gamelog();
         if (fn) fn("[Carrier] TakeOff BLOCKED — entity is LANDING (state 3)\n");
         return;
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   original_TakeOff(ecx, nullptr);
}

// ---------------------------------------------------------------------------
// EntityCarrier::Update — diagnostic logging
//   __thiscall(EntityCarrier* this, float dt)
//   Ghidra VA: 0x004D7FE0
//
// Logs carrier state, position, landed height, and flight timer every ~1s
// and on every state change.  Output goes to the game's debug log.
// ---------------------------------------------------------------------------

static constexpr uintptr_t kCarrierUpdate_addr = 0x004D7FE0;

using fn_CarrierUpdate_t = bool(__fastcall*)(void* ecx, void* edx, float dt);
static fn_CarrierUpdate_t original_CarrierUpdate = nullptr;

static int  g_diagLastState = -1;
static int  g_diagFrameCount = 0;

static bool __fastcall hooked_CarrierUpdate(void* ecx, void* /*edx*/, float dt)
{
   // One-shot confirmation that the hook is active and GameLog works from here.
   static bool s_loggedOnce = false;
   if (!s_loggedOnce) {
      s_loggedOnce = true;
      auto fn0 = get_gamelog();
      if (fn0) fn0("[Carrier] Update hook active, this=%p\n", ecx);
   }

   // Capture state BEFORE original Update to detect transitions
   int stateBefore = -1;
   float progBefore = -1.f;
   float gndDistBefore = -1.f;
   float yPosBefore = -1.f;
   __try {
      stateBefore  = *(int*)((char*)ecx + kState_offset);
      progBefore   = *(float*)((char*)ecx + kProgress_offset);
      gndDistBefore = *(float*)((char*)ecx + kGroundDist_offset);
      // Y position from entity's position vector (mPose matrix row 3, Y component)
      // ECX is struct_base+0x240, mPose is at struct+0x110, row3 starts at +0x30
      // So posY = *(float*)(struct_base + 0x110 + 0x34) = *(float*)(ecx - 0x240 + 0x144)
      // Actually, the position is at this[-1].mSoundEngine.field_0xc in decompiled code
      // which corresponds to the world Y. Let's read from the matrix directly.
      // struct_base = ecx - 0x240, position row3 = struct_base + 0x110 + 0x30 = struct_base + 0x140
      // Y = struct_base + 0x144 = ecx - 0x240 + 0x144 = ecx - 0xFC
      yPosBefore = *(float*)((char*)ecx - 0xFC);
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   bool alive = original_CarrierUpdate(ecx, nullptr, dt);
   if (!alive) return false;

   __try {
      int   state = *(int*)((char*)ecx + kState_offset);
      void* cargo = *(void**)((char*)ecx + kCargoSlots_offset + kCargoSlot_ObjPtr);
      float landedHt    = *(float*)((char*)ecx + kLandedHeight_offset);
      float progress    = *(float*)((char*)ecx + kProgress_offset);
      float groundDist  = *(float*)((char*)ecx + kGroundDist_offset);
      float yPos        = *(float*)((char*)ecx - 0xFC);
      unsigned char occFlags = *(unsigned char*)((char*)ecx + kOccupantFlags_off);
      bool hasPilot = (occFlags & 3) == 3;

      // Detect state transitions — always log these
      if (stateBefore != state && stateBefore != -1) {
         auto fn = get_gamelog();
         if (fn) {
            static const char* sn[] = {"LANDED","ASCENDING","FLYING","LANDING","DYING","DEAD"};
            const char* snB = (stateBefore >= 0 && stateBefore <= 5) ? sn[stateBefore] : "???";
            const char* snA = (state >= 0 && state <= 5) ? sn[state] : "???";
            fn("[Carrier] *** TRANSITION %d(%s)->%d(%s) ***\n"
               "  before: prog=%.3f gndDist=%.2f yPos=%.2f\n"
               "  after:  prog=%.3f gndDist=%.2f yPos=%.2f landedHt=%.2f pilot=%s\n",
               stateBefore, snB, state, snA,
               progBefore, gndDistBefore, yPosBefore,
               progress, groundDist, yPos, landedHt,
               hasPilot ? "YES" : "NO");
         }
      }

      bool stateChanged = (state != g_diagLastState);
      if (stateChanged) g_diagLastState = state;

      if (!cargo && !stateChanged) return alive;

      g_diagFrameCount++;
      if (!stateChanged && (g_diagFrameCount % 60 != 0)) return alive;

      void* cls = *(void**)((char*)ecx + kClass_offset);
      float classLandedHt  = -1.f;
      float classTakeoffHt = -1.f;
      if (cls) {
         __try {
            classLandedHt  = *(float*)((char*)cls + kClassLandedHt_off);
            classTakeoffHt = *(float*)((char*)cls + kClassTakeoffHt_off);
         } __except(EXCEPTION_EXECUTE_HANDLER) {
            classLandedHt = -99.f; // signal: class read failed
         }
      }

      static const char* stateNames[] = {
         "LANDED", "ASCENDING", "FLYING", "LANDING", "DYING", "DEAD"
      };
      const char* snm = (state >= 0 && state <= 5) ? stateNames[state] : "???";

      auto fn = get_gamelog();
      if (fn) {
         fn("[Carrier] state=%d(%s) prog=%.3f gndDist=%.2f yPos=%.2f landedHt=%.2f "
            "classHt=%.2f takeoffHt=%.2f pilot=%s cargo=%s\n",
            state, snm, progress, groundDist, yPos, landedHt,
            classLandedHt, classTakeoffHt,
            hasPilot ? "YES" : "NO", cargo ? "YES" : "no");
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {
      static bool s_loggedExc = false;
      if (!s_loggedExc) {
         s_loggedExc = true;
         auto fn = get_gamelog();
         if (fn) fn("[Carrier] EXCEPTION in diagnostic read — bad offset?\n");
      }
   }
   return alive;
}

// ---------------------------------------------------------------------------
// EntityCarrier::UpdateLandedHeight — diagnostic logging
//   __thiscall(EntityCarrier* this_inner)    (this_inner = struct_base, NOT +0x240)
//   Ghidra VA: 0x004D8130      (thunk at 0x004126DE)
//
// Logs the base class height, per-cargo adjustment, and final landedHt.
// ---------------------------------------------------------------------------

static constexpr uintptr_t kUpdateLandedHeight_addr = 0x004D8130;

// Inner-base offsets (struct_base, NOT +0x240)
static constexpr uintptr_t kInner_mLandedHt    = 0x600;  // float
static constexpr uintptr_t kInner_mClass       = 0x66C;  // EntityCarrierClass*
static constexpr uintptr_t kInner_mCargoCount  = 0x1E20; // int

using fn_UpdateLandedHeight_t = void(__fastcall*)(void* ecx, void* edx);
static fn_UpdateLandedHeight_t original_UpdateLandedHeight = nullptr;

static void __fastcall hooked_UpdateLandedHeight(void* ecx, void* /*edx*/)
{
   // Read the class base height BEFORE the original updates it
   float beforeHt = -999.f;
   __try {
      beforeHt = *(float*)((char*)ecx + kInner_mLandedHt);
   } __except(EXCEPTION_EXECUTE_HANDLER) {}

   // Call original — it sets mLandedHeight from class base, then adjusts per cargo
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

      // Only log when there's cargo (otherwise it's just classHt copied over)
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
   original_TakeOff    = (fn_TakeOff_t)   resolve(exe_base, kTakeOff_addr);
   original_CarrierUpdate  = (fn_CarrierUpdate_t)resolve(exe_base, kCarrierUpdate_addr);
   original_UpdateLandedHeight = (fn_UpdateLandedHeight_t)resolve(exe_base, kUpdateLandedHeight_addr);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SetProperty,   hooked_SetProperty);
   DetourAttach(&(PVOID&)original_AttachCargo,   hooked_AttachCargo);
   DetourAttach(&(PVOID&)original_DetachCargo,   hooked_DetachCargo);
   DetourAttach(&(PVOID&)original_TakeOff,   hooked_TakeOff);
   DetourAttach(&(PVOID&)original_CarrierUpdate, hooked_CarrierUpdate);
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
   if (original_TakeOff)        DetourDetach(&(PVOID&)original_TakeOff,        hooked_TakeOff);
   if (original_CarrierUpdate)      DetourDetach(&(PVOID&)original_CarrierUpdate,      hooked_CarrierUpdate);
   if (original_UpdateLandedHeight) DetourDetach(&(PVOID&)original_UpdateLandedHeight, hooked_UpdateLandedHeight);
   DetourTransactionCommit();
}
