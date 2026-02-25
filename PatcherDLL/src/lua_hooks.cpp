#include "pch.h"
#include "lua_hooks.hpp"
#include "lua_funcs.hpp"

#include <detours.h>

lua_api g_lua = {};
lua_State* g_L = nullptr;
bool g_useBarrelFireOrigin = false;
char g_loadDisplayPath[260] = "Load\\load";

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

// ---------------------------------------------------------------------------
// OnCharacterExitVehicle event system
//
// Fully custom callback storage + dispatch.  Registration functions
// (OnCharacterExitVehicle, Name/Team/Class, Release) are Lua C functions
// in lua_funcs.cpp that store callbacks in the Lua registry and track
// filter metadata in the g_cevCallbacks[] array below.
//
// The C++ Detours hook on the exit-vehicle function (0x0052FC70) scans the
// character array to find charIndex + vehicleCtrl, resolves the character's
// name/team/class, then fires all matching callbacks via rawgeti + pcall.
//
// Lua usage (identical to vanilla On-Events):
//   local h = OnCharacterExitVehicle(function(player, vehicle) ... end)
//   ReleaseCharacterExitVehicle(h)
//   h = nil
// ---------------------------------------------------------------------------

CEVCallback g_cevCallbacks[CEV_MAX_CBS] = {};
int g_cevNextKey = -1000;

// __fastcall mirrors __thiscall ABI: ECX=this, EDX=unused, then stack args.
using fn_char_exit_vehicle = void(__fastcall*)(void* ecx, void* edx_unused, int arg1, int arg2);
static fn_char_exit_vehicle original_char_exit_vehicle = nullptr;

static constexpr uintptr_t exit_vehicle_func = 0x0052FC70;

static auto get_gamelog() {
   typedef void(__cdecl* GameLog_t)(const char*, ...);
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   return (GameLog_t)((0x7E3D50 - 0x400000u) + base);
}

