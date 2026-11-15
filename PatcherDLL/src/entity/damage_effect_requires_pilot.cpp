#include "pch.h"
#include "damage_effect_requires_pilot.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "entity/odf_gameext_props.hpp"
#include "util/install_log.hpp"

#include <detours.h>

#include <cstdlib>
#include <unordered_map>

// See damage_effect_requires_pilot.hpp for the mechanism.

namespace {

constexpr uint32_t kDamageStartPercent = pbl_hash("DamageStartPercent");
static_assert(kDamageStartPercent == 0x06182385u);
constexpr uint32_t kRequiresPilot = pbl_hash("DamageEffectRequiresPilot");

constexpr int kMaxGroups = 10;   // GameObjectClass::mDamageEffectClass[10]

// Same on all three builds, read off HealthChangeCallback and
// Controllable::SetPilot (modtools 0x005E0890, Steam 0x00483FD0).
constexpr uint32_t kDmgVt_GetGameObject        = 0x10;
constexpr uint32_t kObj_Class                  = 0x08;
constexpr uint32_t kObjVt_GetControllable      = 0x6C;
constexpr uint32_t kClsVt_GetDamageEffectClass = 0x3C;   // thiscall(int), RET 4
constexpr uint32_t kCtl_Pilot                  = 0xD0;
constexpr uint32_t kCtl_PilotType              = 0x144;

constexpr int kPilotNone = 0;
constexpr int kPilotSelf = 1;

// No health ratio is at or under this, so the engine never shows the group.
constexpr float kNeverShown = -1.0f;

// ---------------------------------------------------------------------------
// The ODF property, per class
// ---------------------------------------------------------------------------

struct ClassGroups {
   uint8_t  count   = 0;    // groups so far, inherited ones included
   int8_t   current = -1;   // the group the lines being read belong to
   uint16_t gated   = 0;    // bit i: group i needs a driver
};

std::unordered_map<const void*, ClassGroups> s_classes;
bool s_anyGated = false;

bool on_property(void* cls, uint32_t hash, const char* value)
{
   if (hash == kDamageStartPercent) {
      // SetProperty takes the first free slot, or none past the tenth.
      ClassGroups& c = s_classes[cls];
      c.current = c.count < kMaxGroups ? (int8_t)c.count++ : (int8_t)-1;
      return false;
   }
   if (hash != kRequiresPilot) return false;

   auto it = s_classes.find(cls);
   if (it != s_classes.end() && it->second.current >= 0 && value) {
      const uint16_t bit = (uint16_t)(1u << it->second.current);
      if (std::atoi(value) != 0) {
         it->second.gated |= bit;
         s_anyGated = true;
      } else {
         it->second.gated &= (uint16_t)~bit;
      }
   }
   return true;   // no stock SetProperty knows it
}

void on_derive(const void* parent, void* child)
{
   if (s_classes.empty()) return;
   auto it = parent ? s_classes.find(parent) : s_classes.end();
   if (it == s_classes.end()) {
      s_classes.erase(child);
      return;
   }
   ClassGroups c = it->second;
   c.current = -1;
   s_classes[child] = c;
}

// ---------------------------------------------------------------------------
// Engine access
// ---------------------------------------------------------------------------

void* vcall0(void* obj, uint32_t slot)
{
   typedef void*(__thiscall* Fn)(void*);
   return ((Fn)(*(void***)obj)[slot / sizeof(void*)])(obj);
}

// The groups that need a driver for this Damageable, with its object and class.
uint16_t gated_groups(void* damageable, void** obj, void** cls)
{
   if (!s_anyGated) return 0;
   *obj = vcall0(damageable, kDmgVt_GetGameObject);
   if (!*obj) return 0;
   *cls = *(void**)((char*)*obj + kObj_Class);
   if (!*cls) return 0;
   auto it = s_classes.find(*cls);
   return it == s_classes.end() ? 0 : it->second.gated;
}

bool has_driver(void* obj)
{
   const char* ctrl = (const char*)vcall0(obj, kObjVt_GetControllable);
   if (!ctrl) return true;
   const int type = *(const int*)(ctrl + kCtl_PilotType);
   if (type == kPilotNone || type == kPilotSelf) return true;
   return *(void* const*)(ctrl + kCtl_Pilot) != nullptr;
}

// A group's mPercent, its first member.
float* group_percent(void* cls, int index)
{
   typedef void*(__thiscall* Fn)(void*, int);
   return (float*)((Fn)(*(void***)cls)[kClsVt_GetDamageEffectClass / sizeof(void*)])(cls, index);
}

// ---------------------------------------------------------------------------
// Damageable::HealthChangeCallback
// ---------------------------------------------------------------------------

using HealthChangeFn = void(__fastcall*)(void* self, void* edx, bool flag, float delta);
HealthChangeFn s_healthChange = nullptr;

// One running call's marked groups. The callback fires the health change
// event before it reads the groups, and a listener can damage another vehicle
// of the same class, so calls nest: each puts back what it found, and reads a
// group's real value from the outermost call holding it.
struct Held {
   float* percent;
   float  found;
   float  real;
};
struct Call {
   Held held[kMaxGroups];
   int  count;
};
constexpr int kMaxNesting = 8;
Call s_calls[kMaxNesting];
int  s_depth = 0;

float real_percent(const float* percent, float found)
{
   for (int c = 0; c < s_depth; ++c)
      for (int i = 0; i < s_calls[c].count; ++i)
         if (s_calls[c].held[i].percent == percent) return s_calls[c].held[i].real;
   return found;
}

// Whether each marked vehicle had a driver when its groups were last evaluated.
std::unordered_map<const void*, bool> s_driven;

void __fastcall hooked_HealthChange(void* self, void* edx, bool flag, float delta)
{
   void* obj = nullptr;
   void* cls = nullptr;
   const uint16_t gated = gated_groups(self, &obj, &cls);
   if (!gated || s_depth == kMaxNesting) {
      s_healthChange(self, edx, flag, delta);
      return;
   }

   const bool driven = has_driver(obj);
   s_driven[self] = driven;

   Call& call = s_calls[s_depth];
   call.count = 0;
   for (int i = 0; i < kMaxGroups; ++i) {
      float* percent = group_percent(cls, i);
      if (!percent) break;
      if (!(gated & (1u << i))) continue;
      Held& h = call.held[call.count++];
      h.percent = percent;
      h.found   = *percent;
      h.real    = real_percent(percent, h.found);
      *percent  = driven ? h.real : kNeverShown;
   }

   ++s_depth;
   s_healthChange(self, edx, flag, delta);
   --s_depth;

   for (int i = 0; i < call.count; ++i) *call.held[i].percent = call.held[i].found;
}

// ---------------------------------------------------------------------------
// Damageable::UpdateStatus
// ---------------------------------------------------------------------------

void __cdecl update_status_tick(void* self)
{
   void* obj = nullptr;
   void* cls = nullptr;
   if (!gated_groups(self, &obj, &cls)) return;

   const bool driven = has_driver(obj);
   auto it = s_driven.find(self);
   if (it != s_driven.end() && it->second == driven) return;
   hooked_HealthChange(self, nullptr, false, 0.0f);
}

// Modtools passes the frame time on the stack, Steam and GOG in XMM1; every
// register and the stack go on as they came.
void* s_updateStatus = nullptr;

__declspec(naked) void hooked_UpdateStatus()
{
   __asm {
      pushfd
      pushad
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      push   ecx                          // the Damageable
      call   update_status_tick
      add    esp, 4
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      add    esp, 0x80
      popad
      popfd
      jmp    [s_updateStatus]
   }
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

bool guard(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[DamageEffectRequiresPilot] NOT installed: unexpected code at %s 0x%08X", what,
                     (unsigned)va);
         return false;
      }
   }
   return true;
}

