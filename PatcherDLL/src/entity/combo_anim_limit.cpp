#include "pch.h"

#include "combo_anim_limit.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <detours.h>
#include <vector>

// The class is a singleton (Create checks sInstance). Its maps and the one
// load-time MapCache get separate payloads; the original class tail, cache keys,
// stack frame and parent pointer bookkeeping never move.
namespace {

combo_anim_storage g_storage;
combo_map_key g_mapRegistry[kComboMapCount];
combo_anim_bank g_bankRegistry[kComboBankCount];
// The original initializer fills these before reading them. The order list
// includes a terminating -1 after the last bank; weapon IDs retain their cap.
int g_bankOrder[kComboBankCount + 1];
int g_bankWeaponMaps[kComboBankCount][kComboWeaponCount];
// PostLoad deduplicates by (map, logical animation index), resets the count for
// each class, and copies the live records to a dynamically sized heap array.
combo_lowres_entry g_lowresScratch[kComboLowresCount];
bool g_installed = false;
bool g_initializing = false;
void** g_instance = nullptr;
int* g_mapCount = nullptr;
int* g_bankCount = nullptr;
int* g_nameCount = nullptr;
int* g_referenceCount = nullptr;
void* g_originals[13] = {};
uintptr_t g_movementFault = 0;
uintptr_t g_movementCaller = 0;

// ReadProcessMemory reports inaccessible memory without faulting again inside
// the crash handler. Check the addition before reading a field from an object.
template<typename T>
bool read_crash_field(uintptr_t base, size_t offset, T& value)
{
   if (!base || offset > UINTPTR_MAX - sizeof(T) || base > UINTPTR_MAX - offset - sizeof(T))
      return false;
   SIZE_T bytes = 0;
   return ReadProcessMemory(GetCurrentProcess(), (void*)(base + offset), &value, sizeof(value), &bytes) &&
          bytes == sizeof(value);
}

bool current_owner(void* owner)
{
   return owner && g_instance && owner == *g_instance && owner == g_storage.owner;
}

// Integer-only dispatcher. The shims preserve the engine's observed register
// contract, including ECX/EDX kept live across leaf calls by the retail compiler.
__declspec(noinline) uintptr_t __cdecl dispatch(unsigned kind, void* owner, const uint32_t* args)
{
   if (kind == 10)
      return (uintptr_t)g_storage.add_cache_map((combo_anim_cache_entry*)owner, (int)args[0],
                                                (int)args[1]);
   if (kind == 11)
      return (uintptr_t)g_storage.get_supplied_map((combo_anim_stack*)owner, (int)args[0], (void*)args[1]);
   if (!current_owner(owner)) return 0;

   const bool lower = (kind & 1) != 0;
   combo_anim_map* map = nullptr;
   int index = 0;
   switch (kind) {
   case 0:
   case 1:
      if (args[0] >= 13) return 0;
      map = g_storage.get(owner, (int)args[2]);
      index = 38 + ((args[1] % 3) + ((args[3] & 0xFF) ? 3 : 0)) * 13 + args[0];
      break;
   case 2:
   case 3:
      if (args[0] >= 38) return 0;
      map = g_storage.get(owner, (int)args[1]);
      index = (int)args[0];
      break;
   case 4:
   case 5:
      if (args[0] >= 3 || args[2] >= 6) return 0;
      map = g_storage.get(owner, (int)args[1]);
      index = 116 + args[0] * 6 + args[2];
      break;
   case 6:
   case 7:
      map = g_storage.get(owner, (int)args[0]);
      index = args[1] & 0xFF; // The engine argument is a byte, passed in a stack word.
      break;
   case 8:
      map = g_storage.get(owner, (int)args[0]);
      return map && args[1] < kComboCustomCount ? map->custom[args[1]] : 0;
   case 9: {
      // Debug animation preview uses a PHYSICAL pair index, without the logical
      // weapon/melee gap. This helper is compiled out of Steam/GOG.
      map = g_storage.get(owner, (int)args[0]);
      if (args[1] >= 116 + kComboAnimationCount || args[2] > 1) return 0;
      const int logical = args[1] < 116 ? (int)args[1] : (int)args[1] + 18;
      return (uintptr_t)combo_anim_body(map, logical, args[2] != 0);
   }
   default:
      return 0;
   }
   return (uintptr_t)combo_anim_body(map, index, lower);
}

// PUSHFD/PUSHAD put saved ECX/EAX at +24/+28. After the XMM save those become
// +152/+156; the first engine argument is at +168. RET retains each original
// callee-pop size. No floating-point work or engine calls occur in dispatch.
// clang-format off
#define COMBO_SHIM(name, kind, pop_bytes)                  \
   __declspec(naked) void name()                           \
   {                                                      \
      __asm { pushfd }                                    \
      __asm { pushad }                                    \
      __asm { sub esp, 128 }                              \
      __asm { movdqu [esp], xmm0 }                        \
      __asm { movdqu [esp + 16], xmm1 }                    \
      __asm { movdqu [esp + 32], xmm2 }                    \
      __asm { movdqu [esp + 48], xmm3 }                    \
      __asm { movdqu [esp + 64], xmm4 }                    \
      __asm { movdqu [esp + 80], xmm5 }                    \
      __asm { movdqu [esp + 96], xmm6 }                    \
      __asm { movdqu [esp + 112], xmm7 }                   \
      __asm { lea eax, [esp + 168] }                      \
      __asm { mov edx, [esp + 152] }                      \
      __asm { push eax }                                  \
      __asm { push edx }                                  \
      __asm { push kind }                                 \
      __asm { call dispatch }                             \
      __asm { add esp, 12 }                               \
      __asm { mov [esp + 156], eax }                      \
      __asm { movdqu xmm0, [esp] }                        \
      __asm { movdqu xmm1, [esp + 16] }                    \
      __asm { movdqu xmm2, [esp + 32] }                    \
      __asm { movdqu xmm3, [esp + 48] }                    \
      __asm { movdqu xmm4, [esp + 64] }                    \
      __asm { movdqu xmm5, [esp + 80] }                    \
      __asm { movdqu xmm6, [esp + 96] }                    \
      __asm { movdqu xmm7, [esp + 112] }                   \
      __asm { add esp, 128 }                              \
      __asm { popad }                                     \
      __asm { popfd }                                     \
      __asm { ret pop_bytes }                             \
   }

COMBO_SHIM(shim_upper_movement, 0, 16)
COMBO_SHIM(shim_lower_movement, 1, 16)
COMBO_SHIM(shim_upper_action, 2, 8)
COMBO_SHIM(shim_lower_action, 3, 8)
COMBO_SHIM(shim_upper_weapon, 4, 12)
COMBO_SHIM(shim_lower_weapon, 5, 12)
COMBO_SHIM(shim_upper_generic, 6, 8)
COMBO_SHIM(shim_lower_generic, 7, 8)
COMBO_SHIM(shim_custom, 8, 8)
COMBO_SHIM(shim_physical, 9, 12)
COMBO_SHIM(shim_map_cache_add, 10, 8)
COMBO_SHIM(shim_supplied_pointer_get_map, 11, 8)
#undef COMBO_SHIM
// clang-format on

bool valid_bank_maps(int banks, int maps)
{
   if (banks < 1 || banks > kComboBankCount || maps < 0 || maps > kComboMapCount) {
      install_log("[ComboAnimLimit] invalid registry counts: banks=%d/%d maps=%d/%d", banks,
                  kComboBankCount, maps, kComboMapCount);
      return false;
   }
   for (int i = 0; i < maps; ++i) {
      const combo_map_key& map = g_mapRegistry[i];
      if (map.bank >= 0 && map.bank < banks && map.weapon >= 0 && map.weapon < kComboWeaponCount)
         continue;
      install_log("[ComboAnimLimit] invalid registered map: map=%d bank=%d weapon=%d banks=%d/%d",
                  i, map.bank, map.weapon, banks, kComboBankCount);
      return false;
   }
   return true;
}

void begin_initialize(void* owner)
{
   // A second or oversized context cannot share the singleton payload. Refuse
   // before any engine writer can enter it instead of corrupting another map.
   if (g_initializing || !owner || !g_mapCount || !g_bankCount ||
       !valid_bank_maps(*g_bankCount, *g_mapCount)) {
      install_log("[ComboAnimLimit] invalid initialization context: owner=%p maps=%d reentry=%d",
                  owner, g_mapCount ? *g_mapCount : -1, g_initializing);
      FatalAppExitA(0, "BF2GameExt: invalid combo animation map context. See BF2GameExt.log.");
   }
   g_initializing = true;
   g_storage.reset(owner, *g_mapCount);
   memset(g_bankOrder, 0xFF, sizeof(g_bankOrder));
   memset(g_bankWeaponMaps, 0xFF, sizeof(g_bankWeaponMaps));
   install_log("[ComboAnimLimit] initializing: owner=%p names=%d/%d references=%d/%d maps=%d/%d "
               "banks=%d/%d",
               owner, *g_nameCount, kComboAnimationCount, *g_referenceCount, kComboReferenceCount,
               g_storage.map_count, kComboMapCount, *g_bankCount, kComboBankCount);
}

void end_initialize()
{
   // Never retain a pointer to the initializer's stack after it returns.
   g_storage.cache = nullptr;
   g_initializing = false;
   install_log("[ComboAnimLimit] initialized: names=%d/%d references=%d/%d maps=%d/%d "
               "banks=%d/%d cache_peak=%u/%d cache_claims=%u cache_failures=%u",
               *g_nameCount, kComboAnimationCount, *g_referenceCount, kComboReferenceCount,
               g_storage.map_count, kComboMapCount, *g_bankCount, kComboBankCount, g_storage.cache_high_water,
               kComboCacheCount, g_storage.cache_claims, g_storage.cache_failures);
}

void __fastcall hooked_initialize_modtools(void* owner, void* edx)
{
   begin_initialize(owner);
   ((void(__fastcall*)(void*, void*))g_originals[12])(owner, edx);
   end_initialize();
}

void __cdecl hooked_initialize_retail()
{
   begin_initialize(*g_instance);
   ((void(__cdecl*)())g_originals[12])();
   end_initialize();
}

struct function_site {
   uintptr_t va;
   void* hook;
   uint8_t expected[16];
};

enum class patch_kind {
   custom_bytes,
   custom_dwords,
   cleanup_base,
   cleanup_stride,
   jetpack_animation,
   registry_address,
   map_capacity,
   collision_reader,
   collision_base,
   collision_clear,
   lowres_scratch,
   bank_address,
   bank_capacity,
   bank_order,
   bank_map
};
struct storage_patch {
   uintptr_t va;
   uint8_t length;
   uint8_t operand;
   patch_kind kind;
   uint8_t expected[11];
};

// Verified against the debug reference and both retail images. GOG's code in
// this family is Steam+0x10A0; globals have separate addresses below.
// clang-format off
const function_site kModtoolsFunctions[] = {
   {0x0057DBE0, (void*)shim_upper_movement, {0x8B, 0x44, 0x24, 0x08, 0x56, 0x33, 0xD2, 0xBE, 0x03, 0x00, 0x00, 0x00, 0xF7, 0xF6, 0x8A, 0x44}},
   {0x0057DC40, (void*)shim_lower_movement, {0x8B, 0x44, 0x24, 0x08, 0x56, 0x33, 0xD2, 0xBE, 0x03, 0x00, 0x00, 0x00, 0xF7, 0xF6, 0x8A, 0x44}},
   {0x0057DCA0, (void*)shim_upper_action, {0x8B, 0x44, 0x24, 0x08, 0x8B, 0x54, 0x24, 0x04, 0x69, 0xC0, 0x97, 0x00, 0x00, 0x00, 0x03, 0xC2}},
   {0x0057DCC0, (void*)shim_lower_action, {0x8B, 0x44, 0x24, 0x08, 0x8B, 0x54, 0x24, 0x04, 0x69, 0xC0, 0x97, 0x00, 0x00, 0x00, 0x03, 0xC2}},
   {0x0057DCE0, (void*)shim_upper_weapon, {0x8B, 0x54, 0x24, 0x08, 0x8B, 0x44, 0x24, 0x04, 0x69, 0xD2, 0x97, 0x00, 0x00, 0x00, 0x56, 0x8B}},
   {0x0057DD10, (void*)shim_lower_weapon, {0x8B, 0x54, 0x24, 0x08, 0x8B, 0x44, 0x24, 0x04, 0x69, 0xD2, 0x97, 0x00, 0x00, 0x00, 0x56, 0x8B}},
   {0x0057DD40, (void*)shim_upper_generic, {0x8B, 0x54, 0x24, 0x04, 0x0F, 0xB6, 0x44, 0x24, 0x08, 0x69, 0xD2, 0x2E, 0x01, 0x00, 0x00, 0xD1}},
   {0x0057DD80, (void*)shim_lower_generic, {0x8B, 0x54, 0x24, 0x04, 0x0F, 0xB6, 0x44, 0x24, 0x08, 0x69, 0xD2, 0x2E, 0x01, 0x00, 0x00, 0x8D}},
   {0x0057DEA0, (void*)shim_custom, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x54, 0x24, 0x08, 0x69, 0xC0, 0x2E, 0x01, 0x00, 0x00, 0x03, 0xC2}},
   {0x0057DAE0, (void*)shim_physical, {0x8B, 0x44, 0x24, 0x08, 0x85, 0xC0, 0x56, 0x7C, 0x26, 0x8B, 0x54, 0x24, 0x10, 0x8D, 0x34, 0x42}},
   {0x0057F2B0, (void*)shim_map_cache_add, {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14, 0x33, 0xC0, 0x8B, 0xD1, 0x90}},
   {0x0057F520, (void*)shim_supplied_pointer_get_map, {0x8B, 0xD1, 0x8B, 0x82, 0xFC, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x75, 0x03, 0xC2, 0x08, 0x00, 0x76}},
   {0x00581AF0, (void*)hooked_initialize_modtools, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0xB8, 0x74, 0x91, 0x00, 0x00, 0xE8, 0x70, 0x0E, 0x35, 0x00}},
};

