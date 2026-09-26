#include "pch.h"
#include "hud_class_icons.hpp"
#include "hud_class_icons_core.hpp"
#include "core/entity_layout.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/layout/character.hpp"
#include "core/layout/droideka.hpp"
#include "core/layout/weapon.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

// =============================================================================
// What it reads, read off each build (names from the Phantom PDB). Details and
// the rest of the research are in docs/RE/HUDSystem.md.
//
//                               modtools @         Steam @            GOG @
//   Trackable::GetGameObject    slot +0x1C         +0x1C              +0x1C
//     (vptr at Controllable+0x18, thiscall(); read at UpdateVehicleHealth's own
//      call on mVehicle, so a seat resolves to the same object as the stock
//      seating mesh)            0x006B48EF         0x00561D8C         0x00562B0C
//   GameObject::GetEntityClass  slot +0x28         +0x28              +0x28
//     (primary vptr, thiscall()) 0x006B4AE3        0x00561EF7         0x00562C77
//   EntityClass::mHealthTexture +0x48              +0x28              +0x28
//     (a PblHash; EntityClass::SetProperty's store)
//                               0x004D017F         0x00491B5D         0x00491B5D
//   HUD::ElementBitmapBase::EventBitmap, type 3 (Uint) to SetTexture(uint) at
//     vtable +0x50              0x00698F40         0x0054DBF0         0x0054E940
//   WeaponClass::mIconTexture   +0x6C              +0x6C              +0x6C
//     (a PblHash; WeaponClass::SetProperty's store)
//                               0x0061F754         0x0067B084         0x0067C124
//   Weapon per channel, as HUD::GameEvents::UpdateWeaponEvents picks it: the
//     controlled object's GetWeaponIndex(channel) (primary slot +0x3C) and
//     GetWeapon(index) (+0x40), both thiscall RET 4, then Weapon::mClass (+0x64)
//                               0x006B2F0A         0x00560ADE         0x0056185E
//
// Same on every build: Character slots unit +0x148 and vehicle +0x14C
// (layout::Character); GameObject::IsRtti at primary slot +0x00, thiscall(hash),
// RET 4; the soldier's mState at Controllable + g_soldier->mState and the
// droideka's from the object start (layout::Droideka); PblHashTableCode::_Find
// cdecl(table, 0x2000, hash) on the texture table the terrain texture fix uses.
//
// EventBitmap's SetTexture(uint) only stores the hash. Nothing checks that the
// texture is loaded, so every candidate is checked here before it is sent.
// =============================================================================

