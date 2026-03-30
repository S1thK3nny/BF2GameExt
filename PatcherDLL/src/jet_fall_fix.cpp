#include "pch.h"

#include "jet_fall_fix.hpp"
#include "cfile.hpp"
#include <detours.h>

// ============================================================================
// JET -> FALL animation fix
// ============================================================================
//
// EndJetJump is a __thiscall bool() on the Controllable sub-object.
// It checks mState for JET_JUMP(6)/JET_HOVER(7) and calls SetState(FALL).
//
// From EndJetJump's `this` (Controllable*):
//   mState            = this + state_off      (SoldierState enum)
//   mOldState         = this + state_off + 4
//   mSoldierAnimator* = this + animator_off
//
// SoldierAnimator:
//   mSoldierAction    = animator + 0x70   (SoldierState enum, same all builds)
// ============================================================================

// SoldierState enum values
static constexpr uint32_t STATE_JUMP = 4;
static constexpr uint32_t STATE_FALL = 8;

// Per-build addresses
struct jet_fall_addrs {
   uintptr_t id_rva;
   uint64_t  id_expected;
   uintptr_t end_jet_jump_offset;   // EndJetJump function offset from exe base
   uint32_t  state_off;             // mState offset from Controllable this
   uint32_t  animator_off;          // mSoldierAnimator* offset from Controllable this
};

static const jet_fall_addrs MODTOOLS = {
   .id_rva              = 0x62b59c,
   .id_expected         = 0x746163696c707041,  // "Applicat"
   .end_jet_jump_offset = 0x144850,            // VA 0x544850
   .state_off           = 0x514,
   .animator_off        = 0x520,
};

static const jet_fall_addrs STEAM = {
   .id_rva              = 0x39f834,
   .id_expected         = 0x746163696c707041,
   .end_jet_jump_offset = 0x0ee1d0,            // VA 0x4ee1d0
   .state_off           = 0x504,
   .animator_off        = 0x510,
};

static const jet_fall_addrs GOG = {
   .id_rva              = 0x3a0698,
   .id_expected         = 0x746163696c707041,
   .end_jet_jump_offset = 0x0ee1d0,            // VA 0x4ee1d0 — same as Steam (TODO: verify)
   .state_off           = 0x504,
   .animator_off        = 0x510,
};

// ---------------------------------------------------------------------------
// Hook state
// ---------------------------------------------------------------------------
typedef bool (__thiscall* fn_EndJetJump)(void* self);
static fn_EndJetJump original_EndJetJump = nullptr;

static const jet_fall_addrs* active_addrs = nullptr;

// ---------------------------------------------------------------------------
// Hooked EndJetJump
// ---------------------------------------------------------------------------
static bool __fastcall hooked_EndJetJump(void* self, void* /*edx*/)
{
   bool result = original_EndJetJump(self);

   if (!result) return false;

   char* ctrl = (char*)self;
   uint32_t state = *(uint32_t*)(ctrl + active_addrs->state_off);

   if (state != STATE_FALL) return true;

   // Override mOldState to JUMP so the entity-level transition looks like JUMP->FALL
   *(uint32_t*)(ctrl + active_addrs->state_off + 4) = STATE_JUMP;

   // Override SoldierAnimator::mSoldierAction to JUMP so the animator
   // sees a JUMP->FALL transition (which has proper fall animation handling)
   char* animator = *(char**)(ctrl + active_addrs->animator_off);
   if (animator) {
      *(uint32_t*)(animator + 0x70) = STATE_JUMP;
   }

   return true;
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------
void jet_fall_fix_install(uintptr_t exe_base)
{
   cfile log("BF2GameExt.log", "a");

   const jet_fall_addrs* candidates[] = { &MODTOOLS, &STEAM, &GOG };

   for (const auto* addrs : candidates) {
      const char* id_ptr = (const char*)(exe_base + addrs->id_rva);
      if (memcmp(id_ptr, &addrs->id_expected, sizeof(addrs->id_expected)) != 0)
         continue;

      active_addrs = addrs;
      original_EndJetJump = (fn_EndJetJump)(exe_base + addrs->end_jet_jump_offset);

      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourAttach((PVOID*)&original_EndJetJump, (PVOID)hooked_EndJetJump);
      LONG status = DetourTransactionCommit();

      if (status == NO_ERROR) {
         log.printf("[JetFallFix] Installed EndJetJump hook at %p\n",
                    (void*)(exe_base + addrs->end_jet_jump_offset));
      } else {
         log.printf("[JetFallFix] Failed to install hook (Detours error %ld)\n", status);
         active_addrs = nullptr;
         original_EndJetJump = nullptr;
      }
      return;
   }

   log.printf("[JetFallFix] Could not identify executable — hook not installed\n");
}

void jet_fall_fix_uninstall()
{
   if (!original_EndJetJump) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach((PVOID*)&original_EndJetJump, (PVOID)hooked_EndJetJump);
   DetourTransactionCommit();

   original_EndJetJump = nullptr;
   active_addrs = nullptr;
}
