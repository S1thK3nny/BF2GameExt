#include "pch.h"

#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "core/x86_emit.hpp"
#include "held_ordnance_effect.hpp"
#include "held_ordnance_effect_sites.hpp"
#include "util/install_log.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

// =============================================================================
// Held ordnance effects
//
// WeaponCannon ODF: HeldOrdnanceEffectBone = "bone_name"
// Shows the projectile's TrailEffect at an animated soldier bone before release.
// Classic Collection passes the held effect's generation handle in OrdnanceDesc.
// PC has no such field, but its constructor already owns attachment and lifetime.
// Replace only the effect-create call during this weapon's CreateOrdnance, then
// let the rest of the native constructor transfer it to the projectile.
//
// Configuration follows class inheritance. The side tables clear on destruction
// and level reset; generation checks reject recycled effects and soldier owners.
// INI: [Fixes] HeldOrdnanceEffect. See docs/user/ODF_PROPERTIES.md.
// =============================================================================

bool g_heldOrdnanceEffectEnabled = true;

namespace held_ordnance_effect {

// RTTI uses PblHash (lowercase FNV-1a); animation joint keys use RedHash CRC.
constexpr uint32_t kSoldierRtti = 0x5E8739F4;
constexpr unsigned kEffectGeneration = 0x1C;
constexpr unsigned kObjectGeneration = 0x204;

template<class T>
T& at(void* p, unsigned offset)
{
   return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + offset);
}

template<class Fn>
Fn method(void* p, unsigned slot)
{
   return reinterpret_cast<Fn>((*static_cast<void***>(p))[slot]);
}

using void_method = void(__thiscall*)(void*);
using bool_method = bool(__thiscall*)(void*);
using create_method = void*(__thiscall*)(void*);
using create_ordnance_method = void*(__thiscall*)(void*, void*);
using deselect_method = void(__thiscall*)(void*, uint32_t, bool);
using idle_method = bool(__thiscall*)(void*, float);
using destroy_method = void*(__thiscall*)(void*, unsigned);
using pose_method = void(__thiscall*)(void*, void*);
using attach_method = void(__thiscall*)(void*, const void*);
using rtti_method = bool(__thiscall*)(void*, uint32_t);
using property_method = void(__thiscall*)(void*, uint32_t, const char*);
using derive_method = void*(__thiscall*)(void*, uint32_t);

constexpr uint32_t kBoneProperty = pbl_hash("HeldOrdnanceEffectBone");

// Same case-sensitive CRC-32/BZIP2 as PblTEMPHash and tentacle bone names.
uint32_t bone_hash(const char* text)
{
   uint32_t hash = 0xFFFFFFFF;
   for (; *text; ++text) {
      hash ^= static_cast<uint32_t>(static_cast<uint8_t>(*text)) << 24;
      for (unsigned bit = 0; bit != 8; ++bit)
         hash = (hash << 1) ^ ((hash & 0x80000000u) ? 0x04C11DB7u : 0);
   }
   return hash ^ 0xFFFFFFFF;
}

struct effect_handle {
   void* pointer = nullptr;
   uint32_t generation = 0;

   void* get() const
   {
      return pointer && at<uint32_t>(pointer, kEffectGeneration) == generation ? pointer : nullptr;
   }
};

struct held_entry {
   effect_handle effect;
   void* owner = nullptr;
   uint32_t ownerGeneration = 0;
   void* effectClass = nullptr;
   unsigned attempts = 0;
};

// Entries exist only between EnterFire and transfer/cancellation. Destructor
// cleanup prevents weapon-address reuse; the level-reset callback discards stale
// handles without touching an engine pool that has already been destroyed.
std::map<void*, held_entry> g_held;
// Native Derive copies class fields; mirror that inheritance for our side table.
// Class destruction and level reset remove keys before their addresses are reused.
std::map<void*, uint32_t> g_bones;
std::recursive_mutex g_mutex;
uint64_t g_epoch = 0;
bool g_installed = false;
bool g_shuttingDown = false;
const sites::build_sites* g_sites = nullptr;
pose_method g_setupPose = nullptr;
attach_method g_attach = nullptr;
const void* g_identity = nullptr;
void* g_original[7] = {};
uint8_t g_originalCreate[7] = {};
uint8_t g_originalFire[5] = {};
void* g_createSite = nullptr;
void* g_fireSite = nullptr;
void** g_cannonTable = nullptr;
void** g_classTable = nullptr;
void* g_classOriginal[3] = {};

