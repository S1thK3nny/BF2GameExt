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
// ---------------------------------------------------------------------------
static bool is_active_for_channel(void* weapon)
{
   uintptr_t wpn     = (uintptr_t)weapon;
   uintptr_t owner   = *(uintptr_t*)(wpn + kWeapon_mOwner);
   uintptr_t trigger = *(uintptr_t*)(wpn + kWeapon_mTrigger);

   if (!owner || !trigger) return true;  // safety: allow

   // Derive channel from trigger pointer position within mControlFire[2]
   uintptr_t fireBase = owner + kEntity_mControlFire;
   if (trigger < fireBase) return true;

   int channel = (int)(trigger - fireBase) >> 2;
   if (channel < 0 || channel > 1) return true;  // not a soldier fire channel
   if (fireBase + channel * 4 != trigger) return true;  // misaligned

   // Look up active weapon for this channel (mWeaponIndex[channel], 0xFF empty)
   uint8_t activeSlot = *(uint8_t*)(owner + g_soldier->weaponIndexMap + channel);
   if (activeSlot >= 8) return true;  // safety

   uintptr_t activeWpn = *(uintptr_t*)(owner + g_soldier->weaponArray + activeSlot * 4);
   return (activeWpn == wpn);
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