bool s_installed = false;

} // namespace

void damage_effect_requires_pilot_install(uintptr_t base)
{
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   const auto* a = g_addr;
   if (!a->damageable_health_change_callback || !a->damageable_update_status) {
      install_log("[DamageEffectRequiresPilot] NOT installed: no address set for this build");
      return;
   }
   // UpdateStatus on modtools loads a float constant by address at +1.
   if (!guard(base, a->damageable_health_change_callback, "Damageable::HealthChangeCallback",
              modtools ? "\x55\x8B\xEC\x83\xE4\xF0\x83\xEC\x64\x53\x56\x57\x8B\xF9"
                       : "\x55\x8B\xEC\x83\xE4\xF0\x83\xEC\x68\x56\x8B\xF1",
              modtools ? "xxxxxxxxxxxxxx" : "xxxxxxxxxxxx") ||
       !guard(base, a->damageable_update_status, "Damageable::UpdateStatus",
              modtools ? "\x51\xD9\x05\x00\x00\x00\x00\x56\x8B\xF1\xD9\x46\x0C"
                       : "\x55\x8B\xEC\x51\x56\x8B\xF1\x0F\x28\xE1",
              modtools ? "xxx????xxxxxx" : "xxxxxxxxxx"))
      return;

   if (!odf_add_property_handler(on_property) || !odf_add_derive_handler(on_derive)) {
      install_log("[DamageEffectRequiresPilot] NOT installed: no ODF listener slot left");
      return;
   }

   s_healthChange = reinterpret_cast<HealthChangeFn>(resolve(base, a->damageable_health_change_callback));
   s_updateStatus = resolve(base, a->damageable_update_status);
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)s_healthChange, hooked_HealthChange);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_updateStatus, hooked_UpdateStatus);
   if (r != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
      if (r != NO_ERROR) DetourTransactionAbort();
      s_healthChange = nullptr;
      s_updateStatus = nullptr;
      install_log("[DamageEffectRequiresPilot] NOT installed: detouring failed (%ld)", (long)r);
      return;
   }
   s_installed = true;
   install_log("[DamageEffectRequiresPilot] installed (HealthChangeCallback 0x%08X, UpdateStatus 0x%08X)",
               (unsigned)a->damageable_health_change_callback, (unsigned)a->damageable_update_status);
}

void damage_effect_requires_pilot_uninstall()
{
   if (!s_installed) return;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)s_healthChange, hooked_HealthChange);
   DetourDetach(&(PVOID&)s_updateStatus, hooked_UpdateStatus);
   DetourTransactionCommit();
   s_installed = false;
   s_anyGated  = false;
   s_classes.clear();
   s_driven.clear();
}
