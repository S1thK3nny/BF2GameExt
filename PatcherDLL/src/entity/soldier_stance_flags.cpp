#include "pch.h"
#include "soldier_stance_flags.hpp"
#include "core/entity_layout.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstdlib>
#include <unordered_map>
#include <detours.h>

// =============================================================================
// DisableProne / DisableCrouch - per-class ODF properties for EntitySoldier.
//
// Storage: a side table keyed by EntitySoldierClass*. The class flag word
// (+0x8BC modtools, +0x6C8 Steam/GOG) has 16 PDB-reserved bits (z_uiReserved0,
// bits 16-31), but they are no use to us: the class copy constructor copies that
// word field by field with masks covering bits 0-15 only, so reserved bits are
// never inherited and a derived class starts with whatever the pool block held.
//
// Inheritance: every ODF soldier class is built by EntitySoldierClass::Derive,
// which runs the copy constructor (this, parent, hash) and then applies the
// child's own ODF lines. Hooking the constructor gives the child its parent's
// flags before those lines run, so `DisableProne = 0` in a child still turns it
// back off. The constructor also clears any entry left at a reused class
// address, which is why no per-mission reset is needed.
//
// IsAcklay (bit 5 of the flag word) implies DisableProne. It is read live from
// the class rather than captured in SetProperty, so it follows the engine's own
// inheritance of that bit.
// =============================================================================

namespace {

constexpr uint8_t kFlagNoProne  = 0x01;
constexpr uint8_t kFlagNoCrouch = 0x02;

// EntitySoldierClass::m_bIsAcklay, bit 5 of the class flag word.
constexpr uint32_t kIsAcklayBit = 0x20;

using fn_hash_string_t = uint32_t(__cdecl*)(const char*);

// EntitySoldierClass::SetProperty - __thiscall(this, uint hash, const char* value)
using fn_SetProperty_t = void(__fastcall*)(void* ecx, void* edx,
                                           unsigned int hash, const char* value);

// EntitySoldierClass copy ctor - __thiscall(this, parent, uint hash) -> this, RET 8
using fn_CopyCtor_t = void*(__fastcall*)(void* ecx, void* edx,
                                         void* parent, unsigned int hash);

fn_hash_string_t  fn_hash_string       = nullptr;
fn_SetProperty_t  original_SetProperty = nullptr;
fn_CopyCtor_t     original_CopyCtor    = nullptr;

// Computed lazily in the SetProperty hook: game code cannot run during the
// install window (exe sections are RW/no-exec there).
uint32_t s_hashDisableProne  = 0;
uint32_t s_hashDisableCrouch = 0;

// Only classes with at least one flag set have an entry.
std::unordered_map<const void*, uint8_t> s_flags;

uint8_t get_flags(const void* cls)
{
   if (s_flags.empty()) return 0;
   auto it = s_flags.find(cls);
   return it == s_flags.end() ? 0 : it->second;
}

void set_flag(const void* cls, uint8_t flag, bool on)
{
   uint8_t f = get_flags(cls);
   f = on ? (uint8_t)(f | flag) : (uint8_t)(f & ~flag);
   if (f) s_flags[cls] = f;
   else   s_flags.erase(cls);
}

void __fastcall hooked_SetProperty(void* ecx, void* /*edx*/,
                                   unsigned int hash, const char* value)
{
   if (s_hashDisableProne == 0 && fn_hash_string) {
      s_hashDisableProne  = fn_hash_string("DisableProne");
      s_hashDisableCrouch = fn_hash_string("DisableCrouch");
   }

   // Consumed: the stock parser has no case for either hash. atoi matches the
   // engine's own int parsing, so a non-numeric value reads as 0.
   if (s_hashDisableProne != 0 && hash == s_hashDisableProne) {
      if (ecx && value) set_flag(ecx, kFlagNoProne, std::atoi(value) != 0);
      return;
   }
   if (s_hashDisableCrouch != 0 && hash == s_hashDisableCrouch) {
      if (ecx && value) set_flag(ecx, kFlagNoCrouch, std::atoi(value) != 0);
      return;
   }

   original_SetProperty(ecx, nullptr, hash, value);
}

void* __fastcall hooked_CopyCtor(void* ecx, void* /*edx*/, void* parent, unsigned int hash)
{
   void* self = original_CopyCtor(ecx, nullptr, parent, hash);

   if (ecx) {
      const uint8_t inherited = parent ? get_flags(parent) : 0;
      if (inherited) s_flags[ecx] = inherited;
      else if (!s_flags.empty()) s_flags.erase(ecx);
   }
   return self;
}

} // namespace

bool soldier_class_prone_disabled(const void* cls)
{
   if (!cls) return false;
   if (get_flags(cls) & kFlagNoProne) return true;
   return (*(const uint32_t*)((const char*)cls + g_soldier->clsFlags) & kIsAcklayBit) != 0;
}

bool soldier_class_crouch_disabled(const void* cls)
{
   if (!cls) return false;
   return (get_flags(cls) & kFlagNoCrouch) != 0;
}

void soldier_stance_flags_install(uintptr_t exe_base)
{
   if (g_build == GameBuild::Unknown) return;
   if (!g_addr->hash_string || !g_addr->soldier_class_set_property ||
       !g_addr->soldier_class_copy_ctor)
      return;

   fn_hash_string       = (fn_hash_string_t)resolve(exe_base, g_addr->hash_string);
   original_SetProperty = (fn_SetProperty_t)resolve(exe_base, g_addr->soldier_class_set_property);
   original_CopyCtor    = (fn_CopyCtor_t)   resolve(exe_base, g_addr->soldier_class_copy_ctor);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   DetourAttach(&(PVOID&)original_CopyCtor,    hooked_CopyCtor);
   DetourTransactionCommit();
}

void soldier_stance_flags_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_SetProperty) DetourDetach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   if (original_CopyCtor)    DetourDetach(&(PVOID&)original_CopyCtor,    hooked_CopyCtor);
   DetourTransactionCommit();
   s_flags.clear();
}
