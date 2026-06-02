#include "pch.h"
#include "soldier_upperbody_anim_guard.hpp"
#include "core/resolve.hpp"

#include <windows.h>
#include <stdint.h>

// See header for the full rationale. Vtable-swap guard, no Detours/trampoline:
// the target is a virtual, so we just replace the slot pointer (same pattern as
// the WeaponCannon::OverrideAimer hook in lua_hooks.cpp).

namespace {

// Original is __thiscall(this, float dt, SoldierAnimation* anim), RET 0x8.
// As a free function that matches the ABI: __fastcall(ECX=this, EDX=ignored,
// then the two stack args dt, anim) — the callee cleans 8 bytes, == RET 0x8.
typedef unsigned int(__thiscall* UpperAnim_t)(void* self, float dt, int* anim);

void** s_slot     = nullptr;   // vtable slot
void*  s_orig     = nullptr;   // original thunk (slot value before swap)
int*   s_nameCount = nullptr;  // SoldierAnimationBank distinct-name count (for diagnostics)

// ---- one-shot-per-anim diagnostics throttle --------------------------------
constexpr int kMaxSeen = 64;
uint32_t s_seen[kMaxSeen];
int      s_nSeen   = 0;
uint32_t s_skips   = 0;   // total frames skipped

bool seen_before(uint32_t id)
{
   for (int i = 0; i < s_nSeen; ++i)
      if (s_seen[i] == id) return true;
   if (s_nSeen < kMaxSeen) s_seen[s_nSeen++] = id;
   return false;
}

void report(int* anim)
{
   ++s_skips;

   uint32_t id = 0;
   __try { if (anim) id = (uint32_t)anim[0]; } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }

   if (seen_before(id)) return;   // log each distinct anim id once

   int names = -1;
   __try { if (s_nameCount) names = *s_nameCount; } __except (EXCEPTION_EXECUTE_HANDLER) {}

   clean_gamelog(
      "[animguard] suppressed corrupt upper-body animation (id=%08X) to avoid CTD at "
      "0x5789e8. A soldier class is exceeding the 16 distinct bank-NAME cap "
      "(registry now %d/16) — rework that class to <=16 names. Run 'listanimbanks'.\n",
      id, names);
}

// Replacement for the SoldierAnimator upper-body update vtable slot.
unsigned int __fastcall hooked_upper_anim(void* self, void* /*edx*/, float dt, int* anim)
{
   // One-time runtime confirmation that the vtable swap took effect (startup logs
   // are emitted before BFront2.log opens and are never seen). Fires the first
   // time a soldier's upper body actually updates in-match.
   static bool s_announced = false;
   if (!s_announced) {
      s_announced = true;
      clean_gamelog("[animguard] active — intercepting SoldierAnimator upper-body updates.\n");
   }

   bool bad = false;
   __try {
      if (!anim) {
         bad = true;
      } else {
         // anim->+8 is the anim data block; the original derefs it (byte[0] and
         // byte[3]) with no guard. Probe both bytes for readability.
         char* group = (char*)((int*)anim)[2];
         if (!group) {
            bad = true;
         } else {
            volatile char b0 = group[0];
            volatile char b3 = group[3];
            (void)b0; (void)b3;
         }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      bad = true;
   }

   if (bad) {
      report(anim);
      return 0;   // skip: leave the upper-body pose unchanged this frame
   }

   return ((UpperAnim_t)s_orig)(self, dt, anim);
}

} // namespace

void soldier_upperbody_anim_guard_install(uintptr_t exe_base)
{
   s_slot      = (void**)resolve(exe_base, game_addrs::modtools::soldier_upper_anim_vtable_slot);
   s_nameCount = (int*)  resolve(exe_base, game_addrs::modtools::anim_soldier_bank_count);

   void* expected_thunk = resolve(exe_base, game_addrs::modtools::soldier_upper_anim_thunk);
   void* expected_impl  = resolve(exe_base, game_addrs::modtools::soldier_upper_anim_impl);

   if (!s_slot) return;

   void* cur = *s_slot;
   if (cur != expected_thunk && cur != expected_impl) return;  // unexpected build — don't touch

   DWORD oldProt = 0;
   if (VirtualProtect(s_slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
      s_orig  = *s_slot;
      *s_slot = (void*)&hooked_upper_anim;
      VirtualProtect(s_slot, sizeof(void*), oldProt, &oldProt);
   }
}

void soldier_upperbody_anim_guard_uninstall()
{
   if (s_slot && s_orig) {
      DWORD oldProt = 0;
      if (VirtualProtect(s_slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
         *s_slot = s_orig;
         VirtualProtect(s_slot, sizeof(void*), oldProt, &oldProt);
      }
      s_orig = nullptr;
   }
}