namespace {
using namespace hud_class_icons;

constexpr int kTypeBool = 1;  // HUD::EventClass::Type
constexpr int kTypeUint = 3;

constexpr uint32_t kCtrl_Trackable     = 0x18;
constexpr uint32_t kVt_GetGameObject   = 0x1C;  // on the Trackable vptr
constexpr uint32_t kVt_GetEntityClass  = 0x28;  // GameObject primary vptr
constexpr uint32_t kVt_GetWeaponIndex  = 0x3C;  // Controllable primary vptr
constexpr uint32_t kVt_GetWeapon       = 0x40;
constexpr uint32_t kWeaponClassIcon    = 0x6C;  // WeaponClass::mIconTexture, every build
constexpr int      kWeaponChannels     = 2;
constexpr uint32_t kEC_Type            = 0x04;
constexpr uint32_t kEC_HandlerList     = 0x08;  // self-linked when nobody listens
constexpr uint32_t kTextureTableSize   = 0x2000;
constexpr uint32_t kSoldierRtti        = pbl_hash("EntitySoldier");
static_assert(kSoldierRtti == 0x5E8739F4u, "the target bar's soldier RTTI hash");

struct Layout {
   uint32_t classHealthTexture;  // EntityClass::mHealthTexture
   uint32_t droidekaState;       // EntityDroideka::mState, from the object start
};
constexpr Layout kModtools = { 0x48, layout::Droideka::kStateModtools };
constexpr Layout kRelease  = { 0x28, layout::Droideka::kStateRelease };  // Steam, GOG

using Find         = void*(__cdecl*)(uint32_t hash);
using FindFast     = void*(__fastcall*)(uint32_t hash);
using Create       = void*(__cdecl*)(int type, const char* fmt, ...);
using Send         = void(__fastcall*)(void* ev, void* edx);
using LocalPlayer  = uint8_t*(__cdecl*)(unsigned localIndex);
using TableFind    = void*(__cdecl*)(const void* table, uint32_t size, uint32_t hash);
using GameObjectFn = uint8_t*(__thiscall*)(void* self);
using ClassFn      = const uint8_t*(__thiscall*)(void* self);
using IsRtti       = bool(__thiscall*)(void* self, uint32_t hash);
using WeaponIndexFn = int(__thiscall*)(void* self, int channel);
using WeaponFn     = uint8_t*(__thiscall*)(void* self, int index);

// Each weapon channel's pair must stay texture then Disable, weapon1 then weapon2.
enum EventId { kUnitTexture, kUnitDisable, kUnitStance, kVehicleTexture, kVehicleDisable,
               kWeapon1Texture, kWeapon1Disable, kWeapon2Texture, kWeapon2Disable,
               kEventCount };
const char* const kNames[kEventCount] = {
   "player1.unit.healthTexture",
   "player1.unit.healthTextureDisable",
   "player1.unit.stance",
   "player1.vehicle.healthTexture",
   "player1.vehicle.healthTextureDisable",
   "player1.weapon1.iconTexture",
   "player1.weapon1.iconTextureDisable",
   "player1.weapon2.iconTexture",
   "player1.weapon2.iconTextureDisable",
};
constexpr int kTypes[kEventCount] = { kTypeUint, kTypeBool, kTypeUint, kTypeUint, kTypeBool,
                                      kTypeUint, kTypeBool, kTypeUint, kTypeBool };

bool        s_active = false;
Layout      s_layout = kModtools;
Find        s_find = nullptr;
FindFast    s_findFast = nullptr;
Create      s_create = nullptr;
Send        s_send = nullptr;
LocalPlayer s_localPlayer = nullptr;
TableFind   s_tableFind = nullptr;
const void* s_textureTable = nullptr;
const uintptr_t* s_eventList = nullptr;     // EventClass::sList
const uint32_t*  s_droidekaRtti = nullptr;  // set by a static initialiser

// Every pointer below dies with the mission: see hud_class_icons_open.
void*     s_events[kEventCount];
Published s_sentUnit, s_sentStance, s_sentVehicle, s_sentWeapon[kWeaponChannels];
Pick      s_unitPick, s_vehiclePick, s_weaponPick[kWeaponChannels];
bool      s_announced = false;

void invalidate()
{
   s_sentUnit.invalidate();
   s_sentStance.invalidate();
   s_sentVehicle.invalidate();
   s_unitPick.invalidate();
   s_vehiclePick.invalidate();
   for (int ch = 0; ch < kWeaponChannels; ++ch) {
      s_sentWeapon[ch].invalidate();
      s_weaponPick[ch].invalidate();
   }
}

bool guard(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[HudClassIcons] not active: prologue mismatch at %s 0x%08X",
                     what, (unsigned)va);
         return false;
      }
   }
   return true;
}

void* find_event(uint32_t hash)
{
   if (s_find)     return s_find(hash);
   if (s_findFast) return s_findFast(hash);
   return nullptr;
}

bool has_listener(const void* cls)
{
   if (!cls) return false;
   const uintptr_t node = (uintptr_t)cls + kEC_HandlerList;
   return *(const uintptr_t*)node != node;
}

void send(EventId e, uint32_t value)
{
   if (!s_events[e]) return;
   struct { void* mClass; uint32_t mData; } ev = { s_events[e], value };
   s_send(&ev, nullptr);
}

