#include "pch.h"
#include "dual_cannon.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>

// =============================================================================
// ClassLabel "dualcannon"
//
// A weapon ODF's ClassLabel resolves by a linear scan of Factory<Weapon>::sList for
// a matching PblHash, so registering one more base class object is all it takes for
// the label to load. GameState::CreateBaseWeaponClasses builds the stock list on every
// mission load (PostStateCleanup unlinks it and resets sCounter), so we hook it and
// append ours after the original returns.
//
// The object is a real WeaponCannonClass, built by the engine's own constructor, and
// then re-pointed at a DLL-owned copy of the cannon class vtable. Every per-ODF class
// the loader derives from it goes through our Derive, and every weapon it builds
// through our Build, so both carry our vtables from then on while inheriting all
// cannon behaviour.
//
// Derive and Build CHAIN to whatever the live cannon vtable slot held when we copied
// it, rather than allocating themselves. held_ordnance_effect hooks Derive (and
// SetProperty and the destructor) to keep side tables in step with class inheritance,
// and allocating here would silently skip that. Extra per-class and per-weapon state
// therefore lives in side tables too, never in a larger object.
//
// Multiplayer: the base constructor post-increments Factory::sCounter into
// Factory::mNetIndex (+0x1C), which WriteNetEvent sends in 8 bits for CREATE_ORDNANCE
// and ReadWeaponClass resolves back. One extra registration would shift the index of
// every ODF class after it, so an unmodded peer would attribute kills to the wrong
// weapon. The base class object itself is never a fire weapon, so it takes 0xFF (the
// "no class" value, which lookups reject before scanning) and gives its counter slot
// back. Maps that do not use dualcannon then index exactly as they would without us.
// =============================================================================

