#include "pch.h"
#include "foleyfx_region_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

// See the header.

namespace {

constexpr uintptr_t kRegion_Group = 0x60; // FoleyFXGroup* mGroup
constexpr uintptr_t kRegion_Node  = 0x64; // smList node; its first dword is `next`

// FoleyFXGroup: +0 mID (PblHash of its Name), +4 list of FoleyFX (circular,
// node = {next, FoleyFX*}, head at +4), +8 node in FoleyFXGroup::smList.
constexpr uintptr_t kGroup_ID   = 0x00;
constexpr uintptr_t kGroup_FX   = 0x04;
constexpr uintptr_t kGroup_Node = 0x08;
// FoleyFX::GetFoleyFXClassID is `MOV EAX,[ECX+8]` on every build.
constexpr uintptr_t kFoleyFX_Class = 0x08;

// bool __thiscall PblRegion::IsPointInside(PblVector3*), RET 4, result in AL.
using fn_is_point_inside_t = bool(__fastcall*)(void* region, void* edx, const void* pos);
// FoleyFXGroup* __cdecl FoleyFXGroup::GetTerrainFX()
using fn_get_terrain_fx_t = void*(__cdecl*)();
// FoleyFX* __thiscall FoleyFXGroup::FindFoleyFX(uint classId), RET 4.
using fn_find_foleyfx_t = void*(__fastcall*)(void* group, void* edx, uint32_t classId);

fn_is_point_inside_t g_isPointInside = nullptr;
fn_get_terrain_fx_t  g_getTerrainFX  = nullptr;
fn_find_foleyfx_t    g_findFoleyFX   = nullptr;
void**               g_listHead      = nullptr; // FoleyFXRegion::smList
void**               g_groupHead     = nullptr; // FoleyFXGroup::smList

struct CallPatch {
   uint8_t* relSite = nullptr; // the rel32 operand, i.e. CALL opcode + 1
   int32_t  relOrig = 0;
};
CallPatch s_terrainCall;
CallPatch s_findCall;

// ---------------------------------------------------------------------------
// Same-name groups
// ---------------------------------------------------------------------------

// The most recently loaded group with the same name as `group`: the one the
// loader would leave in smTerrain/smWater, since each Read overwrites those.
void* latest_copy(void* group)
{
   if (!group || !g_groupHead) return group;
   const uint32_t id = *(const uint32_t*)((char*)group + kGroup_ID);
   void* latest = group;
   for (void** n = (void**)*g_groupHead; n && n != g_groupHead; n = (void**)*n) {
      void* g = (char*)n - kGroup_Node;
      if (*(const uint32_t*)((char*)g + kGroup_ID) == id) latest = g;
   }
   return latest;
}

// `group`'s own entry for exactly `classId`, or null.
void* exact_entry(void* group, uint32_t classId)
{
   void** head = (void**)((char*)group + kGroup_FX);
   for (void** n = (void**)*head; n && n != head; n = (void**)*n) {
      void* fx = n[1];
      if (fx && *(const uint32_t*)((char*)fx + kFoleyFX_Class) == classId) return fx;
   }
   return nullptr;
}

// FindFoleyFX with gaps filled: whatever the group itself answers (its exact
// entry, or its class-0 fallback) stands; only when it has nothing does the
// most recently loaded same-name copy that has this class supply it.
void* __cdecl find_foleyfx_filled(void* group, uint32_t classId)
{
   void* fx = g_findFoleyFX(group, nullptr, classId);
   if (fx || !group || classId == 0 || !g_groupHead) return fx;

   const uint32_t id = *(const uint32_t*)((char*)group + kGroup_ID);
   for (void** n = (void**)*g_groupHead; n && n != g_groupHead; n = (void**)*n) {
      void* g = (char*)n - kGroup_Node;
      if (g == group || *(const uint32_t*)((char*)g + kGroup_ID) != id) continue;
      if (void* e = exact_entry(g, classId)) fx = e; // keep the latest match
   }
   return fx;
}

// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------

// The collider's foley class ID: FoleyFXCollider +0 is its FoleyFXColliderClass,
// whose ID is at +0 (GetFoleyFXClassID is `MOV EAX,[ECX]` on every build).
uint32_t collider_class_id(const void* collider)
{
   const void* cls = collider ? *(void* const*)collider : nullptr;
   return cls ? *(const uint32_t*)cls : 0;
}

// The group for a terrain collision at `pos`: the first region containing it
// that has a group, else the stock terrain group. A region's group is swapped
// for the latest copy of its name, the same rule the terrain group follows;
// the engine resolved it with FindByID, which returns the FIRST copy.
//
// A region whose group has no sound for this collider's class (even after the
// same-name fill-in) is treated as absent, so the unit gets the ground sound.
// Otherwise SetupFoleyFX would keep whatever the unit had last, which may be
// the sound of the crate or vehicle it just stepped off.
void* __cdecl terrain_group_for(const void* pos, const void* collider)
{
   if (pos) {
      const uint32_t classId = collider_class_id(collider);
      for (void** node = (void**)*g_listHead; node && node != g_listHead; node = (void**)*node) {
         void* region = (char*)node - kRegion_Node;
         void* group  = *(void**)((char*)region + kRegion_Group);
         if (!group || !g_isPointInside(region, nullptr, pos)) continue;

         group = latest_copy(group);
         if (classId && g_findFoleyFX && !find_foleyfx_filled(group, classId)) break;
         return group;
      }
   }
   return g_getTerrainFX();
}

// ---------------------------------------------------------------------------
// Call-site stubs
//
// Both replaced callees are tiny leaves on the LTCG retail builds (GetTerrainFX
// is `MOV EAX,[smTerrain]; RET`; FindFoleyFX clobbers only EAX/ECX/EDX), and
// LTCG may rely on that. So every stub returns only EAX and puts back ECX, EDX
// and XMM0-7, which IsPointInside and the C code above are free to use.
// ---------------------------------------------------------------------------

#define SAVE_XMM                                 \
   __asm sub  esp, 0x80                          \
   __asm movups [esp + 0x00], xmm0               \
   __asm movups [esp + 0x10], xmm1               \
   __asm movups [esp + 0x20], xmm2               \
   __asm movups [esp + 0x30], xmm3               \
   __asm movups [esp + 0x40], xmm4               \
   __asm movups [esp + 0x50], xmm5               \
   __asm movups [esp + 0x60], xmm6               \
   __asm movups [esp + 0x70], xmm7

#define RESTORE_XMM                              \
   __asm movups xmm0, [esp + 0x00]               \
   __asm movups xmm1, [esp + 0x10]               \
   __asm movups xmm2, [esp + 0x20]               \
   __asm movups xmm3, [esp + 0x30]               \
   __asm movups xmm4, [esp + 0x40]               \
   __asm movups xmm5, [esp + 0x50]               \
   __asm movups xmm6, [esp + 0x60]               \
   __asm movups xmm7, [esp + 0x70]               \
   __asm add  esp, 0x80

// Stand-ins for `CALL GetTerrainFX` in FoleyFXCollider::CollisionCallback, where
// the collision position and the FoleyFXCollider live in registers: position in
// ESI and collider in EDI on modtools, the other way round on retail.
#define TERRAIN_SHIM_BODY(posReg, colliderReg)   \
   __asm push ecx                                \
   __asm push edx                                \
   SAVE_XMM                                      \
   __asm push colliderReg                        \
   __asm push posReg                             \
   __asm call terrain_group_for                  \
   __asm add  esp, 8                             \
   RESTORE_XMM                                   \
   __asm pop  edx                                \
   __asm pop  ecx                                \
   __asm ret

__declspec(naked) void terrain_group_shim_modtools() { TERRAIN_SHIM_BODY(esi, edi) }
__declspec(naked) void terrain_group_shim_retail()   { TERRAIN_SHIM_BODY(edi, esi) }

// Stand-in for `CALL FoleyFXGroup::FindFoleyFX` in SetupFoleyFX: thiscall, group
// in ECX, classId on the stack, RET 4.
__declspec(naked) void find_foleyfx_shim()
{
   __asm push ebp
   __asm mov  ebp, esp
   __asm push ecx
   __asm push edx
   SAVE_XMM
   __asm push dword ptr [ebp + 8] // classId
   __asm push ecx                 // group
   __asm call find_foleyfx_filled
   __asm add  esp, 8
   RESTORE_XMM
   __asm pop  edx
   __asm pop  ecx
   __asm pop  ebp
   __asm ret  4
}

#undef TERRAIN_SHIM_BODY
#undef RESTORE_XMM
#undef SAVE_XMM

// ---------------------------------------------------------------------------
// Install helpers
// ---------------------------------------------------------------------------

// Follow one `JMP rel32` (an incremental-link thunk) if `p` starts with one.
uintptr_t skip_thunk(uintptr_t p)
{
   const uint8_t* b = (const uint8_t*)p;
   if (*b != 0xE9) return p;
   return p + 5 + (intptr_t)*(const int32_t*)(b + 1);
}

// Point the `CALL rel32` at `site` at `shim`, after checking it currently calls
// `callee` (directly or through its incremental-link thunk). Anything else
// means the address moved, and the site is left alone.
bool retarget_call(uintptr_t exe_base, uintptr_t site, uintptr_t callee, void* shim, CallPatch& out)
{
   uint8_t* const call = (uint8_t*)resolve(exe_base, site);
   if (*call != 0xE8) {
      install_log("[FoleyFXRegion] site %08X reads %02X, expected E8 -- left stock",
                  (unsigned)site, *call);
      return false;
   }

   const uintptr_t expected = (uintptr_t)resolve(exe_base, callee);
   const int32_t   relOrig  = *(int32_t*)(call + 1);
   const uintptr_t actual   = (uintptr_t)(call + 5) + (intptr_t)relOrig;
   if (actual != expected && skip_thunk(actual) != expected) {
      install_log("[FoleyFXRegion] site %08X targets %p, expected %p -- left stock",
                  (unsigned)site, (void*)actual, (void*)expected);
      return false;
   }

   out.relSite = call + 1;
   out.relOrig = relOrig;
   *(int32_t*)out.relSite = (int32_t)((intptr_t)shim - (intptr_t)(call + 5));
   return true;
}

void restore_call(CallPatch& p)
{
   // Sections are re-protected by now, so this cannot be a plain store.
   if (p.relSite) protected_write(p.relSite, &p.relOrig, sizeof(p.relOrig));
   p.relSite = nullptr;
}

} // namespace

