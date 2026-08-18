#include "pch.h"
#include "shield_channel_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>

// =============================================================================
// WeaponShield channel fix
//
// BUG: WeaponShield::Update reads the fire trigger directly and toggles the
//      shield BEFORE calling the base Weapon::Update.  Every other weapon type
//      that reads the trigger does so from inside Weapon::Update's state
//      machine, which gates each read on mSelectedFlag.  WeaponShield does not,
//      so pressing fire toggles the shield even when a different weapon is
//      selected for that channel.
//
// FIX: Detours hook on WeaponShield::Update.  When the shield is not the active
//      weapon for its fire channel, the original is still called -- only
//      mTrigger is swapped for a zeroed stand-in for the duration, so the
//      `(*mTrigger & 3) == 3` toggle test cannot fire.
//
//      Suppressing just the toggle matters, because everything else in
//      WeaponShield::Update has to keep running while the shield is deselected:
//
//        * A shield that is already up keeps draining (mAddShield per second).
//          When it reaches zero the function's OFF path is what destroys the
//          effect, stops the loop sound, plays ShieldOffSound and pulls the
//          shield body out of the collision manager.  Skipping the call left
//          the effect on screen and a stale collision body behind after the
//          shield itself had expired.
//        * A shield that is up needs its per-frame upkeep: effect attach, sound
//          gain, and repositioning the collision body.
//        * A shield with a finite clip count never consults the trigger at all
//          (see "Two shield modes" below) -- it is driven purely by shield
//          strength, so it must not be suppressed under any circumstances.
//
//      Only the toggle is affected, and only while deselected.
//
// BUG 2: nothing drops the shield when the soldier boards something.  A raised
//      shield survives EntitySoldier::EnterControllable, and since the effect is
//      attached to the soldier's geometry -- which is now parked at the
//      vehicle's origin -- the bubble ends up floating at the root of whatever
//      you climbed into.  The engine turns the jetpack effects off by hand at
//      exactly that point (TurnOffJetEffect / TurnOffJetIdleEffect); the shield
//      was simply left out.
//
// FIX 2: a second hook on EntitySoldier::EnterControllable drops every shield the
//      soldier owns as it boards, and the Update hook keeps them down for as
//      long as Character::mVehicle / mRemote is set.  Both go through the
//      engine's own OFF path rather than tearing the effect down by hand.
//
// Two shield modes (WeaponShield::Update, verified on Phantom @0x7D0190 and
// modtools @0x63F360):
//
//      m_pAmmoCounter->mMaxClips == FLT_MAX   -> player toggle, reads mTrigger
//      m_pAmmoCounter->mMaxClips  < FLT_MAX   -> on whenever shield strength > 0
//
//      Stock cis_weap_walk_droideka_shield.odf sets no clip count, so it is the
//      toggle variant.
// =============================================================================

// Weapon struct offsets -- build-invariant.  Verified in the Phantom
// (0x7D0190), modtools (0x63F360) and Steam (0x691A80) WeaponShield::Update
// disassembly: [ECX+0x6c] owner, [ECX+0x74] trigger.
static constexpr int kWeapon_mOwner   = 0x6C;  // Controllable* (= entity ptr)
static constexpr int kWeapon_mTrigger = 0x74;  // Trigger*

// Entity offsets -- relative to mOwner (= entity = struct_base+0x240).
// mControlFire is invariant across builds; the weapon array / channel->slot map
// shift by -0x10 on release, so those come from the active SoldierLayout.
static constexpr int kEntity_mControlFire = 0x38;   // Trigger[2], 4 bytes each
static constexpr int kEntity_mCharacter   = 0xCC;   // Character*

// Character offsets (struct is 0x1B0 on every build; see docs/RE/game_struct_reference.md).
static constexpr int kCharacter_mUnit    = 0x148;  // Controllable* -- the soldier
static constexpr int kCharacter_mVehicle = 0x14C;  // Controllable* -- what it boarded
static constexpr int kCharacter_mRemote  = 0x150;  // Controllable* -- deployed remote unit