static void __fastcall hooked_char_exit_vehicle(void* thisPtr, void* /*edx*/, int arg1, int arg2)
{
   // thisPtr = character Controllable* (EntitySoldier + 0x240).
   // Scan the character array BEFORE calling original (original clears state).
   int       charIndex      = -1;
   void*     vehicleCtrl    = nullptr;
   int       charTeam       = -1;
   uint32_t  entityNameHash = 0;
   void*     entityClassPtr = nullptr;

   const uintptr_t exe_base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - unrelocated_base + exe_base; };

   __try {
      const uintptr_t arrayBase = *(uintptr_t*)res(0xB93A08);
      const int       maxChars  = *(int*)      res(0xB939F4);
      if (arrayBase && maxChars > 0) {
         for (int i = 0; i < maxChars; i++) {
            const uintptr_t slot = arrayBase + (uintptr_t)i * 0x1B0;
            if (*(void**)(slot + 0x148) == thisPtr) {
               charIndex    = i;
               vehicleCtrl  = *(void**)(slot + 0x14C);
               charTeam     = *(int*)(slot + 0x134);

               char* entitySoldier = (char*)thisPtr - 0x240;
               entityNameHash  = *(uint32_t*)(entitySoldier + 4);   // EntityEx::mId
               entityClassPtr  = *(void**)   (entitySoldier + 8);   // EntityEx::mEntityClass
               break;
            }
         }
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {}

   // vehicleEntity = vehicleCtrl - 0x240 (EntitySoldier base)
   void* vehicleEntity = vehicleCtrl ? (char*)vehicleCtrl - 0x240 : nullptr;

   // Fire all matching callbacks.
   if (charIndex >= 0 && g_L) {
      for (int i = 0; i < CEV_MAX_CBS; i++) {
         if (g_cevCallbacks[i].regKey == 0) continue;

         bool match = false;
         switch (g_cevCallbacks[i].filterType) {
            case CEV_PLAIN: match = true; break;
            case CEV_NAME:  match = (entityNameHash != 0) && (entityNameHash == g_cevCallbacks[i].nameHash); break;
            case CEV_TEAM:  match = (charTeam == g_cevCallbacks[i].teamFilter); break;
            case CEV_CLASS: match = (entityClassPtr != nullptr) && (entityClassPtr == g_cevCallbacks[i].classPtr); break;
         }

         if (match) {
            __try {
               g_lua.rawgeti(g_L, -10001, g_cevCallbacks[i].regKey);
               g_lua.pushnumber(g_L, (float)charIndex);
               g_lua.pushlightuserdata(g_L, vehicleEntity);
               int rc = g_lua.pcall(g_L, 2, 0, 0);
               if (rc != 0 && g_lua.tolstring) {
                  size_t len = 0;
                  const char* err = g_lua.tolstring(g_L, -1, &len);
                  if (err) get_gamelog()("[CEV] pcall error: %s\n", err);
                  g_lua.settop(g_L, -2);
               }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
         }
      }
   }

   original_char_exit_vehicle(thisPtr, nullptr, arg1, arg2);
}

// ---------------------------------------------------------------------------
// LoadDisplay::EnterState hook
//
// The original function has a hardcoded PUSH imm32 pointing at "Load\\load"
// in .rdata (VA 0x00A5AF5C). During install we patch the 4-byte operand of
// that PUSH instruction to point to g_loadDisplayPath instead.  Modders call
// SetLoadDisplayLevel() from ScriptPreInit to override the path before the
// load screen fires.
// ---------------------------------------------------------------------------

// VA 0x0067e388 — the 4-byte imm32 operand of the PUSH instruction at 0x0067e387
static constexpr uintptr_t enter_state_path_op = 0x0067e388;

// Saved so we can restore on uninstall.
static uint32_t* g_enter_state_path_op_ptr  = nullptr;
static uint32_t  g_enter_state_path_op_orig = 0;

using fn_init_state = void(__cdecl*)();
static fn_init_state original_init_state = nullptr;

static void __cdecl hooked_init_state()
{
   original_init_state();

   // RESET the path to vanilla every time a new mission/state starts
   strncpy_s(g_loadDisplayPath, sizeof(g_loadDisplayPath), "Load\\load", _TRUNCATE);

   g_L = *(lua_State**)resolve((uintptr_t)GetModuleHandleW(nullptr), lua_addrs::modtools::g_lua_state_ptr);

   // Reset callback storage for the new Lua state.
   memset(g_cevCallbacks, 0, sizeof(g_cevCallbacks));
   g_cevNextKey = -1000;

   if (g_L) {
      register_lua_functions(g_L);
   }
}

void lua_register_func(lua_State* L, const char* name, lua_CFunction fn)
{
   if (!g_lua.pushlstring || !g_lua.pushcclosure || !g_lua.settable) return;

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
   g_lua.touserdata        = (fn_lua_touserdata)       resolve(exe_base, lua_touserdata);
   g_lua.pushlightuserdata = (fn_lua_pushlightuserdata) resolve(exe_base, lua_pushlightuserdata);
   g_lua.isnumber          = (fn_lua_isnumber)          resolve(exe_base, lua_isnumber);
   g_lua.gettable     = (fn_lua_gettable)    resolve(exe_base, lua_gettable);
   g_lua.pcall        = (fn_lua_pcall)       resolve(exe_base, lua_pcall);
   g_lua.rawgeti      = (fn_lua_rawgeti)     resolve(exe_base, lua_rawgeti);
   g_lua.settop       = (fn_lua_settop)       resolve(exe_base, lua_settop);
   g_lua.insert       = (fn_lua_insert)       resolve(exe_base, lua_insert);
   original_init_state = (fn_init_state)resolve(exe_base, init_state);

   original_char_exit_vehicle = (fn_char_exit_vehicle)resolve(exe_base, exit_vehicle_func);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r1 = DetourAttach(&(PVOID&)original_init_state, hooked_init_state);
   LONG r2 = DetourAttach(&(PVOID&)original_char_exit_vehicle, hooked_char_exit_vehicle);
   LONG rc = DetourTransactionCommit();

   auto fn_log = get_gamelog();
   fn_log("DetourAttach init_state=%ld  exit_vehicle=%ld  commit=%ld\n", r1, r2, rc);

   // Patch the PUSH imm32 operand inside LoadDisplay::EnterState so the
   // hardcoded "Load\\load" pointer is replaced by &g_loadDisplayPath.
   // dllmain.cpp has already made all sections PAGE_READWRITE at this point.
   g_enter_state_path_op_ptr  = (uint32_t*)resolve(exe_base, enter_state_path_op);
   g_enter_state_path_op_orig = *g_enter_state_path_op_ptr;
   *g_enter_state_path_op_ptr = (uint32_t)(uintptr_t)g_loadDisplayPath;
   fn_log("[LoadDisplay] patched path operand 0x%08x -> 0x%08x (\"%s\")\n",
          g_enter_state_path_op_orig, *g_enter_state_path_op_ptr, g_loadDisplayPath);

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
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_init_state)          DetourDetach(&(PVOID&)original_init_state, hooked_init_state);
   if (original_char_exit_vehicle)   DetourDetach(&(PVOID&)original_char_exit_vehicle, hooked_char_exit_vehicle);
   DetourTransactionCommit();

   // Restore the PUSH operand in LoadDisplay::EnterState
   if (g_enter_state_path_op_ptr && g_enter_state_path_op_orig) {
      DWORD oldProt;
      if (VirtualProtect(g_enter_state_path_op_ptr, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
         *g_enter_state_path_op_ptr = g_enter_state_path_op_orig;
         VirtualProtect(g_enter_state_path_op_ptr, sizeof(uint32_t), oldProt, &oldProt);
      }
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
