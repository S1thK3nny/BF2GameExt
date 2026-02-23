#include "pch.h"
#include "lua_hooks.hpp"
#include "lua_funcs.hpp"

#include <detours.h>

lua_api g_lua = {};
lua_State* g_L = nullptr;
bool g_useBarrelFireOrigin = false;

// ---------------------------------------------------------------------------
// Barrel fire origin — WeaponCannon OverrideAimer vtable hook
// ---------------------------------------------------------------------------

// Vtable slot + original/hook pointers — accessible from lua_funcs.cpp for live toggling
void** g_cannonOverrideAimerSlot = nullptr;
void*  g_cannonOverrideAimerOrig = nullptr;
void*  g_cannonOverrideAimerHook = nullptr;

// Weapon::ZoomFirstPerson — resolved at install time
typedef bool(__thiscall* ZoomFirstPerson_t)(void* weapon);
static ZoomFirstPerson_t fn_ZoomFirstPerson = nullptr;

// Replacement for WeaponCannon::OverrideAimer (vtable slot 0x70).
// When enabled, reads the barrel fire point matrix translation from the Weapon
// and writes it to the Aimer's mFirePos. Falls back to vanilla aimer position
// when the matrix is stale (first-person zoom) or reflected (water).
static bool __fastcall hooked_cannon_OverrideAimer(void* weapon, void* /*edx*/)
{
   if (!g_useBarrelFireOrigin) return false;

   // Zoom detection: revert to vanilla aimer only during first-person zoom.
   // mIsAiming (owner+0x160): runtime zoomed state.
   // mIsFirstPersonView: Controllable+0x34 (mTracker ptr) → Tracker+0x14.
   // Only skip barrel fire when both zoomed AND in first-person view.
   void* owner = *(void**)((char*)weapon + 0x6C);
   if (owner) {
      bool isZoomed = *(bool*)((char*)owner + 0x160);
      if (isZoomed) {
         void* tracker = *(void**)((char*)owner + 0x34);
         if (tracker) {
            bool isFirstPerson = *(bool*)((char*)tracker + 0x14);
            if (isFirstPerson) return false;
         }
      }
   }

   __try {
      void* aimer = *(void**)((char*)weapon + 0x70);   // Weapon::mAimer
      if (!aimer) return false;

      // Weapon::mFirePointMatrix at weapon+0x20 (PblMatrix, 0x40 bytes).
      // PblMatrix::trans row is at offset 0x30 — the world-space fire position.
      float* trans = (float*)((char*)weapon + 0x20 + 0x30);

      // Validate: check for uninitialized (0xCDCDCDCD) or zero
      const uint32_t raw = *(uint32_t*)&trans[0];
      if (raw == 0xCDCDCDCD ||
          (trans[0] == 0.0f && trans[1] == 0.0f && trans[2] == 0.0f))
         return false;

      float* aimerFirePos = (float*)((char*)aimer + 0x88);  // Aimer::mFirePos
      float* rootPos      = (float*)((char*)aimer + 0x70);  // Aimer::mRootPos

      // Water reflection check: the rendering reflection pass can mirror
      // mFirePointMatrix Y across the water plane. Normal barrel-to-root
      // Y delta is ~3 units; reflected can be 18+. If reflected, clamp Y
      // to a reasonable offset from rootPos rather than skipping entirely.
      float fireY = trans[1];
      float barrelRootDy = fireY - rootPos[1];
      if (barrelRootDy < -5.0f || barrelRootDy > 5.0f) {
         fireY = rootPos[1] - 2.7f;
      }

      // Write to both mFirePos and mFirePointMatrix trans.
      // Muzzle flash reads mFirePos; projectile system may read the matrix.
      aimerFirePos[0] = trans[0];
      aimerFirePos[1] = fireY;
      aimerFirePos[2] = trans[2];

      trans[0] = aimerFirePos[0];
      trans[1] = fireY;
      trans[2] = aimerFirePos[2];
      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

static const uintptr_t unrelocated_base = 0x400000;

static void* resolve(uintptr_t exe_base, uintptr_t unrelocated_addr)
{
   return (void*)((unrelocated_addr - unrelocated_base) + exe_base);
}

using fn_init_state = void(__cdecl*)();
static fn_init_state original_init_state = nullptr;

static void __cdecl hooked_init_state()
{
   original_init_state();

   // Strategy B: read gLuaState_Pointer now that InitState has returned
   lua_State** p_lua_state = (lua_State**)((lua_addrs::modtools::g_lua_state_ptr - 0x400000) +
                                           (uintptr_t)GetModuleHandleW(nullptr));
   g_L = *p_lua_state;

   if (g_L)
      register_lua_functions(g_L);
}

void lua_register_func(lua_State* L, const char* name, lua_CFunction fn)
{
   if (not g_lua.pushlstring or not g_lua.pushcclosure or not g_lua.settable) return;

   // Lua 5.0: push name + closure, then settable into LUA_GLOBALSINDEX
   g_lua.pushlstring(L, name, strlen(name));
   g_lua.pushcclosure(L, fn, 0);
   g_lua.settable(L, -10001);
}

void lua_hooks_install(uintptr_t exe_base)
{
   using namespace lua_addrs::modtools;

   g_lua.pushcclosure = (fn_lua_pushcclosure)resolve(exe_base, lua_pushcclosure);
   g_lua.pushlstring  = (fn_lua_pushlstring) resolve(exe_base, lua_pushlstring);
   g_lua.settable     = (fn_lua_settable)    resolve(exe_base, lua_settable);
   g_lua.tolstring    = (fn_lua_tolstring)   resolve(exe_base, lua_tolstring);
   g_lua.pushnumber   = (fn_lua_pushnumber)  resolve(exe_base, lua_pushnumber);
   g_lua.tonumber     = (fn_lua_tonumber)    resolve(exe_base, lua_tonumber);
   g_lua.gettop       = (fn_lua_gettop)      resolve(exe_base, lua_gettop);
   g_lua.pushnil      = (fn_lua_pushnil)     resolve(exe_base, lua_pushnil);
   g_lua.pushboolean  = (fn_lua_pushboolean) resolve(exe_base, lua_pushboolean);
   g_lua.toboolean    = (fn_lua_toboolean)   resolve(exe_base, lua_toboolean);
   g_lua.touserdata   = (fn_lua_touserdata)  resolve(exe_base, lua_touserdata);
   g_lua.isnumber     = (fn_lua_isnumber)    resolve(exe_base, lua_isnumber);

   original_init_state = (fn_init_state)resolve(exe_base, init_state);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_init_state, hooked_init_state);
   DetourTransactionCommit();

   // Resolve Weapon::ZoomFirstPerson for the barrel fire origin hook
   fn_ZoomFirstPerson = (ZoomFirstPerson_t)resolve(exe_base, weapon_zoom_first_person);

   // Patch WeaponCannon vtable: replace OverrideAimer with our hook.
   // Validate that the slot currently points to the vanilla implementation.
   g_cannonOverrideAimerSlot = (void**)resolve(exe_base, weapon_cannon_vftable_override_aimer);
   void* expected_impl  = resolve(exe_base, weapon_override_aimer_impl);
   void* expected_thunk = resolve(exe_base, weapon_override_aimer_thunk);

   g_cannonOverrideAimerHook = (void*)&hooked_cannon_OverrideAimer;

   if (*g_cannonOverrideAimerSlot == expected_impl ||
       *g_cannonOverrideAimerSlot == expected_thunk) {
      g_cannonOverrideAimerOrig = *g_cannonOverrideAimerSlot;
      *g_cannonOverrideAimerSlot = g_cannonOverrideAimerHook;
   }
}

void lua_hooks_uninstall()
{
   if (original_init_state) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(&(PVOID&)original_init_state, hooked_init_state);
      DetourTransactionCommit();
   }

   // Restore WeaponCannon vtable entry
   if (g_cannonOverrideAimerSlot && g_cannonOverrideAimerOrig) {
      DWORD oldProt;
      if (VirtualProtect(g_cannonOverrideAimerSlot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
         *g_cannonOverrideAimerSlot = g_cannonOverrideAimerOrig;
         VirtualProtect(g_cannonOverrideAimerSlot, sizeof(void*), oldProt, &oldProt);
      }
   }
}