// The shield's up/down state lives on the owner's Damageable, not on the weapon,
// which is why it survives a weapon switch (and a vehicle) by design.  Reached
// exactly the way WeaponShield::Update reaches it: the sub-object at owner+0x18,
// through its own vtable slot 0x20.
static constexpr int kEntity_mDamageableSub  = 0x18;
static constexpr int kDamageableSub_vtblGet  = 0x20;  // -> Damageable*
static constexpr int kDamageable_mFlags      = 0x1FC;
static constexpr uint32_t kShieldUpBit       = 1u << 5;

// ---------------------------------------------------------------------------
// Function types
// ---------------------------------------------------------------------------

// Returns bool (the base Weapon::Update result is passed straight through).
typedef bool (__thiscall* fn_ShieldUpdate_t)(void* ecx, float dt);

typedef bool  (__thiscall* fn_EnterControllable_t)(void* ecx, void* target);
typedef void* (__thiscall* fn_GetDamageable_t)(void* ecx);

static fn_ShieldUpdate_t      original_ShieldUpdate      = nullptr;
static fn_EnterControllable_t original_EnterControllable = nullptr;

// The address the WeaponShield vtable holds in slot 1.  Detours patches the
// function body, not the vtable, so this stays a valid "is this a WeaponShield?"
// fingerprint after the hook is installed.
static uintptr_t s_shieldUpdateAddr = 0;

// Stand-in for Controllable::mControlFire[n] while the shield is deselected.
// Trigger is four bytes and WeaponShield::Update only ever reads it, so one
// shared zeroed instance is enough.
static uint32_t s_nullTrigger = 0;

// ---------------------------------------------------------------------------
// Channel-active check
//
// Every offset past mControlFire comes from SoldierLayout, i.e. it is only valid
// when the owner really is an EntitySoldier.  WeaponShield is not soldier-only:
// EntityDroideka owns one too, and there the soldier offsets land in unrelated
// memory (mZephyrSkeleton starts at +0x484, while weaponIndexMap is +0x510 and
// weaponArray +0x4F0).  Reading a byte there and treating it as a weapon slot
// could yield a bogus "some other weapon is active" and suppress the toggle on
// an owner whose shield was never broken to begin with.
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
   if (ownSlot < 0) return true;  // not an EntitySoldier layout -- allow

   // Active weapon for this channel (mWeaponIndex[channel], 0xFF = empty)
   uint8_t activeSlot = *(uint8_t*)(owner + g_soldier->weaponIndexMap + channel);
   if (activeSlot >= 8) return true;  // safety

   return (activeSlot == (uint8_t)ownSlot);
}


// ---------------------------------------------------------------------------
// Shield state on the owner
// ---------------------------------------------------------------------------
static uint32_t* shield_state_flags(uintptr_t owner)
{
   void*  sub  = (void*)(owner + kEntity_mDamageableSub);
   void** vtbl = *(void***)sub;
   if (!vtbl) return nullptr;

   fn_GetDamageable_t get = (fn_GetDamageable_t)vtbl[kDamageableSub_vtblGet / 4];
   if (!get) return nullptr;

   void* dmg = get(sub);
   if (!dmg) return nullptr;

   return (uint32_t*)((uintptr_t)dmg + kDamageable_mFlags);
}

// Is the owner riding something -- vehicle, turret, deployed remote?  Once
// Character::mVehicle is set the soldier no longer holds its own controller, so
// its fire triggers are dead and the shield could never be toggled back down.
static bool owner_is_riding(uintptr_t owner)
{
   uintptr_t chr = *(uintptr_t*)(owner + kEntity_mCharacter);
   if (!chr) return false;

   return *(uintptr_t*)(chr + kCharacter_mVehicle) != 0
       || *(uintptr_t*)(chr + kCharacter_mRemote)  != 0;
}

