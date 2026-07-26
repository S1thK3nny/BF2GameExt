#include "pch.h"
#include "shield_channel_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>

// =============================================================================
// WeaponShield channel fix
//
// BUG: WeaponShield::Update reads the fire trigger directly and activates the
//      shield effect BEFORE calling the base Weapon::Update.  It never checks
//      whether the shield is the currently active weapon for its fire channel.
//      Pressing the fire button always toggles the shield, even when a
//      different weapon is selected for that channel.
//
//      WeaponShield is the only weapon type with this problem — all other
//      Update overrides that read the trigger (WeaponAreaEffect, etc.) gate
//      on mSelectedFlag first.
//
// FIX: Detours hook on WeaponShield::Update.  Before calling the original,
//      check whether this weapon is the active weapon for its fire channel.
//      If not, skip the shield-specific logic and call base Weapon::Update
//      directly (state machine + sound, no shield effects).
//
// NOTE: diverting to Weapon::Update also skips the shield-OFF path, so anything
//      already up stays up.  Harmless for the case above (the effect is never
//      created while deselected), but it means this hook must never divert an
//      owner whose layout it cannot actually read — see is_active_for_channel.
// =============================================================================

// Weapon struct offsets — build-invariant (verified in the Steam
// WeaponShield::Update disasm @0x691A80: [EBX+0x6c] owner, [EBX+0x74] trigger).
static constexpr int kWeapon_mOwner   = 0x6C;  // Controllable* (= entity ptr)
static constexpr int kWeapon_mTrigger = 0x74;  // Trigger*

// Entity offsets — relative to mOwner (= entity = struct_base+0x240).
// mControlFire is invariant across builds; the weapon array / channel→slot map
// shift by -0x10 on release, so those come from the active SoldierLayout.
static constexpr int kEntity_mControlFire = 0x38;   // Trigger[2], 4 bytes each

// ---------------------------------------------------------------------------
// Function types
// ---------------------------------------------------------------------------

typedef void (__thiscall* fn_ShieldUpdate_t)(void* ecx, float dt);
typedef void (__thiscall* fn_WeaponUpdate_t)(void* ecx, float dt);

static fn_ShieldUpdate_t original_ShieldUpdate = nullptr;
static fn_WeaponUpdate_t fn_WeaponUpdate       = nullptr;

// ---------------------------------------------------------------------------
// Channel-active check
//
// Every offset past mControlFire comes from SoldierLayout, i.e. it is only valid
// when the owner really is an EntitySoldier.  WeaponShield is not soldier-only:
// EntityDroideka owns one too, and there the soldier offsets land in unrelated
// memory (mZephyrSkeleton starts at +0x484, while weaponIndexMap is +0x510 and
// weaponArray +0x4F0).  Reading a byte there and treating it as a weapon slot
// could yield a bogus "some other weapon is active" and route the call away from
// the shield logic entirely — which would break the shield for that owner.
//
// Rather than add an RTTI probe, the layout validates itself: locate this very
// weapon inside the mWeapon[] array first.  A shield that is genuinely in the
// array proves the layout applies to this owner, and gives its slot index for
// free; not finding it means the offsets do not describe this entity, so allow
// the stock behaviour instead of guessing.
// ---------------------------------------------------------------------------
static bool is_active_for_channel(void* weapon)
{
   uintptr_t wpn     = (uintptr_t)weapon;
   uintptr_t owner   = *(uintptr_t*)(wpn + kWeapon_mOwner);
   uintptr_t trigger = *(uintptr_t*)(wpn + kWeapon_mTrigger);

   if (!owner || !trigger) return true;  // safety: allow

   // Derive channel from trigger pointer position within mControlFire[2].
   // mControlFire is a Controllable field, so this part is owner-agnostic.
   uintptr_t fireBase = owner + kEntity_mControlFire;
   if (trigger < fireBase) return true;

   int channel = (int)(trigger - fireBase) >> 2;
   if (channel < 0 || channel > 1) return true;  // not a soldier fire channel
   if (fireBase + channel * 4 != trigger) return true;  // misaligned

   // Positive-ID the soldier layout: this weapon must appear in mWeapon[8].
   int ownSlot = -1;
   for (int i = 0; i < 8; ++i) {
      if (*(uintptr_t*)(owner + g_soldier->weaponArray + i * 4) == wpn) { ownSlot = i; break; }
   }
   if (ownSlot < 0) return true;  // not an EntitySoldier layout — allow

   // Active weapon for this channel (mWeaponIndex[channel], 0xFF = empty)
   uint8_t activeSlot = *(uint8_t*)(owner + g_soldier->weaponIndexMap + channel);
   if (activeSlot >= 8) return true;  // safety

   return (activeSlot == (uint8_t)ownSlot);
}

// ---------------------------------------------------------------------------
// Detours hook
// ---------------------------------------------------------------------------
static void __fastcall hooked_ShieldUpdate(void* ecx, void* /*edx*/, float dt)
{
   if (is_active_for_channel(ecx)) {
      original_ShieldUpdate(ecx, dt);
   } else {
      fn_WeaponUpdate(ecx, dt);
   }
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void shield_channel_fix_install(uintptr_t exe_base)
{
   if (!g_addr->weapon_shield_update || !g_addr->weapon_update)
      return;
   if (g_addr->weapon_shield_update == g_addr->weapon_update)
      return;

   original_ShieldUpdate = (fn_ShieldUpdate_t)resolve(exe_base, g_addr->weapon_shield_update);
   fn_WeaponUpdate       = (fn_WeaponUpdate_t)resolve(exe_base, g_addr->weapon_update);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_ShieldUpdate, hooked_ShieldUpdate);
   DetourTransactionCommit();
}

void shield_channel_fix_uninstall()
{
   if (!original_ShieldUpdate) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_ShieldUpdate, hooked_ShieldUpdate);
   DetourTransactionCommit();
}