// A texture event enables the element and carries the hash; with nothing to
// show, the Disable twin is sent instead, as the stock team icons do.
void publish_texture(Published& sent, EventId texture, EventId disable, uint32_t hash)
{
   if (!sent.set(hash)) return;
   if (hash) send(texture, hash);
   else      send(disable, 1);
}

bool texture_loaded(uint32_t hash)
{
   return s_tableFind(s_textureTable, kTextureTableSize, hash) != nullptr;
}

uint8_t* game_object(uint8_t* controllable)
{
   void* trackable = controllable + kCtrl_Trackable;
   return ((GameObjectFn)(*(void***)trackable)[kVt_GetGameObject / 4])(trackable);
}

uint32_t class_texture(uint8_t* obj)
{
   const uint8_t* cls = ((ClassFn)(*(void***)obj)[kVt_GetEntityClass / 4])(obj);
   return cls ? *(const uint32_t*)(cls + s_layout.classHealthTexture) : 0;
}

// The soldier's state is read from the Controllable the Character slot holds;
// the droideka's from the object start. See layout::Droideka.
uint32_t unit_stance(uint8_t* unit, uint8_t* obj)
{
   const IsRtti is_rtti = (IsRtti)(*(void***)obj)[0];
   const uint32_t droideka = s_droidekaRtti ? *s_droidekaRtti : 0;
   if (droideka && is_rtti(obj, droideka))
      return droideka_stance(*(const int*)(obj + s_layout.droidekaState));
   if (is_rtti(obj, kSoldierRtti))
      return soldier_stance(*(const int*)(unit + g_soldier->mState));
   return kStand;
}

// The weapon class HUD::GameEvents::UpdateWeaponEvents shows for a channel.
const uint8_t* weapon_class(uint8_t* controlled, int channel)
{
   void** vt = *(void***)controlled;
   const int index = ((WeaponIndexFn)vt[kVt_GetWeaponIndex / 4])(controlled, channel);
   if (index < 0) return nullptr;
   uint8_t* weapon = ((WeaponFn)vt[kVt_GetWeapon / 4])(controlled, index);
   return weapon ? *(const uint8_t* const*)(weapon + layout::Weapon::kClass) : nullptr;
}

bool any_listener()
{
   for (void* cls : s_events)
      if (has_listener(cls)) return true;
   return false;
}

} // namespace

void hud_class_icons_resolve(uintptr_t base)
{
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   if (!g_addr->pbl_hash_table_find || !g_addr->tex_hash_table ||
       !g_addr->entity_droideka_rtti_hash || !g_addr->hud_event_send ||
       !g_addr->hud_event_class_list) {
      install_log("[HudClassIcons] not active: no address set for this build");
      return;
   }
   // Create, FindByHashID and GetLocalPlayer were guarded by the shared hooks'
   // installer before this runs; the two functions only this module calls are
   // guarded here.
   if (!guard(base, g_addr->pbl_hash_table_find, "PblHashTableCode::_Find",
              modtools ? "\x8B\x54\x24\x08\xD1\xFA\x56\x8B\x74\x24\x10\x8D"
                       : "\x55\x8B\xEC\x8B\x55\x0C\xD1\xFA\x56\x8B\x75\x10",
              "xxxxxxxxxxxx") ||
       !guard(base, g_addr->hud_event_send, "Event::Send", "\x51\x8B\x09\xE8", "xxxx"))
      return;

   s_layout = modtools ? kModtools : kRelease;
   // FindByHashID takes the hash on the stack on modtools and in ECX on retail.
   void* find = resolve(base, g_addr->hud_event_class_find);
   if (modtools) s_find     = (Find)find;
   else          s_findFast = (FindFast)find;
   s_create       = (Create)resolve(base, g_addr->hud_event_class_create);
   s_send         = (Send)resolve(base, g_addr->hud_event_send);
   s_localPlayer  = (LocalPlayer)resolve(base, g_addr->net_game_get_local_player);
   s_tableFind    = (TableFind)resolve(base, g_addr->pbl_hash_table_find);
   s_textureTable = resolve(base, g_addr->tex_hash_table);
   s_eventList    = (const uintptr_t*)resolve(base, g_addr->hud_event_class_list);
   s_droidekaRtti = (const uint32_t*)resolve(base, g_addr->entity_droideka_rtti_hash);
   s_active = true;
   install_log("[HudClassIcons] available: player1.unit.healthTexture, player1.unit.stance, "
               "player1.vehicle.healthTexture, player1.weaponN.iconTexture; inert without a binding");
}