struct red_pose {
   uint32_t count = 0;
   uint32_t keys[128] = {};
   void* matrices[128] = {};
};
static_assert(sizeof(red_pose) == 0x404, "PC RedPose layout");

void* find_bone(const red_pose& pose, uint32_t hash)
{
   unsigned slot = hash & 127;
   for (unsigned n = 0; n != 128; ++n, slot = (slot - 1) & 127) {
      if (pose.keys[slot] == hash) return pose.matrices[slot];
      if (!pose.keys[slot]) break;
   }
   return nullptr;
}

uint32_t configured_bone(void* weapon)
{
   if (!weapon || g_bones.empty()) return 0;
   void* cls = at<void*>(weapon, 0x64);
   auto it = g_bones.find(cls);
   return it != g_bones.end() ? it->second : 0;
}

void __fastcall set_property(void* cls, void*, uint32_t hash, const char* value)
{
   if (hash != kBoneProperty) {
      reinterpret_cast<property_method>(g_classOriginal[0])(cls, hash, value);
      return;
   }
   std::lock_guard<std::recursive_mutex> lock(g_mutex);
   if (!cls || !value || g_shuttingDown) return;
   if (*value)
      g_bones[cls] = bone_hash(value);
   else
      g_bones.erase(cls); // A child can explicitly disable the inherited value.
}

void* __fastcall derive_class(void* cls, void*, uint32_t name)
{
   std::lock_guard<std::recursive_mutex> lock(g_mutex);
   const uint64_t epoch = g_epoch;
   auto it = g_bones.find(cls);
   const uint32_t bone = it != g_bones.end() ? it->second : 0;
   void* derived = reinterpret_cast<derive_method>(g_classOriginal[1])(cls, name);
   if (derived && g_epoch == epoch && !g_shuttingDown) {
      if (bone)
         g_bones[derived] = bone;
      else
         g_bones.erase(derived);
   }
   return derived;
}

void* __fastcall destroy_class(void* cls, void*, unsigned flags)
{
   std::lock_guard<std::recursive_mutex> lock(g_mutex);
   g_bones.erase(cls);
   return reinterpret_cast<destroy_method>(g_classOriginal[2])(cls, flags);
}

// Virtual order differs from CC: PC IsActive/Activate/Deactivate are 5/6/7;
// CC uses 6/7/8. In particular +0x20 on PC means StopAndFinish, not Deactivate.
void release_effect(effect_handle handle)
{
   const uint64_t epoch = g_epoch;
   void* effect = handle.get();
   if (!effect) return;
   method<void_method>(effect, 7)(effect);
   if (g_epoch != epoch) return;
   if (!handle.get()) return;
   // Deactivation has removed the display/thread. Keep the last world matrix,
   // but never retain an abandoned pointer into a soldier's animation storage.
   at<uint32_t>(effect, 0x20) &= ~7u;
   at<void*>(effect, 0x24) = nullptr;
   at<uint32_t>(effect, 0x28) = 0;
   at<void*>(effect, 0x2C) = nullptr;
}

void cancel_held(void* weapon)
{
   auto it = g_held.find(weapon);
   if (it == g_held.end()) return;
   effect_handle handle = it->second.effect;
   g_held.erase(it); // Native callbacks may reenter weapon code.
   release_effect(handle);
}

struct beginning_scope;
thread_local beginning_scope* g_beginning = nullptr;
struct beginning_scope {
   beginning_scope* previous = g_beginning;
   void* weapon;
   uint64_t epoch = g_epoch;
   explicit beginning_scope(void* w) : weapon(w)
   {
      g_beginning = this;
   }
   ~beginning_scope()
   {
      g_beginning = previous;
   }
};