const function_site kRetailFunctions[] = {
   {0x6438a0, (void*)shim_upper_movement, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x0C, 0xB8, 0xAB, 0xAA, 0xAA, 0xAA, 0xF7, 0xE6, 0x57, 0xD1}},
   {0x6438f0, (void*)shim_lower_movement, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x0C, 0xB8, 0xAB, 0xAA, 0xAA, 0xAA, 0xF7, 0xE6, 0x57, 0xD1}},
   {0x643940, (void*)shim_upper_action, {0x55, 0x8B, 0xEC, 0x69, 0x45, 0x0C, 0x97, 0x00, 0x00, 0x00, 0x03, 0x45, 0x08, 0x8B, 0x44, 0xC1}},
   {0x643960, (void*)shim_lower_action, {0x55, 0x8B, 0xEC, 0x69, 0x45, 0x0C, 0x97, 0x00, 0x00, 0x00, 0x03, 0x45, 0x08, 0x8B, 0x44, 0xC1}},
   {0x643980, (void*)shim_upper_weapon, {0x55, 0x8B, 0xEC, 0x69, 0x55, 0x0C, 0x97, 0x00, 0x00, 0x00, 0x8B, 0x45, 0x08, 0x03, 0x55, 0x10}},
   {0x6439b0, (void*)shim_lower_weapon, {0x55, 0x8B, 0xEC, 0x69, 0x55, 0x0C, 0x97, 0x00, 0x00, 0x00, 0x8B, 0x45, 0x08, 0x03, 0x55, 0x10}},
   {0x6439e0, (void*)shim_upper_generic, {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x0F, 0xB6, 0xD0, 0x69, 0x45, 0x08, 0x2E, 0x01, 0x00, 0x00}},
   {0x643a10, (void*)shim_lower_generic, {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x0F, 0xB6, 0xC0, 0x8D, 0x14, 0x45, 0x01, 0x00, 0x00, 0x00}},
   {0x6442f0, (void*)shim_custom, {0x55, 0x8B, 0xEC, 0x69, 0x45, 0x08, 0x2E, 0x01, 0x00, 0x00, 0x03, 0x45, 0x0C, 0x8B, 0x84, 0x81}},
   {}, // GET_SOLDIER_ANIMATION is debug-preview-only.
   {0x644b40, (void*)shim_map_cache_add, {0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0x75, 0x0C, 0x8B, 0xD9, 0x57, 0x8B, 0x7D, 0x08, 0x33, 0xD2}},
   {0x644c20, (void*)shim_supplied_pointer_get_map, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x8B, 0x96, 0xFC, 0x00, 0x00, 0x00, 0x85, 0xD2, 0x75, 0x07}},
   {0x645b70, (void*)hooked_initialize_retail, {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04}},
};

