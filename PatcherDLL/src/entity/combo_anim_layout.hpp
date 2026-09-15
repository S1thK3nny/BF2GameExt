#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The weapon and melee pairs alias. Their logical indices do not: melee starts
// after the 18 ordinary weapon indices. Keep every size derived from this budget.
inline constexpr int kComboAnimationCount = 90;
inline constexpr int kComboAnimationFirst = 134;
inline constexpr int kComboAnimationEnd = kComboAnimationFirst + kComboAnimationCount;
inline constexpr int kComboReferenceCount = 768;
inline constexpr int kComboStockMapCount = 30;
inline constexpr int kComboMapCount = 90;
inline constexpr int kComboStockBankCount = 16;
inline constexpr int kComboBankCount = 64;
inline constexpr int kComboCacheCount = 24;
inline constexpr int kComboWeaponCount = 20;
inline constexpr int kComboCustomCount = 10;
inline constexpr int kComboStockMapSize = 0x4B8;

struct combo_map_key {
   int bank;
   int weapon;
};

// Preserve the original bank record and its parent/lowres indices. Only the
// registry capacity changes; the engine still registers and resolves each name.
struct combo_anim_bank {
   char name[32];
   uint32_t hash;
   int parent;
   int lowres;
};
static_assert(sizeof(combo_anim_bank) == 0x2C);

// Engine collision samples retain their original 900-byte stride. Only the
// containing array moves; all vectors and flags within each record stay put.
struct combo_collision_table {
   uint32_t data[225];
};
static_assert(sizeof(combo_map_key) == 8);
static_assert(sizeof(combo_collision_table) == 0x384);

struct combo_lowres_entry {
   int map;
   uint8_t animation;
   uint8_t unused;
   uint16_t frame_count;
};
static_assert(sizeof(combo_lowres_entry) == 8);
inline constexpr int kComboLowresCount = kComboMapCount * kComboAnimationEnd;

struct combo_anim_map {
   void* action[38][2];
   void* movement[6][13][2];
   union {
      void* weapon[3][6][2];
      void* melee[kComboAnimationCount][2];
   };
   uint32_t custom[kComboCustomCount];
};

// Retain these keys at their original addresses. Engine parent-pop code walks
// them at the stock stride; only AddMap's returned payload moves out of line.
struct combo_anim_cache_entry {
   int bank;
   int weapon;
   uint8_t unused_map[kComboStockMapSize];
};

struct combo_anim_stack {
   struct entry {
      int bank;
      combo_anim_map* maps[kComboWeaponCount];
   } entries[3];
   uint32_t depth;
};

static_assert(sizeof(void*) == 4, "BF2's animation maps use 32-bit pointers");
static_assert(offsetof(combo_anim_map, movement) == 0x130);
static_assert(offsetof(combo_anim_map, melee) == 0x3A0);
static_assert(offsetof(combo_anim_map, custom) == 0x670);
static_assert(sizeof(combo_anim_map) == 0x698);
static_assert(sizeof(combo_anim_cache_entry) == 0x4C0);
static_assert(sizeof(combo_anim_stack) == 0x100);
static_assert(kComboAnimationEnd < 0xFF); // 0xFF remains an unassigned field.

struct combo_anim_storage {
   void* owner = nullptr;
   int map_count = 0;
   combo_anim_cache_entry* cache = nullptr;
   unsigned cache_claims = 0;
   unsigned cache_high_water = 0;
   unsigned cache_failures = 0;
   combo_anim_map maps[kComboMapCount] = {};
   combo_anim_map temporary[kComboCacheCount] = {};
   combo_collision_table collisions[kComboMapCount] = {};

   void reset(void* instance, int count)
   {
      owner = instance;
      map_count = count;
      cache = nullptr;
      cache_claims = cache_high_water = cache_failures = 0;
      memset(maps, 0, sizeof(maps));
      memset(temporary, 0, sizeof(temporary));
      memset(collisions, 0, sizeof(collisions));
   }

   combo_anim_map* get(void* instance, int map)
   {
      if (!instance || instance != owner || map < 0 || map >= map_count || map >= kComboMapCount)
         return nullptr;
      return &maps[map];
   }

   // Called with the unchanged this+0x24+map*0x4B8 argument from the engine.
   combo_anim_map* translate(void* legacy)
   {
      const uintptr_t first = (uintptr_t)owner + 0x24;
      if (!owner || (uintptr_t)legacy < first) return nullptr;
      const uintptr_t offset = (uintptr_t)legacy - first;
      if (offset % kComboStockMapSize || offset / kComboStockMapSize >= kComboMapCount)
         return nullptr;
      return get(owner, (int)(offset / kComboStockMapSize));
   }

   combo_anim_map* add_cache_map(combo_anim_cache_entry* entries, int bank, int weapon)
   {
      if (!entries || (cache && cache != entries) || bank < 0 || weapon < 0 || weapon >= kComboWeaponCount) {
         ++cache_failures;
         return nullptr;
      }
      cache = entries;
      for (int i = 0; i < kComboCacheCount; ++i)
         if (entries[i].bank == bank && entries[i].weapon == weapon) return &temporary[i];

      for (int i = 0; i < kComboCacheCount; ++i) {
         if (entries[i].bank != -1) continue;
         entries[i].bank = bank;
         entries[i].weapon = weapon;
         memset(&temporary[i], 0xFF, sizeof(temporary[i]));
         ++cache_claims;
         unsigned used = 0;
         for (int j = 0; j < kComboCacheCount; ++j) used += entries[j].bank != -1;
         if (used > cache_high_water) cache_high_water = used;
         return &temporary[i];
      }
      ++cache_failures;
      return nullptr;
   }

   combo_anim_map* get_supplied_map(combo_anim_stack* stack, int weapon, void* legacy)
   {
      if (!stack || !stack->depth || stack->depth > 3 || weapon < 0 || weapon >= kComboWeaponCount)
         return nullptr;
      combo_anim_map*& slot = stack->entries[stack->depth - 1].maps[weapon];
      if (slot) return slot; // Parent maps and previous calls intentionally alias.
      combo_anim_map* map = translate(legacy);
      if (!map) return nullptr;
      memset(map, 0xFF, sizeof(*map));
      slot = map;
      return map;
   }
};

inline void* combo_anim_body(combo_anim_map* map, int index, bool lower)
{
   if (!map || index < 0 || index >= kComboAnimationEnd) return nullptr;
   void* result;
   if (index < 38)
      result = map->action[index][lower];
   else if (index < 116)
      result = map->movement[(index - 38) / 13][(index - 38) % 13][lower];
   else if (index < kComboAnimationFirst)
      result = map->weapon[(index - 116) / 6][(index - 116) % 6][lower];
   else
      result = map->melee[index - kComboAnimationFirst][lower];
   return (uintptr_t)result == UINT32_MAX ? nullptr : result;
}