void begin_held(void* weapon)
{
   if (g_shuttingDown) return;
   const uint32_t bone = configured_bone(weapon);
   if (!bone) return;
   for (auto* scope = g_beginning; scope; scope = scope->previous)
      if (scope->weapon == weapon && scope->epoch == g_epoch) return;
   beginning_scope beginning(weapon);
   const uint64_t epoch = g_epoch;
   auto previous = g_held.find(weapon);
   if (previous != g_held.end()) {
      if (previous->second.effect.get()) return;
      g_held.erase(previous);
   }

   void* owner = at<void*>(weapon, 0x6C);
   if (!owner) return;
   void* component = static_cast<uint8_t*>(owner) + 0x18;
   void* soldier = method<create_method>(component, 8)(component);
   if (g_epoch != epoch || !soldier) return;
   const bool isSoldier = method<rtti_method>(soldier, 0)(soldier, kSoldierRtti);
   if (g_epoch != epoch || !isSoldier) return;
   const uint32_t ownerGeneration = at<uint32_t>(soldier, kObjectGeneration);
   if (!ownerGeneration) return;
   // The query returns the full EntitySoldier: animator +0x760 / retail +0x750.
   // Render-subobject offsets +0x6CC / +0x6BC belong to a different receiver.
   void* animator = at<void*>(soldier, g_sites->animatorOffset);
   if (!animator) return;

   void* cls = at<void*>(weapon, 0x64);
   void* ordnanceClass = at<void*>(cls, 0x78);
   if (!ordnanceClass) return;
   void* effectClass = at<void*>(ordnanceClass, g_sites->trailOffset);
   if (!effectClass) return;

   red_pose pose;
   g_setupPose(animator, &pose);
   if (g_epoch != epoch || at<uint32_t>(soldier, kObjectGeneration) != ownerGeneration) return;
   void* matrix = find_bone(pose, bone);
   if (!matrix) return;
   // SetupPose exports pointers into the animator's persistent ZephyrPose.
   // The temporary RedPose is only a lookup table; none of its storage escapes.
   void* effect = method<create_method>(effectClass, 1)(effectClass);
   if (g_epoch != epoch || !effect) return;
   effect_handle handle{effect, at<uint32_t>(effect, kEffectGeneration)};
   if (at<uint32_t>(soldier, kObjectGeneration) != ownerGeneration) {
      release_effect(handle);
      return;
   }
   at<uint32_t>(effect, 0x20) = (at<uint32_t>(effect, 0x20) & ~7u) | 3u;
   at<void*>(effect, 0x24) = soldier;
   at<uint32_t>(effect, 0x28) = ownerGeneration;
   at<void*>(effect, 0x2C) = matrix;
   g_attach(effect, g_identity);
   if (g_epoch != epoch) return;
   if (!handle.get()) return;
   method<void_method>(effect, 6)(effect);
   if (g_epoch != epoch) return;
   if (!handle.get()) return;
   const bool active = method<bool_method>(effect, 5)(effect);
   if (g_epoch != epoch) return;
   if (!handle.get()) return;
   if (!active || at<uint32_t>(soldier, kObjectGeneration) != ownerGeneration) {
      release_effect(handle);
      return;
   }
   g_held[weapon] = {handle, soldier, ownerGeneration, effectClass, 0};
}

struct adoption_scope;
thread_local adoption_scope* g_adoption = nullptr;

struct adoption_scope {
   adoption_scope* previous = g_adoption;
   uint64_t epoch = g_epoch;
   effect_handle effect;
   void* effectClass = nullptr;
   void* ordnanceClass = nullptr;
   void* descriptor = nullptr;
   bool consumed = false;

   adoption_scope()
   {
      g_adoption = this;
   }
   ~adoption_scope()
   {
      g_adoption = previous;
   }
   adoption_scope(const adoption_scope&) = delete;
   adoption_scope& operator=(const adoption_scope&) = delete;
};

void* __cdecl create_trail(void* effectClass, void* ordnanceClass, void* descriptor)
{
   adoption_scope* scope = g_adoption;
   if (scope && scope->epoch == g_epoch && !scope->consumed && scope->effectClass == effectClass &&
       scope->ordnanceClass == ordnanceClass && scope->descriptor == descriptor) {
      if (void* effect = scope->effect.get()) {
         scope->consumed = true;
         return effect;
      }
   }
   return method<create_method>(effectClass, 1)(effectClass);
}

__declspec(naked) void* create_trail_debug()
{
   __asm {
      mov ecx, [esp + 18h] // Original Ordnance constructor's class argument.
      push ebp // Descriptor stays in EBP through the constructor.
      push ecx
      push edx // Original MOV ECX,EDX selected the effect class.
      call create_trail
      add esp, 12
      ret
   }
}

__declspec(naked) void* create_trail_retail()
{
   __asm {
      push edi // Descriptor and original class in EDI/EBX.
      push ebx
      push ecx
      call create_trail
      add esp, 12
      ret
   }
}

void __fastcall enter_fire(void* weapon, void*)
{
   std::lock_guard<std::recursive_mutex> lock(g_mutex);
   const uint64_t epoch = g_epoch;
   begin_held(weapon);
   if (g_epoch != epoch) return;
   reinterpret_cast<void_method>(g_original[0])(weapon);
}