// HUD::Manager::Open runs GameEvents::Open before any .lvl is read, and a .hud
// can only bind a class that already exists: create them here, finding first.
void hud_class_icons_open()
{
   if (!s_active) return;
   invalidate();
   s_announced = false;
   for (int i = 0; i < kEventCount; ++i) {
      s_events[i] = nullptr;
      __try {
         void* cls = find_event(pbl_hash(kNames[i]));
         if (!cls)   // literal name through "%s": Create runs its fmt through vsnprintf
            cls = s_create(kTypes[i], "%s", kNames[i]);
         else if (*(const uint32_t*)((const uint8_t*)cls + kEC_Type) != (uint32_t)kTypes[i]) {
            install_log("[HudClassIcons] %s already exists with another type; not published",
                        kNames[i]);
            cls = nullptr;
         }
         s_events[i] = cls;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         s_events[i] = nullptr;
      }
   }
}

void hud_class_icons_update()
{
   if (!s_active) return;
   __try {
      if (!s_eventList || *s_eventList == (uintptr_t)s_eventList) {
         for (void*& cls : s_events) cls = nullptr;  // the HUD has been torn down
         return;
      }
      if (!any_listener()) return;
      if (!s_announced) {
         s_announced = true;
         install_log("[HudClassIcons] publishing: a .hud binds at least one icon event");
      }

      // The vehicle is Character::mVehicle, the slot the stock player1.vehicle.*
      // events and seating mesh come from: a vehicle or a turret seat.
      uint8_t* chr     = s_localPlayer(0);
      uint8_t* unit    = chr ? *(uint8_t**)(chr + layout::Character::kUnit) : nullptr;
      uint8_t* vehicle = chr ? *(uint8_t**)(chr + layout::Character::kVehicle) : nullptr;

      uint32_t stance = kStand, unitTexture = 0, vehicleTexture = 0;
      if (uint8_t* obj = unit ? game_object(unit) : nullptr) {
         stance = unit_stance(unit, obj);
         unitTexture = s_unitPick.get(class_texture(obj), stance, texture_loaded);
      }
      if (uint8_t* obj = vehicle ? game_object(vehicle) : nullptr)
         vehicleTexture = s_vehiclePick.get(class_texture(obj), kStand, texture_loaded);

      publish_texture(s_sentUnit, kUnitTexture, kUnitDisable, unitTexture);
      if (s_sentStance.set(stance)) send(kUnitStance, stance);
      publish_texture(s_sentVehicle, kVehicleTexture, kVehicleDisable, vehicleTexture);

      // Weapons follow the object the stock weapon events do: remote, else
      // vehicle, else unit.
      uint8_t* remote     = chr ? *(uint8_t**)(chr + layout::Character::kRemote) : nullptr;
      uint8_t* controlled = remote ? remote : vehicle ? vehicle : unit;
      for (int ch = 0; ch < kWeaponChannels; ++ch) {
         const uint8_t* cls = controlled ? weapon_class(controlled, ch) : nullptr;
         const uint32_t icon = cls ? s_weaponPick[ch].get(
            *(const uint32_t*)(cls + kWeaponClassIcon), kStand, texture_loaded) : 0;
         publish_texture(s_sentWeapon[ch], EventId(kWeapon1Texture + 2 * ch),
                         EventId(kWeapon1Disable + 2 * ch), icon);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A torn-down object must not escape into the game's HUD update.
      invalidate();
   }
}
