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

// EntityCarrier instance offsets (from struct_base)
static constexpr uintptr_t kWorldX_offset       = 0x120;
static constexpr uintptr_t kWorldY_offset       = 0x124;
static constexpr uintptr_t kWorldZ_offset       = 0x128;
static constexpr uintptr_t kState_offset        = 0x5A4;  // int: 0=landed 1=ascending 2=flying 3=landing 4=dying 5=dead
static constexpr uintptr_t kLandedHeight_offset = 0x600;  // float: hover floor / landing threshold
static constexpr uintptr_t kFlightTimer_offset  = 0x604;  // float: flight timer
static constexpr uintptr_t kCargoSlots_offset   = 0x1DD0;
static constexpr uintptr_t kCargoSlot_ObjPtr    = 0x0C;
static constexpr uintptr_t kClass_offset        = 0x66C;  // EntityCarrierClass*

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
   bool alive = original_CarrierUpdate(ecx, nullptr, dt);
   if (!alive) return false;

   // One-shot confirmation that the hook is active and GameLog works from here.
   static bool s_loggedOnce = false;
   if (!s_loggedOnce) {
      s_loggedOnce = true;
      auto fn0 = get_gamelog();
      if (fn0) fn0("[Carrier] Update hook active, this=%p\n", ecx);
   }

   __try {
      // Only log while slot 0 has cargo (active carrier flight) or on state change.
      void* cargo = *(void**)((char*)ecx + kCargoSlots_offset + kCargoSlot_ObjPtr);
      int   state = *(int*)((char*)ecx + kState_offset);

      bool stateChanged = (state != g_diagLastState);
      if (stateChanged) g_diagLastState = state;

      if (!cargo && !stateChanged) return alive;

      g_diagFrameCount++;
      if (!stateChanged && (g_diagFrameCount % 60 != 0)) return alive;

      float wx = *(float*)((char*)ecx + kWorldX_offset);
      float wy = *(float*)((char*)ecx + kWorldY_offset);
      float wz = *(float*)((char*)ecx + kWorldZ_offset);
      float landedHt    = *(float*)((char*)ecx + kLandedHeight_offset);
      float flightTimer = *(float*)((char*)ecx + kFlightTimer_offset);

      void* cls = *(void**)((char*)ecx + kClass_offset);
      float classLandedHt  = cls ? *(float*)((char*)cls + kClassLandedHt_off) : -1.f;
      float classTakeoffHt = cls ? *(float*)((char*)cls + kClassTakeoffHt_off) : -1.f;

      static const char* stateNames[] = {
         "LANDED", "ASCENDING", "FLYING", "LANDING", "DYING", "DEAD"
      };
      const char* sn = (state >= 0 && state <= 5) ? stateNames[state] : "???";

      auto fn = get_gamelog();
      if (fn) {
         fn("[Carrier] state=%d(%s) pos=(%.1f,%.1f,%.1f) landedHt=%.2f "
            "classHt=%.2f takeoffHt=%.2f timer=%.1f cargo=%s\n",
            state, sn, wx, wy, wz, landedHt,
            classLandedHt, classTakeoffHt, flightTimer,
            cargo ? "YES" : "no");
      }
   } __except(EXCEPTION_EXECUTE_HANDLER) {
      // Bad offset read — silently ignore to avoid crashing the game.
   }
   return alive;
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void entity_carrier_fixes_install(uintptr_t exe_base)
{
   original_SetProperty    = (fn_SetProperty_t)  resolve(exe_base, kSetProperty_addr);
   original_AttachCargo    = (fn_AttachCargo_t)   resolve(exe_base, kAttachCargo_addr);
   original_DetachCargo    = (fn_DetachCargo_t)   resolve(exe_base, kDetachCargo_addr);
   original_CarrierUpdate  = (fn_CarrierUpdate_t)resolve(exe_base, kCarrierUpdate_addr);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SetProperty,   hooked_SetProperty);
   DetourAttach(&(PVOID&)original_AttachCargo,   hooked_AttachCargo);
   DetourAttach(&(PVOID&)original_DetachCargo,   hooked_DetachCargo);
   DetourAttach(&(PVOID&)original_CarrierUpdate, hooked_CarrierUpdate);
   DetourTransactionCommit();
}

void entity_carrier_fixes_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_SetProperty)   DetourDetach(&(PVOID&)original_SetProperty,   hooked_SetProperty);
   if (original_AttachCargo)   DetourDetach(&(PVOID&)original_AttachCargo,   hooked_AttachCargo);
   if (original_DetachCargo)   DetourDetach(&(PVOID&)original_DetachCargo,   hooked_DetachCargo);
   if (original_CarrierUpdate) DetourDetach(&(PVOID&)original_CarrierUpdate, hooked_CarrierUpdate);
   DetourTransactionCommit();
}