void __fastcall exit_fire(void* weapon, void*)
{
   std::lock_guard<std::recursive_mutex> lock(g_mutex);
   const uint64_t epoch = g_epoch;
   cancel_held(weapon);
   if (g_epoch != epoch) return;
   reinterpret_cast<void_method>(g_original[1])(weapon);
}

bool __fastcall fire(void* weapon, void*)
{
   std::lock_guard<std::recursive_mutex> lock(g_mutex);
   auto it = g_held.find(weapon);
   const unsigned attempts = it == g_held.end() ? 0 : it->second.attempts;
   const effect_handle held = it == g_held.end() ? effect_handle{} : it->second.effect;
   const uint64_t epoch = g_epoch;
   const bool result = reinterpret_cast<bool_method>(g_original[2])(weapon);
   it = g_held.find(weapon);
   // Network-only fire paths do not enter CreateOrdnance. CC releases their
   // held effect immediately; failed native construction retains it until exit.
   if (g_epoch == epoch && it != g_held.end() && it->second.attempts == attempts &&
       it->second.effect.pointer == held.pointer && it->second.effect.generation == held.generation)
      cancel_held(weapon);
   return result;
}

void* __fastcall create_ordnance(void* weapon, void*, void* descriptor)
{
   std::lock_guard<std::recursive_mutex> lock(g_mutex);
   adoption_scope scope;
   auto it = g_held.find(weapon);
   if (it != g_held.end()) {
      held_entry& held = it->second;
      ++held.attempts;
      if (held.owner && at<uint32_t>(held.owner, kObjectGeneration) == held.ownerGeneration) {
         scope.effect = held.effect;
         scope.effectClass = held.effectClass;
         scope.ordnanceClass = at<void*>(at<void*>(weapon, 0x64), 0x78);
         scope.descriptor = descriptor;
      }
   }
   const uint64_t epoch = g_epoch;
   void* ordnance = reinterpret_cast<create_ordnance_method>(g_original[3])(weapon, descriptor);
   if (scope.consumed && g_epoch == epoch) {
      // No deactivation: native Ordnance now owns this exact effect and has
      // replaced its soldier attachment with the projectile position/direction.
      it = g_held.find(weapon);
      if (it != g_held.end() && it->second.effect.pointer == scope.effect.pointer &&
          it->second.effect.generation == scope.effect.generation)
         g_held.erase(it);
   }
   return ordnance;
}

bool __fastcall update_idle(void* weapon, void*, float dt)
{
   std::lock_guard<std::recursive_mutex> lock(g_mutex);
   const uint64_t epoch = g_epoch;
   cancel_held(weapon);
   if (g_epoch != epoch) return false;
   return reinterpret_cast<idle_method>(g_original[4])(weapon, dt);
}

void __fastcall deselect(void* weapon, void*, uint32_t reason, bool flag)
{
   std::lock_guard<std::recursive_mutex> lock(g_mutex);
   const uint64_t epoch = g_epoch;
   cancel_held(weapon);
   if (g_epoch != epoch) return;
   reinterpret_cast<deselect_method>(g_original[5])(weapon, reason, flag);
}

void* __fastcall destroy(void* weapon, void*, unsigned flags)
{
   std::lock_guard<std::recursive_mutex> lock(g_mutex);
   const uint64_t epoch = g_epoch;
   cancel_held(weapon);
   if (g_epoch != epoch) return weapon;
   return reinterpret_cast<destroy_method>(g_original[6])(weapon, flags);
}

void* const kHooks[] = {reinterpret_cast<void*>(enter_fire),
                        reinterpret_cast<void*>(exit_fire),
                        reinterpret_cast<void*>(fire),
                        reinterpret_cast<void*>(create_ordnance),
                        reinterpret_cast<void*>(update_idle),
                        reinterpret_cast<void*>(deselect),
                        reinterpret_cast<void*>(destroy)};

void* const kClassHooks[] = {reinterpret_cast<void*>(set_property), reinterpret_cast<void*>(derive_class),
                             reinterpret_cast<void*>(destroy_class)};

void log_install(const char* message)
{
   install_log("[HeldOrdnanceEffect] %s", message);
}

