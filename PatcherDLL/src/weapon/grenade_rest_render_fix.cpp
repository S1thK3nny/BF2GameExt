#include "pch.h"
#include "grenade_rest_render_fix.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"
#include "util/install_log.hpp"
#include "game/Battlefront2/Source/OrdnanceGrenade.h"
#include "game/Battlefront2/Source/OrdnanceSticky.h"
#include "game/PebbleFL/Common/PblVector.h"

#include <detours.h>
#include <math.h>
#include <unordered_map>

// See grenade_rest_render_fix.hpp.

// thiscall, RET 0xC on every build.
typedef void (__fastcall* fn_Render_t)(void* ecx, void* edx, uint32_t lod, float f, uint32_t flags);
// thiscall(class, arg), RET 8 on every build.
typedef void* (__fastcall* fn_Ctor_t)(void* ecx, void* edx, void* cls, void* arg);

static fn_Render_t g_origGrenadeRender = nullptr;
static fn_Ctor_t   g_origGrenadeCtor   = nullptr;
static fn_Render_t g_origStickyRender  = nullptr;
static fn_Ctor_t   g_origStickyCtor    = nullptr;

// Ordnance offsets, the same on every build.
static constexpr uint32_t kOrd_Class          = 0x30;
static constexpr uint32_t kOrd_DisplayableOff = 0x98;  // Render's `this`
static constexpr uint32_t kOrd_Velocity       = 0xFC;

// Squared speed treated as standing still.
static constexpr float kStillSq = 1e-12f;

// Last direction each ordnance was drawn along, keyed by ordnance.
static std::unordered_map<const void*, PblVector3> g_lastDir;

// Draws with the last moving direction in place of a zero velocity.
static void render_along_last_dir(fn_Render_t orig, char* ord, void* ecx, void* edx,
                                  uint32_t lod, float f, uint32_t flags)
{
   PblVector3* const vel = (PblVector3*)(ord + kOrd_Velocity);
   const float sq = vel->x * vel->x + vel->y * vel->y + vel->z * vel->z;

   if (sq > kStillSq) {
      const float inv = 1.0f / sqrtf(sq);
      g_lastDir[ord] = { vel->x * inv, vel->y * inv, vel->z * inv };
      orig(ecx, edx, lod, f, flags);
      return;
   }

   const auto it = g_lastDir.find(ord);
   const PblVector3 saved = *vel;
   *vel = it != g_lastDir.end() ? it->second : PblVector3{ 0.0f, 0.0f, 1.0f };
   orig(ecx, edx, lod, f, flags);
   *vel = saved;
}

static void __fastcall hooked_GrenadeRender(void* ecx, void* edx, uint32_t lod, float f, uint32_t flags)
{
   char* const ord = (char*)ecx - kOrd_DisplayableOff;
   const void* cls = *(void**)(ord + kOrd_Class);

   if (!cls || layout::OrdnanceGrenadeClass::mAlignVertical(cls)) {
      g_origGrenadeRender(ecx, edx, lod, f, flags);
      return;
   }
   render_along_last_dir(g_origGrenadeRender, ord, ecx, edx, lod, f, flags);
}

static void __fastcall hooked_StickyRender(void* ecx, void* edx, uint32_t lod, float f, uint32_t flags)
{
   char* const ord = (char*)ecx - kOrd_DisplayableOff;
   const void* cls = *(void**)(ord + kOrd_Class);

   // Stuck ones draw on the stuck pose, not the velocity.
   if (!cls || layout::OrdnanceStickyClass::mAlignVertical(cls) ||
       layout::OrdnanceSticky::mStickInfo(ord)) {
      g_origStickyRender(ecx, edx, lod, f, flags);
      return;
   }
   render_along_last_dir(g_origStickyRender, ord, ecx, edx, lod, f, flags);
}

static void* __fastcall hooked_GrenadeCtor(void* ecx, void* edx, void* cls, void* arg)
{
   g_lastDir.erase(ecx);
   return g_origGrenadeCtor(ecx, edx, cls, arg);
}

static void* __fastcall hooked_StickyCtor(void* ecx, void* edx, void* cls, void* arg)
{
   g_lastDir.erase(ecx);
   return g_origStickyCtor(ecx, edx, cls, arg);
}

struct HookPair {
   const char*  name;
   uintptr_t    render_addr, ctor_addr;
   fn_Render_t* orig_render;
   fn_Ctor_t*   orig_ctor;
   PVOID        hook_render, hook_ctor;
};

static void install_pair(uintptr_t exe_base, const HookPair& h)
{
   if (h.render_addr == 0 || h.ctor_addr == 0) {
      install_log("[GrenadeRestRender] %s skipped: no address on this build", h.name);
      return;
   }

   *h.orig_render = reinterpret_cast<fn_Render_t>(resolve(exe_base, h.render_addr));
   *h.orig_ctor   = reinterpret_cast<fn_Ctor_t>(resolve(exe_base, h.ctor_addr));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG rr = DetourAttach(reinterpret_cast<PVOID*>(h.orig_render), h.hook_render);
   const LONG rk = DetourAttach(reinterpret_cast<PVOID*>(h.orig_ctor), h.hook_ctor);
   const LONG rc = DetourTransactionCommit();

   if (rr != NO_ERROR || rk != NO_ERROR || rc != NO_ERROR) {
      *h.orig_render = nullptr;
      *h.orig_ctor   = nullptr;
      install_log("[GrenadeRestRender] %s detour failed (%ld/%ld/%ld)", h.name, rr, rk, rc);
      return;
   }
   install_log("[GrenadeRestRender] %s installed (render %08X, ctor %08X)",
               h.name, (unsigned)h.render_addr, (unsigned)h.ctor_addr);
}

static void uninstall_pair(const HookPair& h)
{
   if (!*h.orig_render) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(reinterpret_cast<PVOID*>(h.orig_render), h.hook_render);
   DetourDetach(reinterpret_cast<PVOID*>(h.orig_ctor), h.hook_ctor);
   DetourTransactionCommit();
   *h.orig_render = nullptr;
   *h.orig_ctor   = nullptr;
}

static HookPair grenade_pair()
{
   return { "grenade", g_addr->ordnance_grenade_render, g_addr->ordnance_grenade_ctor,
            &g_origGrenadeRender, &g_origGrenadeCtor,
            reinterpret_cast<PVOID>(hooked_GrenadeRender), reinterpret_cast<PVOID>(hooked_GrenadeCtor) };
}

static HookPair sticky_pair()
{
   return { "sticky", g_addr->ordnance_sticky_render, g_addr->ordnance_sticky_ctor,
            &g_origStickyRender, &g_origStickyCtor,
            reinterpret_cast<PVOID>(hooked_StickyRender), reinterpret_cast<PVOID>(hooked_StickyCtor) };
}

void grenade_rest_render_fix_install(uintptr_t exe_base)
{
   install_pair(exe_base, grenade_pair());
   install_pair(exe_base, sticky_pair());
}

void grenade_rest_render_fix_uninstall()
{
   uninstall_pair(grenade_pair());
   uninstall_pair(sticky_pair());
   g_lastDir.clear();
}
