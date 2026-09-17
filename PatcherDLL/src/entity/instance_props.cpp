#include "pch.h"
#include "entity/instance_props.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

// =============================================================================
// Family: VehicleSpawn
//
// sVehicleSpawnList is a PblList whose head IS the global, with _head at +0:
//     +0x00  PblList* _pList     (self)
//     +0x04  Node*    _pNext
//     +0x08  Node*    _pPrev
//     +0x0C  T*       _pObject
//     +0x10  int      _iCount
// Walk from head->_pNext until the node address comes back around to the head.
// Same walk as render/spawn_vehicle_list.cpp, which documents the layout in full.
//
// VehicleSpawn is 368 bytes and identical on modtools, Steam and GOG.  mNameId at
// +0x78 is the hash of the name typed into ZeroEditor for that world instance;
// UpdateSpawn reverse-hashes it to name the vehicles it creates, so it is both
// live and discoverable in game.
//
// VehicleSpawn::SetProperty is the engine's own [InstanceProperties] parser and
// nothing in it is load-time only, so it is safe to drive at runtime.  Forwarding
// to it rather than poking fields is what keeps the flyer, carrier and command
// post bookkeeping correct: the class keys alone touch mSpawnClass, mFlyerClass
// and mUseCarrier, gated on EntityFlyerClass / EntityCarrierClass RTTI checks.
//
// SpawnCount is deliberately refused, see kHashSpawnCount below.
// =============================================================================

static constexpr int kNode_Next   = 0x04;
static constexpr int kNode_Object = 0x0C;
static constexpr int kVS_NameId   = 0x78;

// A corrupt or mid-construction list must not spin forever.  A busy map runs a
// few dozen vehicle spawns; this is a wide safety margin.
static constexpr int kMaxListWalk = 4096;

// __thiscall(this, PblHash prop, const char* value), two stack dwords, RET 8.
// Verified on all three builds: the modtools prologue reads the hash at
// [ESP+0x18] behind five pushes, and Steam/GOG read it at [EBP+8].
using fn_vs_set_property_t = void(__fastcall*)(void* ecx, void* edx,
                                               uint32_t prop, const char* value);

// PblHash: FNV-1a over `c | 0x20`.  '_' is 0x5F, so the OR lands it on 0x7F all by
// itself, which is what the engine's own hash does.
static uint32_t pbl_hash(const char* s)
{
   uint32_t h = 0x811c9dc5u;
   for (; *s; ++s) {
      h ^= (uint32_t)(uint8_t)(*s | 0x20);
      h *= 0x01000193u;
   }
   return h;
}

// Keys the engine parses but that are not safe to drive at runtime.
//
// SpawnCount resizes VehicleTracker::sMemoryPool by shrinking it and regrowing
// it, while trackers allocated out of that pool may be live.  It is a load-time
// key in practice and there is no reason a script needs it mid-mission.
static constexpr uint32_t kHashSpawnCount = 0x88923FF7u;

// Returns how many spawners carry the name.  `written` counts the ones actually
// written, which is fewer when the key is refused.
static int family_vehicle_spawn(uint32_t nameHash, uint32_t propHash,
                                const char* name, const char* value, int& written)
{
   if (!g_addr->vehicle_spawn_list || !g_addr->vehicle_spawn_set_property)
      return 0;

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);

   uint8_t* const head = (uint8_t*)resolve(base, g_addr->vehicle_spawn_list);
   if (!head) return 0;

   auto setProperty =
      (fn_vs_set_property_t)resolve(base, g_addr->vehicle_spawn_set_property);

   int matched = 0;
   uint8_t* node = *(uint8_t**)(head + kNode_Next);

   for (int guard = 0; node && node != head && guard < kMaxListWalk; ++guard) {
      uint8_t* vs = *(uint8_t**)(node + kNode_Object);
      node = *(uint8_t**)(node + kNode_Next);
      if (!vs) continue;

      if (*(const uint32_t*)(vs + kVS_NameId) != nameHash) continue;
      ++matched;

      if (propHash == kHashSpawnCount) continue;

      setProperty(vs, nullptr, propHash, value);
      ++written;
   }

   if (matched && propHash == kHashSpawnCount) {
      auto fn_log = get_gamelog();
      fn_log("[SetInstanceProperty] SpawnCount is load-time only (it resizes a "
             "live memory pool); ignored for \"%s\"\n", name);
   }

   return matched;
}

// =============================================================================
// Fall-through: EntityEx
//
// Everything the stock SetProperty can reach: props, buildings, command posts,
// turrets, vehicles.  Same lookup and same call the engine makes, verified on all
// three builds out of Lua_Callbacks::SetProperty and FindEntity_:
//
//     obj = PblHashTableCode::_Find(EntityEx::mIdMap_._uiTable, 0x800, nameHash);
//     obj->vtable[0x10](propHash, value);          // __thiscall
//
// _Find is plain cdecl on every build (saves EBP/ESI/EDI, caller pops 12), so a
// typed pointer is safe.  Only EntityEx instances are ever inserted into mIdMap_,
// which is why FindEntity_'s trailing EntityEx RTTI check is not repeated here.
//
// Not replicated: the stock callback's follow-up write to the client-side
// NDObject in network games.
//
// This runs only when no special family matched.  A spawner's live vehicle carries
// the spawner's own name, so the spawner has to win that collision: the vehicle is
// still reachable through the stock SetProperty.
// =============================================================================

static constexpr int kIdMapSlots        = 0x800;
static constexpr int kVtbl_SetProperty  = 0x10;

using fn_hash_table_find_t = void*(__cdecl*)(void* table, int slots, uint32_t hash);

static int family_entity_ex(uint32_t nameHash, uint32_t propHash,
                            const char* value, int& written)
{
   if (!g_addr->entityex_id_map || !g_addr->pbl_hash_table_find)
      return 0;

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);

   auto find  = (fn_hash_table_find_t)resolve(base, g_addr->pbl_hash_table_find);
   void* obj  = find(resolve(base, g_addr->entityex_id_map), kIdMapSlots, nameHash);
   if (!obj) return 0;

   auto setProperty = (fn_vs_set_property_t)
      (*(void***)obj)[kVtbl_SetProperty / sizeof(void*)];

   setProperty(obj, nullptr, propHash, value);
   ++written;
   return 1;
}

// -----------------------------------------------------------------------------

int instance_props_set(const char* name, const char* prop, const char* value)
{
   if (!name || !*name || !prop || !*prop || !value) return 0;

   const uint32_t nameHash = pbl_hash(name);
   const uint32_t propHash = pbl_hash(prop);

   int written = 0;

   if (family_vehicle_spawn(nameHash, propHash, name, value, written) == 0 &&
       family_entity_ex(nameHash, propHash, value, written) == 0) {
      auto fn_log = get_gamelog();
      fn_log("[SetInstanceProperty] no world object named \"%s\"\n", name);
   }

   return written;
}