const storage_patch kModtoolsPatches[] = {
   // Initialize samples the jetpack bone from map 0's upper action 25 directly.
   // It bypasses every getter: leaving this at this+0xEC reads the abandoned map.
   {0x0058414D, 6, 2, patch_kind::jetpack_animation, {0x8B, 0x86, 0xEC, 0x00, 0x00, 0x00}},
   {0x00580584, 7, 3, patch_kind::custom_bytes, {0x8B, 0xAC, 0x83, 0x90, 0x04, 0x00, 0x00}},
   {0x00580622, 7, 3, patch_kind::custom_bytes, {0x89, 0xAC, 0x8B, 0x90, 0x04, 0x00, 0x00}},
   {0x00580724, 7, 3, patch_kind::custom_bytes, {0x8B, 0xAC, 0x90, 0x90, 0x04, 0x00, 0x00}},
   {0x00580732, 7, 3, patch_kind::custom_bytes, {0x8D, 0x84, 0x90, 0x90, 0x04, 0x00, 0x00}},
   {0x00580803, 7, 3, patch_kind::custom_bytes, {0x89, 0x84, 0x91, 0x90, 0x04, 0x00, 0x00}},
   {0x005815D4, 7, 3, patch_kind::custom_bytes, {0x8B, 0xAC, 0x88, 0x90, 0x04, 0x00, 0x00}},
   {0x00581678, 7, 3, patch_kind::custom_bytes, {0x89, 0x9C, 0x81, 0x90, 0x04, 0x00, 0x00}},
   {0x005816CC, 11, 3, patch_kind::custom_bytes, {0xC7, 0x84, 0x90, 0x90, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
   {0x0058172E, 7, 3, patch_kind::custom_bytes, {0x89, 0x84, 0x91, 0x90, 0x04, 0x00, 0x00}},
   {0x00583EF9, 6, 2, patch_kind::custom_bytes, {0x81, 0xC7, 0x90, 0x04, 0x00, 0x00}},
   {0x00584021, 6, 2, patch_kind::cleanup_base, {0x81, 0xC3, 0xC4, 0x03, 0x00, 0x00}},
   {0x00584087, 6, 2, patch_kind::cleanup_stride, {0x81, 0xC3, 0xB8, 0x04, 0x00, 0x00}},
};

const storage_patch kRetailPatches[] = {
   {0x00647456, 6, 2, patch_kind::jetpack_animation, {0xFF, 0xB0, 0xEC, 0x00, 0x00, 0x00}},
   {0x645763, 7, 3, patch_kind::custom_bytes, {0x8B, 0x84, 0x87, 0x90, 0x04, 0x00, 0x00}},
   {0x645788, 7, 3, patch_kind::custom_bytes, {0x89, 0x84, 0x8F, 0x90, 0x04, 0x00, 0x00}},
   {0x6457d4, 7, 3, patch_kind::custom_bytes, {0x8B, 0x84, 0x83, 0x90, 0x04, 0x00, 0x00}},
   {0x64581c, 7, 3, patch_kind::custom_bytes, {0x89, 0x84, 0x8B, 0x90, 0x04, 0x00, 0x00}},
   {0x645830, 11, 3, patch_kind::custom_bytes, {0xC7, 0x84, 0x83, 0x90, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
   {0x645893, 7, 3, patch_kind::custom_bytes, {0x89, 0x8C, 0x83, 0x90, 0x04, 0x00, 0x00}},
   {0x6458e5, 5, 1, patch_kind::custom_dwords, {0x05, 0x24, 0x01, 0x00, 0x00}},
   {0x645974, 7, 3, patch_kind::custom_bytes, {0x89, 0x8C, 0x82, 0x90, 0x04, 0x00, 0x00}},
   {0x646707, 7, 3, patch_kind::custom_bytes, {0xC7, 0x45, 0xD8, 0x90, 0x04, 0x00, 0x00}},
   {0x64676f, 7, 3, patch_kind::custom_bytes, {0x89, 0x8C, 0x82, 0x90, 0x04, 0x00, 0x00}},
   {0x6471c9, 5, 1, patch_kind::custom_bytes, {0xB8, 0x90, 0x04, 0x00, 0x00}},
   {0x647274, 7, 3, patch_kind::custom_bytes, {0x8B, 0x94, 0x81, 0x90, 0x04, 0x00, 0x00}},
   {0x6472a1, 7, 3, patch_kind::custom_bytes, {0x89, 0x94, 0x81, 0x90, 0x04, 0x00, 0x00}},
   {0x6472e5, 7, 3, patch_kind::custom_bytes, {0x89, 0x8C, 0x82, 0x90, 0x04, 0x00, 0x00}},
   {0x647349, 6, 2, patch_kind::cleanup_base, {0x81, 0xC7, 0xC4, 0x03, 0x00, 0x00}},
   {0x64739d, 6, 2, patch_kind::cleanup_stride, {0x81, 0xC7, 0xB8, 0x04, 0x00, 0x00}},
};

const storage_patch kModtoolsMapPatches[] = {
   {0x570414, 7, 3, patch_kind::registry_address, {0x8D, 0x04, 0xC5, 0x58, 0xF5, 0xAC, 0x00}},
   {0x570775, 7, 3, patch_kind::registry_address, {0x39, 0x34, 0xC5, 0x58, 0xF5, 0xAC, 0x00}},
   {0x57077e, 7, 3, patch_kind::registry_address, {0x39, 0x14, 0xC5, 0x5C, 0xF5, 0xAC, 0x00}},
   {0x5707be, 3, 2, patch_kind::map_capacity, {0x83, 0xF8, 0x1E}},
   {0x5287ca, 7, 3, patch_kind::collision_reader, {0x8D, 0x84, 0x08, 0xB4, 0x8D, 0x00, 0x00}},
   {0x53d3a2, 7, 3, patch_kind::collision_reader, {0x8D, 0x84, 0x11, 0xB4, 0x8D, 0x00, 0x00}},
   {0x58427f, 6, 2, patch_kind::collision_base, {0x8D, 0x88, 0xF4, 0x8F, 0x00, 0x00}},
   {0x58428e, 6, 2, patch_kind::collision_base, {0x8D, 0x90, 0x34, 0x91, 0x00, 0x00}},
   {0x5878c2, 7, 3, patch_kind::lowres_scratch, {0x39, 0x94, 0xC4, 0x90, 0x01, 0x00, 0x00}},
   {0x5878cb, 7, 3, patch_kind::lowres_scratch, {0x38, 0x9C, 0xC4, 0x94, 0x01, 0x00, 0x00}},
   {0x587923, 7, 3, patch_kind::lowres_scratch, {0x89, 0xAC, 0xC4, 0x90, 0x01, 0x00, 0x00}},
   {0x58792a, 7, 3, patch_kind::lowres_scratch, {0x88, 0x9C, 0xC4, 0x94, 0x01, 0x00, 0x00}},
   {0x587932, 8, 4, patch_kind::lowres_scratch, {0x66, 0x89, 0x94, 0xC4, 0x96, 0x01, 0x00, 0x00}},
   {0x587990, 7, 3, patch_kind::lowres_scratch, {0x39, 0x94, 0xC4, 0x90, 0x01, 0x00, 0x00}},
   {0x587999, 7, 3, patch_kind::lowres_scratch, {0x38, 0x9C, 0xC4, 0x94, 0x01, 0x00, 0x00}},
   {0x5879e8, 7, 3, patch_kind::lowres_scratch, {0x89, 0x8C, 0xC4, 0x90, 0x01, 0x00, 0x00}},
   {0x5879f4, 7, 3, patch_kind::lowres_scratch, {0x88, 0x9C, 0xC4, 0x94, 0x01, 0x00, 0x00}},
   {0x5879fc, 8, 4, patch_kind::lowres_scratch, {0x66, 0x89, 0x8C, 0xC4, 0x96, 0x01, 0x00, 0x00}},
   {0x587a70, 7, 3, patch_kind::lowres_scratch, {0x39, 0x94, 0xC4, 0x90, 0x01, 0x00, 0x00}},
   {0x587a79, 7, 3, patch_kind::lowres_scratch, {0x38, 0x9C, 0xC4, 0x94, 0x01, 0x00, 0x00}},
   {0x587ac9, 7, 3, patch_kind::lowres_scratch, {0x89, 0x8C, 0xC4, 0x90, 0x01, 0x00, 0x00}},
   {0x587ad0, 7, 3, patch_kind::lowres_scratch, {0x88, 0x9C, 0xC4, 0x94, 0x01, 0x00, 0x00}},
   {0x587ad8, 8, 4, patch_kind::lowres_scratch, {0x66, 0x89, 0x94, 0xC4, 0x96, 0x01, 0x00, 0x00}},
   {0x587b80, 8, 4, patch_kind::lowres_scratch, {0x0F, 0xB7, 0x84, 0xFC, 0x96, 0x01, 0x00, 0x00}},
   {0x587b93, 7, 3, patch_kind::lowres_scratch, {0x8B, 0x84, 0xFC, 0x90, 0x01, 0x00, 0x00}},
   {0x587ba3, 7, 3, patch_kind::lowres_scratch, {0x8A, 0x8C, 0xFC, 0x94, 0x01, 0x00, 0x00}},
};

const storage_patch kSteamMapPatches[] = {
   {0x63c3f0, 7, 3, patch_kind::registry_address, {0x8D, 0x04, 0xCD, 0x00, 0x97, 0x7E, 0x00}},
   {0x63c980, 7, 3, patch_kind::registry_address, {0x39, 0x0C, 0xC5, 0x00, 0x97, 0x7E, 0x00}},
   {0x63c989, 7, 3, patch_kind::registry_address, {0x39, 0x14, 0xC5, 0x04, 0x97, 0x7E, 0x00}},
   {0x63c9b2, 3, 2, patch_kind::map_capacity, {0x83, 0xF8, 0x1E}},
   {0x4f4511, 6, 2, patch_kind::collision_reader, {0x81, 0xC7, 0xB4, 0x8D, 0x00, 0x00}},
   {0x647596, 6, 2, patch_kind::collision_base, {0x8D, 0x81, 0xF4, 0x8F, 0x00, 0x00}},
   {0x6475a6, 6, 2, patch_kind::collision_base, {0x8D, 0x81, 0x34, 0x91, 0x00, 0x00}},
   {0x647916, 10, 0, patch_kind::collision_clear, {0x8D, 0x04, 0x40, 0xC7, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00}},
   {0x6487f2, 7, 3, patch_kind::lowres_scratch, {0x39, 0xBC, 0xC4, 0x98, 0x01, 0x00, 0x00}},
   {0x6487fb, 7, 3, patch_kind::lowres_scratch, {0x38, 0x8C, 0xC4, 0x9C, 0x01, 0x00, 0x00}},
   {0x648844, 7, 3, patch_kind::lowres_scratch, {0x89, 0xBC, 0xF4, 0x98, 0x01, 0x00, 0x00}},
   {0x64884b, 7, 3, patch_kind::lowres_scratch, {0x88, 0x84, 0xF4, 0x9C, 0x01, 0x00, 0x00}},
   {0x64885b, 8, 4, patch_kind::lowres_scratch, {0x66, 0x89, 0x84, 0xF4, 0x9E, 0x01, 0x00, 0x00}},
   {0x6488b7, 7, 3, patch_kind::lowres_scratch, {0x39, 0xBC, 0xC4, 0x98, 0x01, 0x00, 0x00}},
   {0x6488c0, 7, 3, patch_kind::lowres_scratch, {0x38, 0x8C, 0xC4, 0x9C, 0x01, 0x00, 0x00}},
   {0x6488fb, 7, 3, patch_kind::lowres_scratch, {0x89, 0xBC, 0xF4, 0x98, 0x01, 0x00, 0x00}},
   {0x648902, 7, 3, patch_kind::lowres_scratch, {0x88, 0x84, 0xF4, 0x9C, 0x01, 0x00, 0x00}},
   {0x648912, 8, 4, patch_kind::lowres_scratch, {0x66, 0x89, 0x84, 0xF4, 0x9E, 0x01, 0x00, 0x00}},
   {0x648984, 7, 3, patch_kind::lowres_scratch, {0x39, 0xBC, 0xC4, 0x98, 0x01, 0x00, 0x00}},
   {0x64898d, 7, 3, patch_kind::lowres_scratch, {0x38, 0x8C, 0xC4, 0x9C, 0x01, 0x00, 0x00}},
   {0x6489c6, 7, 3, patch_kind::lowres_scratch, {0x89, 0xBC, 0xF4, 0x98, 0x01, 0x00, 0x00}},
   {0x6489cd, 7, 3, patch_kind::lowres_scratch, {0x88, 0x84, 0xF4, 0x9C, 0x01, 0x00, 0x00}},
   {0x6489dd, 8, 4, patch_kind::lowres_scratch, {0x66, 0x89, 0x84, 0xF4, 0x9E, 0x01, 0x00, 0x00}},
   {0x648ab0, 8, 4, patch_kind::lowres_scratch, {0x0F, 0xB7, 0x84, 0xFC, 0x9E, 0x01, 0x00, 0x00}},
   {0x648ab8, 7, 3, patch_kind::lowres_scratch, {0x8B, 0x94, 0xFC, 0x98, 0x01, 0x00, 0x00}},
   {0x648ac0, 7, 3, patch_kind::lowres_scratch, {0x8A, 0x8C, 0xFC, 0x9C, 0x01, 0x00, 0x00}},
};

const storage_patch kGogMapPatches[] = {
   {0x63d490, 7, 3, patch_kind::registry_address, {0x8D, 0x04, 0xCD, 0xE0, 0xA8, 0x7E, 0x00}},
   {0x63da20, 7, 3, patch_kind::registry_address, {0x39, 0x0C, 0xC5, 0xE0, 0xA8, 0x7E, 0x00}},
   {0x63da29, 7, 3, patch_kind::registry_address, {0x39, 0x14, 0xC5, 0xE4, 0xA8, 0x7E, 0x00}},
   {0x63da52, 3, 2, patch_kind::map_capacity, {0x83, 0xF8, 0x1E}},
   {0x4f4511, 6, 2, patch_kind::collision_reader, {0x81, 0xC7, 0xB4, 0x8D, 0x00, 0x00}},
   {0x648636, 6, 2, patch_kind::collision_base, {0x8D, 0x81, 0xF4, 0x8F, 0x00, 0x00}},
   {0x648646, 6, 2, patch_kind::collision_base, {0x8D, 0x81, 0x34, 0x91, 0x00, 0x00}},
   {0x6489b6, 10, 0, patch_kind::collision_clear, {0x8D, 0x04, 0x40, 0xC7, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00}},
   {0x649892, 7, 3, patch_kind::lowres_scratch, {0x39, 0xBC, 0xC4, 0x98, 0x01, 0x00, 0x00}},
   {0x64989b, 7, 3, patch_kind::lowres_scratch, {0x38, 0x8C, 0xC4, 0x9C, 0x01, 0x00, 0x00}},
   {0x6498e4, 7, 3, patch_kind::lowres_scratch, {0x89, 0xBC, 0xF4, 0x98, 0x01, 0x00, 0x00}},
   {0x6498eb, 7, 3, patch_kind::lowres_scratch, {0x88, 0x84, 0xF4, 0x9C, 0x01, 0x00, 0x00}},
   {0x6498fb, 8, 4, patch_kind::lowres_scratch, {0x66, 0x89, 0x84, 0xF4, 0x9E, 0x01, 0x00, 0x00}},
   {0x649957, 7, 3, patch_kind::lowres_scratch, {0x39, 0xBC, 0xC4, 0x98, 0x01, 0x00, 0x00}},
   {0x649960, 7, 3, patch_kind::lowres_scratch, {0x38, 0x8C, 0xC4, 0x9C, 0x01, 0x00, 0x00}},
   {0x64999b, 7, 3, patch_kind::lowres_scratch, {0x89, 0xBC, 0xF4, 0x98, 0x01, 0x00, 0x00}},
   {0x6499a2, 7, 3, patch_kind::lowres_scratch, {0x88, 0x84, 0xF4, 0x9C, 0x01, 0x00, 0x00}},
   {0x6499b2, 8, 4, patch_kind::lowres_scratch, {0x66, 0x89, 0x84, 0xF4, 0x9E, 0x01, 0x00, 0x00}},
   {0x649a24, 7, 3, patch_kind::lowres_scratch, {0x39, 0xBC, 0xC4, 0x98, 0x01, 0x00, 0x00}},
   {0x649a2d, 7, 3, patch_kind::lowres_scratch, {0x38, 0x8C, 0xC4, 0x9C, 0x01, 0x00, 0x00}},
   {0x649a66, 7, 3, patch_kind::lowres_scratch, {0x89, 0xBC, 0xF4, 0x98, 0x01, 0x00, 0x00}},
   {0x649a6d, 7, 3, patch_kind::lowres_scratch, {0x88, 0x84, 0xF4, 0x9C, 0x01, 0x00, 0x00}},
   {0x649a7d, 8, 4, patch_kind::lowres_scratch, {0x66, 0x89, 0x84, 0xF4, 0x9E, 0x01, 0x00, 0x00}},
   {0x649b50, 8, 4, patch_kind::lowres_scratch, {0x0F, 0xB7, 0x84, 0xFC, 0x9E, 0x01, 0x00, 0x00}},
   {0x649b58, 7, 3, patch_kind::lowres_scratch, {0x8B, 0x94, 0xFC, 0x98, 0x01, 0x00, 0x00}},
   {0x649b60, 7, 3, patch_kind::lowres_scratch, {0x8A, 0x8C, 0xFC, 0x9C, 0x01, 0x00, 0x00}},
};

// clang-format on

// BEGIN bank-capacity sites
const storage_patch kModtoolsBankPatches[] = {
   {0x5703c7, 5, 1, patch_kind::bank_address, {0x05, 0xF8, 0xEC, 0xAC, 0x00}},
   {0x570491, 5, 1, patch_kind::bank_address, {0xB9, 0x18, 0xED, 0xAC, 0x00}},
   {0x57051b, 3, 2, patch_kind::bank_capacity, {0x83, 0xF8, 0x10}},
   {0x581ba2, 7, 3, patch_kind::bank_order, {0x89, 0x84, 0x24, 0x20, 0x01, 0x00, 0x00}},
   {0x581bab, 7, 3, patch_kind::bank_order, {0x8D, 0x84, 0x24, 0x20, 0x01, 0x00, 0x00}},
   {0x581d4b, 7, 3, patch_kind::bank_order, {0x8B, 0x84, 0x24, 0x20, 0x01, 0x00, 0x00}},
   {0x581d94, 7, 3, patch_kind::bank_order, {0x8B, 0x84, 0xBC, 0x20, 0x01, 0x00, 0x00}},
   {0x581d9b, 7, 3, patch_kind::bank_order, {0x8D, 0xB4, 0xBC, 0x20, 0x01, 0x00, 0x00}},
   {0x581c11, 7, 3, patch_kind::bank_map, {0x8D, 0x94, 0x24, 0xD0, 0x0F, 0x00, 0x00}},
   {0x581fd1, 7, 3, patch_kind::bank_map, {0x8B, 0xBC, 0x8C, 0xD0, 0x0F, 0x00, 0x00}},
   {0x58202c, 7, 3, patch_kind::bank_map, {0x8B, 0x84, 0x94, 0xD4, 0x0F, 0x00, 0x00}},
   {0x582e51, 8, 3, patch_kind::bank_map, {0x83, 0xBC, 0x8C, 0xD0, 0x0F, 0x00, 0x00, 0xFF}},
   {0x582eac, 7, 3, patch_kind::bank_map, {0x8B, 0x94, 0x8C, 0xD4, 0x0F, 0x00, 0x00}},
};

const storage_patch kSteamBankPatches[] = {
   {0x63c3c3, 5, 1, patch_kind::bank_address, {0x05, 0x40, 0x94, 0x7E, 0x00}},
   {0x63c43d, 5, 1, patch_kind::bank_address, {0xBA, 0x60, 0x94, 0x7E, 0x00}},
   {0x63c4cb, 3, 2, patch_kind::bank_capacity, {0x83, 0xF8, 0x10}},
   {0x645c45, 6, 2, patch_kind::bank_order, {0x89, 0x85, 0x30, 0xFF, 0xFF, 0xFF}},
   {0x645c50, 6, 2, patch_kind::bank_order, {0x8D, 0x85, 0x30, 0xFF, 0xFF, 0xFF}},
   {0x645c5f, 7, 3, patch_kind::bank_order, {0x89, 0x84, 0x35, 0x34, 0xFF, 0xFF, 0xFF}},
   {0x645c70, 6, 2, patch_kind::bank_order, {0x8D, 0x85, 0x30, 0xFF, 0xFF, 0xFF}},
   {0x645d6c, 6, 2, patch_kind::bank_order, {0x8B, 0x8D, 0x30, 0xFF, 0xFF, 0xFF}},
   {0x645dc0, 7, 3, patch_kind::bank_order, {0x8B, 0x8C, 0x8D, 0x30, 0xFF, 0xFF, 0xFF}},
   {0x645de0, 7, 3, patch_kind::bank_order, {0x3B, 0xBC, 0x85, 0x2C, 0xFF, 0xFF, 0xFF}},
   {0x645cc8, 6, 2, patch_kind::bank_map, {0x8D, 0xBD, 0xB0, 0xEB, 0xFF, 0xFF}},
   {0x645f3a, 7, 3, patch_kind::bank_map, {0x8B, 0x94, 0x85, 0xB0, 0xEB, 0xFF, 0xFF}},
   {0x645f71, 8, 3, patch_kind::bank_map, {0x83, 0xBC, 0x85, 0xB0, 0xEB, 0xFF, 0xFF, 0xFF}},
   {0x6467ed, 8, 3, patch_kind::bank_map, {0x83, 0xBC, 0x85, 0xB0, 0xEB, 0xFF, 0xFF, 0xFF}},
   {0x646821, 8, 3, patch_kind::bank_map, {0x83, 0xBC, 0x85, 0xB0, 0xEB, 0xFF, 0xFF, 0xFF}},
};

const storage_patch kGogBankPatches[] = {
   {0x63d463, 5, 1, patch_kind::bank_address, {0x05, 0x70, 0xA0, 0x7E, 0x00}},
   {0x63d4dd, 5, 1, patch_kind::bank_address, {0xBA, 0x90, 0xA0, 0x7E, 0x00}},
   {0x63d56b, 3, 2, patch_kind::bank_capacity, {0x83, 0xF8, 0x10}},
   {0x646ce5, 6, 2, patch_kind::bank_order, {0x89, 0x85, 0x30, 0xFF, 0xFF, 0xFF}},
   {0x646cf0, 6, 2, patch_kind::bank_order, {0x8D, 0x85, 0x30, 0xFF, 0xFF, 0xFF}},
   {0x646cff, 7, 3, patch_kind::bank_order, {0x89, 0x84, 0x35, 0x34, 0xFF, 0xFF, 0xFF}},
   {0x646d10, 6, 2, patch_kind::bank_order, {0x8D, 0x85, 0x30, 0xFF, 0xFF, 0xFF}},
   {0x646e0c, 6, 2, patch_kind::bank_order, {0x8B, 0x8D, 0x30, 0xFF, 0xFF, 0xFF}},
   {0x646e60, 7, 3, patch_kind::bank_order, {0x8B, 0x8C, 0x8D, 0x30, 0xFF, 0xFF, 0xFF}},
   {0x646e80, 7, 3, patch_kind::bank_order, {0x3B, 0xBC, 0x85, 0x2C, 0xFF, 0xFF, 0xFF}},
   {0x646d68, 6, 2, patch_kind::bank_map, {0x8D, 0xBD, 0xB0, 0xEB, 0xFF, 0xFF}},
   {0x646fda, 7, 3, patch_kind::bank_map, {0x8B, 0x94, 0x85, 0xB0, 0xEB, 0xFF, 0xFF}},
   {0x647011, 8, 3, patch_kind::bank_map, {0x83, 0xBC, 0x85, 0xB0, 0xEB, 0xFF, 0xFF, 0xFF}},
   {0x64788d, 8, 3, patch_kind::bank_map, {0x83, 0xBC, 0x85, 0xB0, 0xEB, 0xFF, 0xFF, 0xFF}},
   {0x6478c1, 8, 3, patch_kind::bank_map, {0x83, 0xBC, 0x85, 0xB0, 0xEB, 0xFF, 0xFF, 0xFF}},
};
// END bank-capacity sites

template<size_t N>
void append_sites(std::vector<storage_patch>& output, const storage_patch (&input)[N], uintptr_t delta = 0)
{
   for (storage_patch patch : input) {
      patch.va += delta;
      output.push_back(patch);
   }
}

std::vector<storage_patch> storage_sites(GameBuild build)
{
   std::vector<storage_patch> sites;
   if (build == GameBuild::Modtools) {
      append_sites(sites, kModtoolsPatches);
      append_sites(sites, kModtoolsMapPatches);
      append_sites(sites, kModtoolsBankPatches);
   }
   else if (build == GameBuild::Steam) {
      append_sites(sites, kRetailPatches);
      append_sites(sites, kSteamMapPatches);
      append_sites(sites, kSteamBankPatches);
   }
   else if (build == GameBuild::GOG) {
      append_sites(sites, kRetailPatches, 0x10A0);
      // The collision consumer is outside the shifted animation-code region.
      append_sites(sites, kGogMapPatches);
      append_sites(sites, kGogBankPatches);
   }
   return sites;
}

uintptr_t registry_address(GameBuild build)
{
   return build == GameBuild::Modtools ? 0x00ACF558
          : build == GameBuild::Steam  ? 0x007E9700
                                       : 0x007EA8E0;
}

uintptr_t bank_registry_address(GameBuild build)
{
   return build == GameBuild::Modtools ? 0x00ACECF8
          : build == GameBuild::Steam  ? 0x007E9440
                                       : 0x007EA070;
}

void storage_expected(const storage_patch& patch, uintptr_t base, GameBuild build, uint8_t* output)
{
   memcpy(output, patch.expected, patch.length);
   // Retail's map and bank registry operands have PE HIGHLOW relocations.
   if ((patch.kind == patch_kind::registry_address || patch.kind == patch_kind::bank_address) &&
       build != GameBuild::Modtools) {
      uint32_t value;
      memcpy(&value, output + patch.operand, sizeof(value));
      value += (uint32_t)(base - kUnrelocatedBase);
      memcpy(output + patch.operand, &value, sizeof(value));
   }
}

void storage_replacement(const storage_patch& patch, GameBuild build, uint8_t* output)
{
   memcpy(output, patch.expected, patch.length);
   uint32_t value = (uint32_t)offsetof(combo_anim_map, custom);
   uint32_t old = 0;
   if (patch.length >= patch.operand + sizeof(old))
      memcpy(&old, patch.expected + patch.operand, sizeof(old));
   unsigned operand = patch.operand;
   switch (patch.kind) {
   case patch_kind::custom_bytes:
      break;
   case patch_kind::custom_dwords:
      value /= sizeof(uint32_t);
      break;
   case patch_kind::cleanup_stride:
      value = sizeof(combo_anim_map);
      break;
   case patch_kind::cleanup_base:
      output[0] = build == GameBuild::Modtools ? 0xBB : 0xBF;
      value = (uint32_t)&g_storage.maps[0].melee;
      operand = 1;
      output[5] = 0x90;
      break;
   case patch_kind::jetpack_animation:
      value = (uint32_t)&g_storage.maps[0].action[25][0];
      if (build == GameBuild::Modtools) {
         output[0] = 0xA1; // MOV EAX,[absolute cell]; NOP
         operand = 1;
         output[5] = 0x90;
      }
      else
         output[1] = 0x35; // PUSH [absolute cell]
      break;
   case patch_kind::registry_address:
      value = (uint32_t)g_mapRegistry + old - (uint32_t)registry_address(build);
      break;
   case patch_kind::bank_address:
      value = (uint32_t)g_bankRegistry + old - (uint32_t)bank_registry_address(build);
      break;
   case patch_kind::bank_capacity:
      static_assert(kComboBankCount < 128); // CMP imm8 sign-extends.
      output[operand] = kComboBankCount;
      return;
   case patch_kind::map_capacity:
      static_assert(kComboMapCount < 128); // CMP imm8 sign-extends.
      output[operand] = kComboMapCount;
      return;
   case patch_kind::collision_reader:
      value = (uint32_t)g_storage.collisions;
      if (build == GameBuild::Modtools) {
         // Drop the owner from LEA [map_offset+owner+8DB4]. The two readers
         // carry map_offset in EAX and ECX respectively. LEA preserves flags.
         output[1] = 0x80 | (patch.expected[2] & 7);
         operand = 2;
         output[6] = 0x90;
      }
      else {
         // ADD EDI,8DB4 -> MOV EDI,absolute base; NOP. A following ADD sets
         // the flags before any conditional consumer in the original code.
         output[0] = 0xBF;
         operand = 1;
         output[5] = 0x90;
      }
      break;
   case patch_kind::collision_base:
      value = (uint32_t)g_storage.collisions + old - 0x8DB4;
      output[0] = 0xB8 | ((patch.expected[1] >> 3) & 7); // LEA -> MOV same register
      operand = 1;
      output[5] = 0x90;
      break;
   case patch_kind::collision_clear: {
      // Retail's third writer indexes from the owner independently of the
      // relocated anchors. EDI is already the current sample's pointer+12.
      const uint8_t clear[] = {0xC7, 0x87, 0xA4, 0x02, 0, 0, 0, 0, 0, 0};
      memcpy(output, clear, sizeof(clear)); // MOV [EDI+2A4],0; flags unchanged
      return;
   }
   case patch_kind::lowres_scratch:
      // [ESP+index*8+local] -> [index*8+absolute field], same instruction size.
      // The stack base is 8-byte aligned in both code families.
      value = (uint32_t)g_lowresScratch + (old & 7);
      output[operand - 2] &= 0x3F; // ModRM: no base-register displacement
      output[operand - 1] = (output[operand - 1] & 0xF8) | 5; // SIB: no base
      break;
   case patch_kind::bank_order:
   case patch_kind::bank_map:
      value = patch.kind == patch_kind::bank_order ? (uint32_t)g_bankOrder : (uint32_t)g_bankWeaponMaps;
      if (patch.kind == patch_kind::bank_order && build != GameBuild::Modtools)
         value += old - 0xFFFFFF30; // Retail also accesses next/previous entries.
      // Debug's FD4 displacement compensates for a pending PUSH; it accesses
      // the same table as FD0. Retail's frame base is EBP and remains fixed.
      if (operand == 3) {
         output[1] &= 0x3F; // Keep opcode/register/index, remove the stack base.
         output[2] = (output[2] & 0xF8) | 5;
      }
      else if (output[0] == 0x8D) {
         output[0] = 0xB8 | ((output[1] >> 3) & 7); // LEA -> MOV register,absolute
         operand = 1;
         output[5] = 0x90;
      }
      else {
         output[1] = (output[1] & 0x38) | 5; // MOV register/[absolute], same size.
      }
      break;
   }
   memcpy(output + operand, &value, sizeof(value));
}

} // namespace

bool combo_anim_limit_active()
{
   return g_installed;
}

int combo_anim_limit_map_count()
{
   return g_installed ? g_storage.map_count : kComboStockMapCount;
}

void* combo_anim_limit_get_body(void* owner, int map, int index, bool lower)
{
   if (!g_installed || !current_owner(owner)) return nullptr;
   return combo_anim_body(g_storage.get(owner, map), index, lower);
}

size_t combo_anim_limit_crash_details(uintptr_t fault, uintptr_t frame, uintptr_t animator,
                                      unsigned movement, char* output, size_t capacity)
{
   if (!output || !capacity) return 0;
   output[0] = '\0';
   if (!g_installed || !g_movementFault || fault != g_movementFault) return 0;

   // EDI is the requested movement only at this caller. At the faulting first
   // dereference, ECX is still SoldierAnimator and EBP identifies its arguments.
   uintptr_t caller = 0;
   if (!read_crash_field(frame, 4, caller) || caller != g_movementCaller) return 0;

   int map = -1, registered = -1, names = -1, references = -1;
   uint32_t stance = 0, state = 0;
   uint8_t secondary = 0;
   uintptr_t entity = 0, instance = 0;
   const bool readable =
      read_crash_field(animator, 0x50, entity) && read_crash_field(animator, 0x70, state) &&
      read_crash_field(animator, 0x74, map) && read_crash_field(animator, 0x1FE8, stance) &&
      // The caller can clear its local secondary flag without updating the
      // animator yet. Its ESP+0x40 is this callee's EBP+0x50 (two arguments,
      // return address and saved EBP); read the actual lookup flag there.
      read_crash_field(frame, 0x50, secondary);
   if (!readable) {
      _snprintf_s(output, capacity,
                  _TRUNCATE, "    [ComboAnimLimit] movement crash: animator=%08X frame=%08X context unreadable\r\n",
                  (unsigned)animator, (unsigned)frame);
      return strlen(output);
   }
   const bool owner_readable = read_crash_field((uintptr_t)g_instance, 0, instance);
   read_crash_field((uintptr_t)g_mapCount, 0, registered);
   read_crash_field((uintptr_t)g_nameCount, 0, names);
   read_crash_field((uintptr_t)g_referenceCount, 0, references);
   const unsigned row = stance % 3 + (secondary ? 3 : 0);
   const int index = movement < 13 ? 38 + row * 13 + movement : -1;
   const bool owner_matches = owner_readable && instance && instance == (uintptr_t)g_storage.owner;
   const combo_anim_map* payload = owner_matches ? g_storage.get((void*)instance, map) : nullptr;
   const char* status = "owner-mismatch";
   uintptr_t upper = 0, lower = 0;
   if (owner_matches) {
      if (!payload)
         status = "invalid-map";
      else if (index < 0)
         status = "invalid-movement";
      else {
         upper = (uintptr_t)payload->movement[row][movement][0];
         lower = (uintptr_t)payload->movement[row][movement][1];
         status = lower == UINTPTR_MAX ? "unassigned-cell"
                  : lower == 0         ? "null-cell"
                                       : "populated-cell";
      }
   }
   _snprintf_s(
      output, capacity, _TRUNCATE,
      "    [ComboAnimLimit] movement crash: animator=%08X entity=%08X state=%u\r\n"
      "      map=%d stance=%u movement=%u secondary=%u index=%d status=%s\r\n"
      "      instance=%08X storage_owner=%08X initialized=%d registered=%d/%d initializing=%u\r\n"
      "      raw_upper=%08X raw_lower=%08X names=%d/%d references=%d/%d\r\n",
      (unsigned)animator, (unsigned)entity, state, map, stance, movement, (unsigned)secondary,
      index, status, (unsigned)instance, (unsigned)(uintptr_t)g_storage.owner, g_storage.map_count,
      registered, kComboMapCount, (unsigned)g_initializing, (unsigned)upper, (unsigned)lower, names,
      kComboAnimationCount, references, kComboReferenceCount);
   return strlen(output);
}

bool combo_anim_limit_install(uintptr_t base)
{
   if (g_installed) return true;
   const function_site* functions = nullptr;
   const auto patches = storage_sites(g_build);
   const size_t patch_count = patches.size();
   uintptr_t delta = 0;
   uintptr_t instance = 0, maps = 0, names = 0, references = 0, banks = 0;
   switch (g_build) {
   case GameBuild::Modtools:
      functions = kModtoolsFunctions;
      instance = 0x00B8D3C4;
      maps = 0x00ACECF4;
      banks = 0x00ACECE8;
      names = 0x00B8D088;
      references = 0x00B8D08C;
      break;
   case GameBuild::GOG:
      delta = 0x10A0;
      instance = 0x01EB0FD0;
      maps = 0x007EA068;
      banks = 0x007EA04C;
      names = 0x01EB0548;
      references = 0x01EB054C;
      [[fallthrough]];
   case GameBuild::Steam:
      functions = kRetailFunctions;
      if (!delta) {
         instance = 0x01EAFB1C;
         maps = 0x007E906C;
         banks = 0x007E9430;
         names = 0x01EAF094;
         references = 0x01EAF09C;
      }
      break;
   default:
      return false;
   }

   // The enclosing patch set already validated the registry/metadata/sentinel
   // operands. Validate every storage instruction and hook before changing either.
   for (int i = 0; i < 13; ++i) {
      if (!functions[i].va) continue;
      void* entry = resolve(base, functions[i].va + delta);
      if (memcmp(entry, functions[i].expected, sizeof(functions[i].expected))) {
         install_log("[ComboAnimLimit] NOT installed: hook signature mismatch at %08X",
                     (unsigned)(functions[i].va + delta));
         return false;
      }
   }
   for (size_t i = 0; i < patch_count; ++i) {
      const storage_patch& patch = patches[i];
      uint8_t expected[11];
      storage_expected(patch, base, g_build, expected);
      if (memcmp(resolve(base, patch.va), expected, patch.length)) {
         install_log("[ComboAnimLimit] NOT installed: storage signature mismatch at %08X",
                     (unsigned)patch.va);
         return false;
      }
   }

   LONG error = DetourTransactionBegin();
   if (error != NO_ERROR) return false;
   error = DetourUpdateThread(GetCurrentThread());
   for (int i = 0; i < 13 && error == NO_ERROR; ++i) {
      if (!functions[i].va) continue;
      g_originals[i] = resolve(base, functions[i].va + delta);
      error = DetourAttach(&g_originals[i], functions[i].hook);
   }
   if (error != NO_ERROR) {
      DetourTransactionAbort();
      memset(g_originals, 0, sizeof(g_originals));
      install_log("[ComboAnimLimit] NOT installed: Detours preparation failed (%ld)", error);
      return false;
   }

   // Only plain writes remain after the Detours commit. No engine code runs
   // during this install window. On a failed commit restore every storage byte.
   for (size_t i = 0; i < patch_count; ++i) {
      const storage_patch& patch = patches[i];
      uint8_t replacement[11];
      storage_replacement(patch, g_build, replacement);
      memcpy(resolve(base, patch.va), replacement, patch.length);
   }
   error = DetourTransactionCommit();
   if (error != NO_ERROR) {
      for (const storage_patch& patch : patches) {
         uint8_t expected[11];
         storage_expected(patch, base, g_build, expected);
         memcpy(resolve(base, patch.va), expected, patch.length);
      }
      memset(g_originals, 0, sizeof(g_originals));
      install_log("[ComboAnimLimit] NOT installed: Detours commit failed (%ld); storage restored", error);
      return false;
   }

   // Keep the original five built-in mappings. The game's existing reset sets
   // only the count back to five; later registrations overwrite the other rows.
   memcpy(g_mapRegistry, resolve(base, registry_address(g_build)),
          kComboStockMapCount * sizeof(combo_map_key));
   memcpy(g_bankRegistry, resolve(base, bank_registry_address(g_build)),
          kComboStockBankCount * sizeof(combo_anim_bank));
   g_instance = (void**)resolve(base, instance);
   g_mapCount = (int*)resolve(base, maps);
   g_bankCount = (int*)resolve(base, banks);
   g_nameCount = (int*)resolve(base, names);
   g_referenceCount = (int*)resolve(base, references);
   if (g_build == GameBuild::Modtools) {
      g_movementFault = (uintptr_t)resolve(base, 0x0057906D);
      g_movementCaller = (uintptr_t)resolve(base, 0x0057BBE3);
   }
   g_installed = true;
   FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
   install_log("[ComboAnimLimit] storage installed: %d names, indices 0-%d, "
               "%u-byte maps, %d runtime + %d temporary maps, %d bank names",
               kComboAnimationCount, kComboAnimationEnd - 1, (unsigned)sizeof(combo_anim_map),
               kComboMapCount, kComboCacheCount, kComboBankCount);
   return true;
}