// Run one update with the shield forced down, so the engine's own OFF path
// destroys the effect, stops the loop sound, plays ShieldOffSound and pulls the
// shield body out of the collision manager.
static void drive_shield_off(uintptr_t wpn)
{
   uintptr_t owner = *(uintptr_t*)(wpn + kWeapon_mOwner);
   if (!owner) return;

   uint32_t* flags = shield_state_flags(owner);
   if (!flags || !(*flags & kShieldUpBit)) return;  // already down

   *flags &= ~kShieldUpBit;

   void** trigger = (void**)(wpn + kWeapon_mTrigger);
   void*  saved   = *trigger;
   *trigger = &s_nullTrigger;
   original_ShieldUpdate((void*)wpn, 0.0f);
   *trigger = saved;
}

// ---------------------------------------------------------------------------
// Detours hook
// ---------------------------------------------------------------------------
static bool __fastcall hooked_ShieldUpdate(void* ecx, void* /*edx*/, float dt)
{
   uintptr_t wpn     = (uintptr_t)ecx;
   uintptr_t owner   = *(uintptr_t*)(wpn + kWeapon_mOwner);
   void**    trigger = (void**)(wpn + kWeapon_mTrigger);

   if (!*trigger || !owner)
      return original_ShieldUpdate(ecx, dt);

   // Riding something: hold the shield down for as long as it lasts.  Clearing
   // the bit routes the call into the OFF path; the writes are idempotent, so
   // repeating it every frame costs nothing and cannot replay the off sound.
   bool riding = owner_is_riding(owner);
   if (riding) {
      uint32_t* flags = shield_state_flags(owner);
      if (flags) *flags &= ~kShieldUpBit;
   }

   if (!riding && is_active_for_channel(ecx))
      return original_ShieldUpdate(ecx, dt);

   // Deselected, or riding: run the real update with the fire trigger masked
   // out.  Only the toggle test sees the difference; drain, expiry, upkeep and
   // the OFF path all still run.
   void* saved  = *trigger;
   *trigger     = &s_nullTrigger;
   bool  result = original_ShieldUpdate(ecx, dt);
   *trigger     = saved;
   return result;
}

// ---------------------------------------------------------------------------
// EntitySoldier::EnterControllable -- drop the shield as the soldier boards.
//
// `this` is the EntitySoldier struct base; the Controllable sub-object (which is
// what SoldierLayout offsets are measured from, and what Weapon::mOwner points
// at) sits at +0x240.
// ---------------------------------------------------------------------------
static bool __fastcall hooked_EnterControllable(void* ecx, void* /*edx*/, void* target)
{
   bool entered = original_EnterControllable(ecx, target);
   if (!entered || !s_shieldUpdateAddr) return entered;

   uintptr_t entity = (uintptr_t)ecx + 0x240;

   for (int i = 0; i < 8; ++i) {
      uintptr_t w = *(uintptr_t*)(entity + g_soldier->weaponArray + i * 4);
      if (!w) continue;

      // Identify a WeaponShield by its Update slot rather than by RTTI.
      void** vtbl = *(void***)w;
      if (!vtbl || vtbl[1] != (void*)s_shieldUpdateAddr) continue;

      if (*(uintptr_t*)(w + kWeapon_mOwner) == entity)
         drive_shield_off(w);
   }
   return entered;
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void shield_channel_fix_install(uintptr_t exe_base)
{
   if (!g_addr->weapon_shield_update)
      return;

   s_shieldUpdateAddr    = (uintptr_t)resolve(exe_base, g_addr->weapon_shield_update);
   original_ShieldUpdate = (fn_ShieldUpdate_t)s_shieldUpdateAddr;

   if (g_addr->soldier_enter_controllable)
      original_EnterControllable =
         (fn_EnterControllable_t)resolve(exe_base, g_addr->soldier_enter_controllable);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_ShieldUpdate, hooked_ShieldUpdate);
   if (original_EnterControllable)
      DetourAttach(&(PVOID&)original_EnterControllable, hooked_EnterControllable);
   DetourTransactionCommit();
}

void shield_channel_fix_uninstall()
{
   if (!original_ShieldUpdate) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_ShieldUpdate, hooked_ShieldUpdate);
   if (original_EnterControllable)
      DetourDetach(&(PVOID&)original_EnterControllable, hooked_EnterControllable);
   DetourTransactionCommit();

   original_EnterControllable = nullptr;
   s_shieldUpdateAddr         = 0;
}