void foleyfx_region_reset()
{
   if (g_addr->foleyfx_region_list == 0) return; // not derived for this build

   // The list head is a single pointer; an empty list points at itself, which is
   // also its static initial value on every build.
   void** head = (void**)resolve((uintptr_t)GetModuleHandleW(nullptr), g_addr->foleyfx_region_list);
   *head = head;
}

void foleyfx_region_install(uintptr_t exe_base)
{
   if (g_addr->foleyfx_group_list)
      g_groupHead = (void**)resolve(exe_base, g_addr->foleyfx_group_list);

   // Same-name fill-in. Independent of regions: it also serves the terrain,
   // water and object groups.
   if (g_groupHead && g_addr->foleyfx_setup_find_call && g_addr->foleyfx_group_find_foleyfx) {
      g_findFoleyFX = (fn_find_foleyfx_t)resolve(exe_base, g_addr->foleyfx_group_find_foleyfx);
      retarget_call(exe_base, g_addr->foleyfx_setup_find_call, g_addr->foleyfx_group_find_foleyfx,
                    (void*)&find_foleyfx_shim, s_findCall);
   }

   if (g_addr->foleyfx_terrain_group_call == 0 || g_addr->foleyfx_get_terrain_fx == 0 ||
       g_addr->pbl_region_is_point_inside == 0 || g_addr->foleyfx_region_list == 0)
      return; // lookup not derived for this build; the crash fix still runs

   g_getTerrainFX  = (fn_get_terrain_fx_t)resolve(exe_base, g_addr->foleyfx_get_terrain_fx);
   g_isPointInside = (fn_is_point_inside_t)resolve(exe_base, g_addr->pbl_region_is_point_inside);
   g_listHead      = (void**)resolve(exe_base, g_addr->foleyfx_region_list);

   // Where CollisionCallback keeps the position and collider at the call site.
   void (*shim)() = g_build == GameBuild::Modtools ? terrain_group_shim_modtools : terrain_group_shim_retail;
   retarget_call(exe_base, g_addr->foleyfx_terrain_group_call, g_addr->foleyfx_get_terrain_fx,
                 (void*)shim, s_terrainCall);
}

void foleyfx_region_uninstall()
{
   restore_call(s_terrainCall);
   restore_call(s_findCall);
}
