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

// bool __thiscall PblRegion::IsPointInside(PblVector3*), RET 4, result in AL.
using fn_is_point_inside_t = bool(__fastcall*)(void* region, void* edx, const void* pos);
// FoleyFXGroup* __cdecl FoleyFXGroup::GetTerrainFX()
using fn_get_terrain_fx_t = void*(__cdecl*)();

fn_is_point_inside_t g_isPointInside = nullptr;
fn_get_terrain_fx_t  g_getTerrainFX  = nullptr;
void**               g_listHead      = nullptr;

uint8_t* s_relSite = nullptr; // the rel32 operand, i.e. CALL opcode + 1
int32_t  s_relOrig = 0;

// The group for a terrain collision at `pos`: the first region containing it
// that has a group, else the stock terrain group.
void* __cdecl terrain_group_for(const void* pos)
{
   if (pos) {
      for (void** node = (void**)*g_listHead; node && node != g_listHead; node = (void**)*node) {
         void* region = (char*)node - kRegion_Node;
         void* group  = *(void**)((char*)region + kRegion_Group);
         if (group && g_isPointInside(region, nullptr, pos)) return group;
      }
   }
   return g_getTerrainFX();
}

// Stand-ins for `CALL GetTerrainFX` inside FoleyFXCollider::CollisionCallback,
// where the collision position lives in a register rather than an argument:
// ESI on modtools, EDI on the retail builds.
//
// GetTerrainFX is `MOV EAX,[smTerrain]; RET` and the retail builds are LTCG,
// which may rely on a leaf like that leaving every other register alone. So the
// stubs return only EAX and put back ECX, EDX and XMM0-7, which IsPointInside
// and the C code above are free to use.
#define FOLEY_SHIM_BODY(posReg)                  \
   __asm push ecx                                \
   __asm push edx                                \
   __asm sub  esp, 0x80                          \
   __asm movups [esp + 0x00], xmm0               \
   __asm movups [esp + 0x10], xmm1               \
   __asm movups [esp + 0x20], xmm2               \
   __asm movups [esp + 0x30], xmm3               \
   __asm movups [esp + 0x40], xmm4               \
   __asm movups [esp + 0x50], xmm5               \
   __asm movups [esp + 0x60], xmm6               \
   __asm movups [esp + 0x70], xmm7               \
   __asm push posReg                             \
   __asm call terrain_group_for                  \
   __asm add  esp, 4                             \
   __asm movups xmm0, [esp + 0x00]               \
   __asm movups xmm1, [esp + 0x10]               \
   __asm movups xmm2, [esp + 0x20]               \
   __asm movups xmm3, [esp + 0x30]               \
   __asm movups xmm4, [esp + 0x40]               \
   __asm movups xmm5, [esp + 0x50]               \
   __asm movups xmm6, [esp + 0x60]               \
   __asm movups xmm7, [esp + 0x70]               \
   __asm add  esp, 0x80                          \
   __asm pop  edx                                \
   __asm pop  ecx                                \
   __asm ret

__declspec(naked) void terrain_group_shim_esi() { FOLEY_SHIM_BODY(esi) }
__declspec(naked) void terrain_group_shim_edi() { FOLEY_SHIM_BODY(edi) }

#undef FOLEY_SHIM_BODY

// Follow one `JMP rel32` (an incremental-link thunk) if `p` starts with one.
uintptr_t skip_thunk(uintptr_t p)
{
   const uint8_t* b = (const uint8_t*)p;
   if (*b != 0xE9) return p;
   return p + 5 + (intptr_t)*(const int32_t*)(b + 1);
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
   if (g_addr->foleyfx_terrain_group_call == 0 || g_addr->foleyfx_get_terrain_fx == 0 ||
       g_addr->pbl_region_is_point_inside == 0 || g_addr->foleyfx_region_list == 0)
      return; // lookup not derived for this build; the crash fix still runs

   uint8_t* const call = (uint8_t*)resolve(exe_base, g_addr->foleyfx_terrain_group_call);

   // It must be `CALL rel32` aimed at GetTerrainFX (directly or through its
   // incremental-link thunk). Anything else means the address moved.
   if (*call != 0xE8) {
      install_log("[FoleyFXRegion] site %08X reads %02X, expected E8 -- regions left inert",
                  (unsigned)g_addr->foleyfx_terrain_group_call, *call);
      return;
   }

   const uintptr_t expected = (uintptr_t)resolve(exe_base, g_addr->foleyfx_get_terrain_fx);
   const int32_t   relOrig  = *(int32_t*)(call + 1);
   const uintptr_t actual   = (uintptr_t)(call + 5) + (intptr_t)relOrig;

   if (actual != expected && skip_thunk(actual) != expected) {
      install_log("[FoleyFXRegion] site %08X targets %p, expected %p -- regions left inert",
                  (unsigned)g_addr->foleyfx_terrain_group_call, (void*)actual, (void*)expected);
      return;
   }

   g_getTerrainFX  = (fn_get_terrain_fx_t)expected;
   g_isPointInside = (fn_is_point_inside_t)resolve(exe_base, g_addr->pbl_region_is_point_inside);
   g_listHead      = (void**)resolve(exe_base, g_addr->foleyfx_region_list);

   // Where CollisionCallback keeps the collision position at the call site.
   void (*shim)() = g_build == GameBuild::Modtools ? terrain_group_shim_esi : terrain_group_shim_edi;

   s_relSite = call + 1;
   s_relOrig = relOrig;
   *(int32_t*)s_relSite = (int32_t)((intptr_t)shim - (intptr_t)(call + 5));
}

void foleyfx_region_uninstall()
{
   // Sections are re-protected by now, so this cannot be a plain store.
   if (s_relSite) protected_write(s_relSite, &s_relOrig, sizeof(s_relOrig));
   s_relSite = nullptr;
}
