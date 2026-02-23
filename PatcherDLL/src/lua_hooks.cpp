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
   g_lua.settable(L, LUA_GLOBALSINDEX);
}

void lua_hooks_install(uintptr_t exe_base)
{
   using namespace lua_addrs::modtools;

   // -- lua_ State --
   g_lua.open              = (fn_lua_open)             resolve(exe_base, lua_open);
   g_lua.close             = (fn_lua_close)            resolve(exe_base, lua_close);
   g_lua.newthread         = (fn_lua_newthread)        resolve(exe_base, lua_newthread);
   g_lua.atpanic           = (fn_lua_atpanic)          resolve(exe_base, lua_atpanic);

   // -- lua_ Stack --
   g_lua.gettop            = (fn_lua_gettop)           resolve(exe_base, lua_gettop);
   g_lua.settop            = (fn_lua_settop)           resolve(exe_base, lua_settop);
   g_lua.pushvalue         = (fn_lua_pushvalue)        resolve(exe_base, lua_pushvalue);
   g_lua.remove            = (fn_lua_remove)           resolve(exe_base, lua_remove);
   g_lua.insert            = (fn_lua_insert)           resolve(exe_base, lua_insert);
   g_lua.replace           = (fn_lua_replace)          resolve(exe_base, lua_replace);
   g_lua.checkstack        = (fn_lua_checkstack)       resolve(exe_base, lua_checkstack);
   g_lua.xmove             = (fn_lua_xmove)            resolve(exe_base, lua_xmove);

   // -- lua_ Type checking --
   g_lua.type              = (fn_lua_type)             resolve(exe_base, lua_type);
   g_lua.type_name         = (fn_lua_typename)         resolve(exe_base, lua_typename);
   g_lua.isnumber          = (fn_lua_isnumber)         resolve(exe_base, lua_isnumber);
   g_lua.isstring          = (fn_lua_isstring)         resolve(exe_base, lua_isstring);
   g_lua.iscfunction       = (fn_lua_iscfunction)      resolve(exe_base, lua_iscfunction);
   g_lua.isuserdata        = (fn_lua_isuserdata)       resolve(exe_base, lua_isuserdata);
   g_lua.equal             = (fn_lua_equal)            resolve(exe_base, lua_equal);
   g_lua.rawequal          = (fn_lua_rawequal)         resolve(exe_base, lua_rawequal);
   g_lua.lessthan          = (fn_lua_lessthan)         resolve(exe_base, lua_lessthan);

   // -- lua_ Conversion --
   g_lua.tonumber          = (fn_lua_tonumber)         resolve(exe_base, lua_tonumber);
   g_lua.toboolean         = (fn_lua_toboolean)        resolve(exe_base, lua_toboolean);
   g_lua.tostring          = (fn_lua_tostring)         resolve(exe_base, lua_tostring);
   g_lua.tolstring         = (fn_luaL_checklstring)    resolve(exe_base, luaL_checklstring);
   g_lua.str_len           = (fn_lua_strlen)           resolve(exe_base, lua_strlen);
   g_lua.tocfunction       = (fn_lua_tocfunction)      resolve(exe_base, lua_tocfunction);
   g_lua.touserdata        = (fn_lua_touserdata)       resolve(exe_base, lua_touserdata);
   g_lua.tothread          = (fn_lua_tothread)         resolve(exe_base, lua_tothread);
   g_lua.topointer         = (fn_lua_topointer)        resolve(exe_base, lua_topointer);

   // -- lua_ Push --
   g_lua.pushnil           = (fn_lua_pushnil)          resolve(exe_base, lua_pushnil);
   g_lua.pushnumber        = (fn_lua_pushnumber)       resolve(exe_base, lua_pushnumber);
   g_lua.pushlstring       = (fn_lua_pushlstring)      resolve(exe_base, lua_pushlstring);
   g_lua.pushstring        = (fn_lua_pushstring)       resolve(exe_base, lua_pushstring);
   g_lua.pushvfstring      = (fn_lua_pushvfstring)     resolve(exe_base, lua_pushvfstring);
   g_lua.pushfstring       = (fn_lua_pushfstring)      resolve(exe_base, lua_pushfstring);
   g_lua.pushcclosure      = (fn_lua_pushcclosure)     resolve(exe_base, lua_pushcclosure);
   g_lua.pushboolean       = (fn_lua_pushboolean)      resolve(exe_base, lua_pushboolean);
   g_lua.pushlightuserdata = (fn_lua_pushlightuserdata)resolve(exe_base, lua_pushlightuserdata);

   // -- lua_ Get --
   g_lua.gettable          = (fn_lua_gettable)         resolve(exe_base, lua_gettable);
   g_lua.rawget            = (fn_lua_rawget)           resolve(exe_base, lua_rawget);
   g_lua.rawgeti           = (fn_lua_rawgeti)          resolve(exe_base, lua_rawgeti);
   g_lua.newtable          = (fn_lua_newtable)         resolve(exe_base, lua_newtable);
   g_lua.getmetatable      = (fn_lua_getmetatable)     resolve(exe_base, lua_getmetatable);
   g_lua.getfenv           = (fn_lua_getfenv)          resolve(exe_base, lua_getfenv);

   // -- lua_ Set --
   g_lua.settable          = (fn_lua_settable)         resolve(exe_base, lua_settable);
   g_lua.rawset            = (fn_lua_rawset)           resolve(exe_base, lua_rawset);
   g_lua.rawseti           = (fn_lua_rawseti)          resolve(exe_base, lua_rawseti);
   g_lua.setmetatable      = (fn_lua_setmetatable)     resolve(exe_base, lua_setmetatable);
   g_lua.setfenv           = (fn_lua_setfenv)          resolve(exe_base, lua_setfenv);

   // -- lua_ Call --
   g_lua.call              = (fn_lua_call)             resolve(exe_base, lua_call);
   g_lua.pcall             = (fn_lua_pcall)            resolve(exe_base, lua_pcall);
   g_lua.cpcall            = (fn_lua_cpcall)           resolve(exe_base, lua_cpcall);
   g_lua.load              = (fn_lua_load)             resolve(exe_base, lua_load);
   g_lua.dump              = (fn_lua_dump)             resolve(exe_base, lua_dump);

   // -- lua_ Misc --
   g_lua.error             = (fn_lua_error)            resolve(exe_base, lua_error);
   g_lua.next              = (fn_lua_next)             resolve(exe_base, lua_next);
   g_lua.concat            = (fn_lua_concat)           resolve(exe_base, lua_concat);
   g_lua.newuserdata       = (fn_lua_newuserdata)      resolve(exe_base, lua_newuserdata);

   // -- lua_ GC --
   g_lua.getgcthreshold    = (fn_lua_getgcthreshold)   resolve(exe_base, lua_getgcthreshold);
   g_lua.getgccount        = (fn_lua_getgccount)       resolve(exe_base, lua_getgccount);
   g_lua.setgcthreshold    = (fn_lua_setgcthreshold)   resolve(exe_base, lua_setgcthreshold);

   // -- lua_ Debug --
   g_lua.getupvalue        = (fn_lua_getupvalue)       resolve(exe_base, lua_getupvalue);
   g_lua.setupvalue        = (fn_lua_setupvalue)       resolve(exe_base, lua_setupvalue);
   g_lua.getinfo           = (fn_lua_getinfo)          resolve(exe_base, lua_getinfo);
   g_lua.pushupvalues      = (fn_lua_pushupvalues)     resolve(exe_base, lua_pushupvalues);
   g_lua.sethook           = (fn_lua_sethook)          resolve(exe_base, lua_sethook);
   g_lua.gethook           = (fn_lua_gethook)          resolve(exe_base, lua_gethook);
   g_lua.gethookmask       = (fn_lua_gethookmask)      resolve(exe_base, lua_gethookmask);
   g_lua.gethookcount      = (fn_lua_gethookcount)     resolve(exe_base, lua_gethookcount);
   g_lua.getstack          = (fn_lua_getstack)         resolve(exe_base, lua_getstack);
   g_lua.getlocal          = (fn_lua_getlocal)         resolve(exe_base, lua_getlocal);
   g_lua.setlocal          = (fn_lua_setlocal)         resolve(exe_base, lua_setlocal);

   // -- lua_ Compat --
   g_lua.version           = (fn_lua_version)          resolve(exe_base, lua_version);
   g_lua.dofile            = (fn_lua_dofile)           resolve(exe_base, lua_dofile);
   g_lua.dobuffer          = (fn_lua_dobuffer)         resolve(exe_base, lua_dobuffer);
   g_lua.dostring          = (fn_lua_dostring)         resolve(exe_base, lua_dostring);

   // -- luaL_ Auxiliary --
   g_lua.L_openlib         = (fn_luaL_openlib)         resolve(exe_base, luaL_openlib);
   g_lua.L_callmeta        = (fn_luaL_callmeta)        resolve(exe_base, luaL_callmeta);
   g_lua.L_typerror        = (fn_luaL_typerror)        resolve(exe_base, luaL_typerror);
   g_lua.L_argerror        = (fn_luaL_argerror)        resolve(exe_base, luaL_argerror);
   g_lua.L_checklstring    = (fn_luaL_checklstring)    resolve(exe_base, luaL_checklstring);
   g_lua.L_optlstring      = (fn_luaL_optlstring)      resolve(exe_base, luaL_optlstring);
   g_lua.L_checknumber     = (fn_luaL_checknumber)     resolve(exe_base, luaL_checknumber);
   g_lua.L_optnumber       = (fn_luaL_optnumber)       resolve(exe_base, luaL_optnumber);
   g_lua.L_checktype       = (fn_luaL_checktype)       resolve(exe_base, luaL_checktype);
   g_lua.L_checkany        = (fn_luaL_checkany)        resolve(exe_base, luaL_checkany);
   g_lua.L_newmetatable    = (fn_luaL_newmetatable)    resolve(exe_base, luaL_newmetatable);
   g_lua.L_getmetatable    = (fn_luaL_getmetatable)    resolve(exe_base, luaL_getmetatable);
   g_lua.L_checkudata      = (fn_luaL_checkudata)      resolve(exe_base, luaL_checkudata);
   g_lua.L_where           = (fn_luaL_where)           resolve(exe_base, luaL_where);
   g_lua.L_error           = (fn_luaL_error)           resolve(exe_base, luaL_error);
   g_lua.L_findstring      = (fn_luaL_findstring)      resolve(exe_base, luaL_findstring);
   g_lua.L_ref             = (fn_luaL_ref)             resolve(exe_base, luaL_ref);
   g_lua.L_unref           = (fn_luaL_unref)           resolve(exe_base, luaL_unref);
   g_lua.L_getn            = (fn_luaL_getn)            resolve(exe_base, luaL_getn);
   g_lua.L_setn            = (fn_luaL_setn)            resolve(exe_base, luaL_setn);
   g_lua.L_loadfile        = (fn_luaL_loadfile)        resolve(exe_base, luaL_loadfile);
   g_lua.L_loadbuffer      = (fn_luaL_loadbuffer)      resolve(exe_base, luaL_loadbuffer);
   g_lua.L_checkstack      = (fn_luaL_checkstack)      resolve(exe_base, luaL_checkstack);
   g_lua.L_getmetafield    = (fn_luaL_getmetafield)    resolve(exe_base, luaL_getmetafield);

   // -- luaL_ Buffer --
   g_lua.L_buffinit        = (fn_luaL_buffinit)        resolve(exe_base, luaL_buffinit);
   g_lua.L_prepbuffer      = (fn_luaL_prepbuffer)      resolve(exe_base, luaL_prepbuffer);
   g_lua.L_addlstring      = (fn_luaL_addlstring)      resolve(exe_base, luaL_addlstring);
   g_lua.L_addstring       = (fn_luaL_addstring)       resolve(exe_base, luaL_addstring);
   g_lua.L_addvalue        = (fn_luaL_addvalue)        resolve(exe_base, luaL_addvalue);
   g_lua.L_pushresult      = (fn_luaL_pushresult)      resolve(exe_base, luaL_pushresult);

   // -- luaopen_ Library openers --
   g_lua.open_base         = (fn_luaopen)              resolve(exe_base, luaopen_base);
   g_lua.open_table        = (fn_luaopen)              resolve(exe_base, luaopen_table);
   g_lua.open_io           = (fn_luaopen)              resolve(exe_base, luaopen_io);
   g_lua.open_string       = (fn_luaopen)              resolve(exe_base, luaopen_string);
   g_lua.open_math         = (fn_luaopen)              resolve(exe_base, luaopen_math);
   g_lua.open_debug        = (fn_luaopen)              resolve(exe_base, luaopen_debug);

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
