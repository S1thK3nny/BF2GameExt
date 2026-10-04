#include "pch.h"
#include "soldier_anim_tables.hpp"
#include "combo_anim_limit.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "game/Battlefront2/Source/SoldierAnimator.h"
#include "util/install_log.hpp"

#include <cstring>

// See soldier_anim_tables.hpp.

namespace soldier_anim_tables {
namespace {

namespace sac = layout::SoldierAnimatorClass;
namespace sab = layout::SoldierAnimationBank;

static_assert(kMaxMapsAny == kComboMapCount && kMaxMapsAny >= sac::kMaxMaps);
static_assert(kMaxBanksAny == kComboBankCount && kMaxBanksAny >= sab::kMaxBanks);
static_assert(kMaxWeapons == kComboWeaponCount && kMaxWeapons == sab::kMaxWeapons);
static_assert(kNameBuffer == sab::kNameLength + 1);
static_assert(sizeof(combo_anim_bank) == sab::kBankStride && offsetof(combo_anim_bank, parent) == sab::kBankParent);
static_assert(sizeof(combo_map_key) == sab::kMapStride);

// ActionAnimation's count; ComboAnimIncrease keeps 38 of them.
constexpr int kStockActions = 39;

// SoldierAnimatorClass::FindAnimation: thiscall(PblTEMPHash, const char* name),
// RET 8; the ZephyrAnim, or null. Steam and GOG ignore the name.
using FindAnimationFn = void*(__fastcall*)(void* cls, void* edx, uint32_t hash, const char* name);

int             s_state         = 0;   // 0 not tried, 1 ready, -1 unavailable
void* const*    s_classCell     = nullptr;   // SoldierAnimatorClass::sInstance
const uint8_t*  s_banks         = nullptr;
const uint8_t*  s_weapons       = nullptr;
const uint8_t*  s_maps          = nullptr;
FindAnimationFn s_findAnimation = nullptr;

bool guard(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[SoldierAnimTables] unavailable: unexpected code at %s 0x%08X", what, (unsigned)va);
         return false;
      }
   }
   return true;
}

const uint8_t* bank_entry(int bank)
{
   if (bank < 0 || bank >= max_banks()) return nullptr;
   if (expanded()) {
      const combo_anim_bank* banks = combo_anim_limit_bank_registry();
      return banks ? reinterpret_cast<const uint8_t*>(&banks[bank]) : nullptr;
   }
   return s_banks + bank * sab::kBankStride;
}

const uint8_t* weapon_entry(int weapon)
{
   if (weapon < 0 || weapon >= kMaxWeapons) return nullptr;
   return s_weapons + weapon * sab::kWeaponStride;
}

void entry_name(const uint8_t* entry, char (&out)[kNameBuffer])
{
   if (!entry) {
      out[0] = '\0';
      return;
   }
   std::memcpy(out, entry, sab::kNameLength);
   out[sab::kNameLength] = '\0';
}

} // namespace