bool write_patch(bool install)
{
   struct region {
      void* address;
      size_t length;
      DWORD previous;
   };
   region regions[] = {{g_cannonTable, 61 * sizeof(void*), 0},
                       {g_createSite, g_sites->createLength, 0},
                       {g_fireSite, 5, 0},
                       {g_classTable, 7 * sizeof(void*), 0}};
   unsigned opened = 0;
   for (; opened != 4; ++opened) {
      region& r = regions[opened];
      if (!VirtualProtect(r.address, r.length, PAGE_EXECUTE_READWRITE, &r.previous)) break;
   }
   if (opened != 4) {
      while (opened) {
         region& r = regions[--opened];
         DWORD ignored;
         VirtualProtect(r.address, r.length, r.previous, &ignored);
      }
      return false;
   }

   uint8_t create[7], fireCall[5];
   if (install) {
      void* createHook = g_sites->createLength == 7 ? reinterpret_cast<void*>(create_trail_debug)
                                                    : reinterpret_cast<void*>(create_trail_retail);
      x86::encode_branch(create, g_createSite, x86::kCall, createHook, g_sites->createLength);
      x86::encode_branch(fireCall, g_fireSite, x86::kCall, &fire);
   }
   std::memcpy(g_createSite, install ? create : g_originalCreate, g_sites->createLength);
   std::memcpy(g_fireSite, install ? fireCall : g_originalFire, 5);
   for (unsigned i = 0; i != 7; ++i)
      if (i != 2) g_cannonTable[sites::kSlots[i]] = install ? kHooks[i] : g_original[i];
   for (unsigned i = 0; i != 3; ++i)
      g_classTable[sites::kClassSlots[i]] = install ? kClassHooks[i] : g_classOriginal[i];

   // The two vtables can share a page. Restore in reverse acquisition order so
   // an overlapping region's saved writable protection cannot outlive the patch.
   while (opened) {
      const region& r = regions[--opened];
      DWORD ignored;
      VirtualProtect(r.address, r.length, r.previous, &ignored);
   }
   FlushInstructionCache(GetCurrentProcess(), g_createSite, g_sites->createLength);
   FlushInstructionCache(GetCurrentProcess(), g_fireSite, 5);
   return true;
}

} // namespace held_ordnance_effect

void held_ordnance_effect_install(uintptr_t base)
{
   using namespace held_ordnance_effect;
   if (!g_heldOrdnanceEffectEnabled || g_installed) return;
   const auto* build = sites::get(g_build);
   if (!build || !sites::verify(base, *build)) {
      log_install("NOT installed: native instruction/vtable guard mismatch");
      return;
   }
   g_sites = build;
   g_setupPose = reinterpret_cast<pose_method>(resolve(base, build->setupPose));
   g_attach = reinterpret_cast<attach_method>(resolve(base, build->attach));
   g_identity = resolve(base, build->identity);
   g_createSite = resolve(base, build->createSite);
   g_fireSite = resolve(base, build->fireSite);
   g_cannonTable = static_cast<void**>(resolve(base, build->cannonTable));
   g_classTable = static_cast<void**>(resolve(base, build->classTable));
   for (unsigned i = 0; i != 7; ++i) g_original[i] = resolve(base, build->targets[i]);
   for (unsigned i = 0; i != 3; ++i) g_classOriginal[i] = resolve(base, build->classTargets[i]);
   std::memcpy(g_originalCreate, g_createSite, build->createLength);
   std::memcpy(g_originalFire, g_fireSite, sizeof(g_originalFire));
   if (!write_patch(true)) {
      log_install("NOT installed: unable to make all sites writable; no changes made");
      return;
   }
   g_shuttingDown = false;
   g_installed = true;
   log_install("installed (HeldOrdnanceEffectBone, native continuous handoff)");
}

void held_ordnance_effect_uninstall()
{
   using namespace held_ordnance_effect;
   std::lock_guard<std::recursive_mutex> lock(g_mutex);
   if (!g_installed) return;
   g_shuttingDown = true;
   while (!g_held.empty()) cancel_held(g_held.begin()->first);
   if (!write_patch(false)) {
      g_shuttingDown = false;
      log_install("uninstall failed: active hooks retained");
      return;
   }
   g_installed = false;
   g_bones.clear();
}

void held_ordnance_effect_reset()
{
   std::lock_guard<std::recursive_mutex> lock(held_ordnance_effect::g_mutex);
   held_ordnance_effect::g_held.clear();
   held_ordnance_effect::g_bones.clear();
   ++held_ordnance_effect::g_epoch;
}