namespace {

// PblHash("dualcannon"), from ToolsFL\bin\Hash.exe.
constexpr uint32_t kDualCannonHash = 0x14064EC2;

constexpr unsigned kCannonClassSize   = 0x3DC;
constexpr unsigned kClassVtableSlots  = 13;
constexpr unsigned kWeaponVtableSlots = 61;
constexpr unsigned kFactoryNetIndex   = 0x1C;
constexpr uint32_t kNoNetIndex        = 0xFF;

// Class vtable slots.
constexpr unsigned kSlotDerive = 1;
constexpr unsigned kSlotBuild  = 2;

using fn_create_base_classes_t = void(__cdecl*)();
using fn_operator_new_t        = void*(__cdecl*)(unsigned size);
using fn_class_ctor_t          = void*(__thiscall*)(void* self, uint32_t hash);
using fn_derive_t              = void*(__thiscall*)(void* self, uint32_t hash);
using fn_build_t               = void*(__thiscall*)(void* self, void* desc);

fn_create_base_classes_t original_create_base_classes = nullptr;
fn_operator_new_t        s_operatorNew  = nullptr;
fn_class_ctor_t          s_classCtor    = nullptr;
void**                   s_cannonClassVtable  = nullptr;
void**                   s_cannonWeaponVtable = nullptr;
uint32_t*                s_factoryCounter     = nullptr;

// DLL-owned vtables, filled from the live cannon tables on first registration. That is
// long after every install-time vtable patch (barrel_fire_origin, held_ordnance_effect)
// has landed, so the copies inherit those hooks.
void* s_classVtable[kClassVtableSlots]   = {};
void* s_weaponVtable[kWeaponVtableSlots] = {};
bool  s_vtablesReady = false;

// The cannon implementations our overrides chain to.
fn_derive_t s_baseDerive = nullptr;
fn_build_t  s_baseBuild  = nullptr;

void install_log(const char* fmt, ...)
{
   // CRT only: install runs while every section is mapped PAGE_READWRITE, so
   // calling back into the engine's logger would EXEC-fault.
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

void* __fastcall dual_derive(void* self, void* /*edx*/, uint32_t hash)
{
   void* derived = s_baseDerive(self, hash);
   if (derived) *static_cast<void***>(derived) = s_classVtable;
   return derived;
}

void* __fastcall dual_build(void* self, void* /*edx*/, void* desc)
{
   void* weapon = s_baseBuild(self, desc);
   if (weapon) *static_cast<void***>(weapon) = s_weaponVtable;
   return weapon;
}

void build_vtables()
{
   std::memcpy(s_classVtable, s_cannonClassVtable, sizeof(s_classVtable));
   std::memcpy(s_weaponVtable, s_cannonWeaponVtable, sizeof(s_weaponVtable));

   s_baseDerive = reinterpret_cast<fn_derive_t>(s_classVtable[kSlotDerive]);
   s_baseBuild  = reinterpret_cast<fn_build_t>(s_classVtable[kSlotBuild]);
   s_classVtable[kSlotDerive] = reinterpret_cast<void*>(&dual_derive);
   s_classVtable[kSlotBuild]  = reinterpret_cast<void*>(&dual_build);

   s_vtablesReady = true;
}

void __cdecl hooked_create_base_classes()
{
   original_create_base_classes();

   if (!s_vtablesReady) build_vtables();

   void* cls = s_operatorNew(kCannonClassSize);
   if (!cls) {
      get_gamelog()("[DualCannon] out of memory registering ClassLabel \"dualcannon\"\n");
      return;
   }

   s_classCtor(cls, kDualCannonHash);
   *static_cast<void***>(cls) = s_classVtable;

   // Give the counter slot back; see the header comment.
   *static_cast<uint32_t*>(static_cast<void*>(static_cast<char*>(cls) + kFactoryNetIndex)) =
      kNoNetIndex;
   --*s_factoryCounter;

   get_gamelog()("[DualCannon] ClassLabel \"dualcannon\" registered, net index %u, sCounter %u\n",
                 kNoNetIndex, *s_factoryCounter);
}

} // namespace

void dual_cannon_install(uintptr_t exe_base)
{
   HOOK_REQUIRE_MODTOOLS();

   if (!g_addr->game_state_create_base_weapon_classes || !g_addr->engine_operator_new ||
       !g_addr->weapon_cannon_class_ctor || !g_addr->weapon_cannon_class_vftable ||
       !g_addr->weapon_cannon_vftable || !g_addr->weapon_class_factory_counter) {
      install_log("[DualCannon] NOT installed: addresses unknown for this build");
      return;
   }

   // Prologue of CreateBaseWeaponClasses: PUSH ECX / PUSH ESI / PUSH 0x3DC. A mismatch
   // means the address is wrong for this exe, and detouring it would be a guess.
   static constexpr uint8_t kPrologue[] = {0x51, 0x56, 0x68, 0xDC, 0x03, 0x00, 0x00};
   void* target = resolve(exe_base, g_addr->game_state_create_base_weapon_classes);
   if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
      install_log("[DualCannon] NOT installed: unexpected bytes at CreateBaseWeaponClasses %08X",
                  (unsigned)g_addr->game_state_create_base_weapon_classes);
      return;
   }

   s_operatorNew = reinterpret_cast<fn_operator_new_t>(resolve(exe_base, g_addr->engine_operator_new));
   s_classCtor   = reinterpret_cast<fn_class_ctor_t>(resolve(exe_base, g_addr->weapon_cannon_class_ctor));
   s_cannonClassVtable  = static_cast<void**>(resolve(exe_base, g_addr->weapon_cannon_class_vftable));
   s_cannonWeaponVtable = static_cast<void**>(resolve(exe_base, g_addr->weapon_cannon_vftable));
   s_factoryCounter =
      static_cast<uint32_t*>(resolve(exe_base, g_addr->weapon_class_factory_counter));

   original_create_base_classes = reinterpret_cast<fn_create_base_classes_t>(target);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_create_base_classes, hooked_create_base_classes);
   if (DetourTransactionCommit() != NO_ERROR) {
      original_create_base_classes = nullptr;
      install_log("[DualCannon] NOT installed: detour of CreateBaseWeaponClasses failed");
      return;
   }

   install_log("[DualCannon] installed: ClassLabel \"dualcannon\" registers on mission load");
}

void dual_cannon_uninstall()
{
   if (!original_create_base_classes) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_create_base_classes, hooked_create_base_classes);
   DetourTransactionCommit();
   original_create_base_classes = nullptr;
}