bool init(uintptr_t base)
{
   if (s_state != 0) return s_state > 0;
   s_state = -1;
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return false;
   const auto* a = g_addr;
   if (!a->soldier_animator_class_instance || !a->soldier_anim_get_upper_action ||
       !a->soldier_anim_get_lower_action || !a->soldier_anim_banks || !a->soldier_anim_weapons ||
       !a->soldier_anim_maps || !a->anim_class_find_in_banks) {
      install_log("[SoldierAnimTables] unavailable: no address set for this build");
      return false;
   }
   // Stock, the getters' code is the action table's layout:
   // [class + (map * 0x97 + action) * 8 + 0x24] upper, + 0x28 lower.
   // ComboAnimIncrease has replaced them with its own, and keeps the tables.
   if (!combo_anim_limit_active() &&
       (!guard(base, a->soldier_anim_get_upper_action, "SoldierAnimatorClass::GetUpperBodyActionAnimation",
               modtools ? "\x8B\x44\x24\x08\x8B\x54\x24\x04\x69\xC0\x97\x00\x00\x00\x03\xC2\x8B\x44\xC1\x24\xC2\x08\x00"
                        : "\x55\x8B\xEC\x69\x45\x0C\x97\x00\x00\x00\x03\x45\x08\x8B\x44\xC1\x24\x5D\xC2\x08\x00",
               modtools ? "xxxxxxxxxxxxxxxxxxxxxxx" : "xxxxxxxxxxxxxxxxxxxxx") ||
        !guard(base, a->soldier_anim_get_lower_action, "SoldierAnimatorClass::GetLowerBodyActionAnimation",
               modtools ? "\x8B\x44\x24\x08\x8B\x54\x24\x04\x69\xC0\x97\x00\x00\x00\x03\xC2\x8B\x44\xC1\x28\xC2\x08\x00"
                        : "\x55\x8B\xEC\x69\x45\x0C\x97\x00\x00\x00\x03\x45\x08\x8B\x44\xC1\x28\x5D\xC2\x08\x00",
               modtools ? "xxxxxxxxxxxxxxxxxxxxxxx" : "xxxxxxxxxxxxxxxxxxxxx")))
      return false;

   s_classCell     = static_cast<void* const*>(resolve(base, a->soldier_animator_class_instance));
   s_banks         = static_cast<const uint8_t*>(resolve(base, a->soldier_anim_banks));
   s_weapons       = static_cast<const uint8_t*>(resolve(base, a->soldier_anim_weapons));
   s_maps          = static_cast<const uint8_t*>(resolve(base, a->soldier_anim_maps));
   s_findAnimation = reinterpret_cast<FindAnimationFn>(resolve(base, a->anim_class_find_in_banks));
   s_state = 1;
   return true;
}

bool expanded()
{
   return combo_anim_limit_active();
}

int max_maps()
{
   return expanded() ? kComboMapCount : sac::kMaxMaps;
}

int max_banks()
{
   return expanded() ? kComboBankCount : sab::kMaxBanks;
}

bool map_key(int map, int& bank, int& weapon)
{
   if (s_state <= 0 || map < 0 || map >= max_maps()) return false;
   if (expanded()) {
      const combo_map_key* maps = combo_anim_limit_map_registry();
      if (!maps) return false;
      bank = maps[map].bank;
      weapon = maps[map].weapon;
      return true;
   }
   const auto* entry = reinterpret_cast<const int32_t*>(s_maps + map * sab::kMapStride);
   bank = entry[0];
   weapon = entry[1];
   return true;
}

void bank_name(int bank, char (&out)[kNameBuffer])
{
   entry_name(s_state > 0 ? bank_entry(bank) : nullptr, out);
}

void weapon_name(int weapon, char (&out)[kNameBuffer])
{
   entry_name(s_state > 0 ? weapon_entry(weapon) : nullptr, out);
}

int bank_parent(int bank)
{
   const uint8_t* entry = s_state > 0 ? bank_entry(bank) : nullptr;
   return entry ? *reinterpret_cast<const int32_t*>(entry + sab::kBankParent) : -1;
}

int weapon_parent(int weapon)
{
   const uint8_t* entry = s_state > 0 ? weapon_entry(weapon) : nullptr;
   return entry ? *reinterpret_cast<const int32_t*>(entry + sab::kWeaponParent) : -1;
}

SoldierAnimation** action_slot(int map, int action, int half)
{
   if (s_state <= 0 || half < 0 || half > 1) return nullptr;
   if (expanded())
      return reinterpret_cast<SoldierAnimation**>(combo_anim_limit_action_slot(map, action, half != 0));
   if (map < 0 || map >= sac::kMaxMaps || action < 0 || action >= kStockActions) return nullptr;
   uint8_t* cls = static_cast<uint8_t*>(*s_classCell);
   if (!cls) return nullptr;
   return reinterpret_cast<SoldierAnimation**>(cls + sac::action_entry(map, action) + half * 4);
}

SoldierAnimation* action_animation(int map, int action, int half)
{
   SoldierAnimation** slot = action_slot(map, action, half);
   if (!slot || reinterpret_cast<uintptr_t>(*slot) == kUnassigned) return nullptr;
   return *slot;
}

void* find_animation(uint32_t hash)
{
   if (s_state <= 0) return nullptr;
   void* cls = *s_classCell;
   return cls ? s_findAnimation(cls, nullptr, hash, nullptr) : nullptr;
}

} // namespace soldier_anim_tables
