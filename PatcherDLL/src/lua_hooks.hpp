#pragma once

#include "pch.h"

// =============================================================================
// SWBF2 Lua Types
// =============================================================================
// SWBF2 uses Lua 5.0. These types mirror the Lua 5.0 API just enough to call
// the functions we need via their in-exe addresses.

// Opaque Lua state - we only ever pass pointers to it.
struct lua_State;

// The C function signature Lua expects: int fn(lua_State*)
using lua_CFunction = int(__cdecl*)(lua_State*);

// =============================================================================
// Lua function pointer types (matched to in-exe signatures)
// =============================================================================

// lua_pushcclosure(L, fn, n) - pushes a C closure onto the Lua stack
using fn_lua_pushcclosure = void(__cdecl*)(lua_State* L, lua_CFunction fn, int n);

// lua_pushlstring(L, s, len) - push string of known length onto Lua stack
// NOTE: In Lua 5.0, lua_pushstring is a macro over lua_pushlstring.
using fn_lua_pushlstring = void(__cdecl*)(lua_State* L, const char* s, size_t len);

// lua_settable(L, idx) - does t[k]=v where t=stack[idx], k=stack[-2], v=stack[-1], pops both
using fn_lua_settable = void(__cdecl*)(lua_State* L, int idx);

// lua_tolstring(L, idx, len) - get string from Lua stack
using fn_lua_tolstring = const char*(__cdecl*)(lua_State* L, int idx, size_t* len);

// lua_pushnumber(L, n) - push number onto Lua stack
// NOTE: SWBF2 defines lua_Number as float, not double.
using fn_lua_pushnumber = void(__cdecl*)(lua_State* L, float n);

// lua_tonumber(L, idx) - get number from Lua stack
using fn_lua_tonumber = float(__cdecl*)(lua_State* L, int idx);

// lua_gettop (returns number of args on stack)
using fn_lua_gettop = int(__cdecl*)(lua_State* L);

// lua_pushnil
using fn_lua_pushnil = void(__cdecl*)(lua_State* L);

// lua_pushboolean
using fn_lua_pushboolean = void(__cdecl*)(lua_State* L, int b);

// lua_toboolean
using fn_lua_toboolean = int(__cdecl*)(lua_State* L, int idx);

// lua_touserdata(L, idx) - get light userdata pointer from Lua stack
using fn_lua_touserdata = void*(__cdecl*)(lua_State* L, int idx);

// lua_pushlightuserdata(L, p) - pushes a light userdata (raw void*) onto the stack
using fn_lua_pushlightuserdata = void(__cdecl*)(lua_State* L, void* p);

// lua_isnumber(L, idx) - returns 1 if the value at idx is a number or a string
// convertible to a number, 0 otherwise.
using fn_lua_isnumber = int(__cdecl*)(lua_State* L, int idx);

// lua_gettable(L, idx) - pushes t[k] where t=stack[idx], k=stack[-1]; pops k, pushes result
using fn_lua_gettable = void(__cdecl*)(lua_State* L, int idx);

// lua_pcall(L, nargs, nresults, errfunc) - calls function in protected mode
// Returns 0 on success, non-zero on error.
using fn_lua_pcall = int(__cdecl*)(lua_State* L, int nargs, int nresults, int errfunc);

// lua_rawget(L, idx) - does t[k] where t=stack[idx], k=stack[-1]; pops k, pushes result.
// This is 0x7B87C0. Previously mislabeled as lua_rawgeti and used with 3 args (L,idx,n)
// which caused crashes because it only takes 2 args (L, idx).
// We don't currently call this but document it to prevent future confusion.

// lua_rawgeti(L, idx, n) - pushes t[n] where t=stack[idx], raw (no metamethods).
// Confirmed: FUN_007b8810, plain __cdecl(L, t, n).
// Verified from both FUN_007b73e0 (luaL_ref freelist) and FUN_00486a30 call pattern.
using fn_lua_rawgeti = void(__cdecl*)(lua_State* L, int idx, int n);

// lua_settop(L, idx) - sets the stack top (idx >= 0: absolute; idx < 0: relative).
// lua_pop(L, n) is a macro for lua_settop(L, -(n)-1).
// Used to clean up the error string pcall leaves on the stack on failure.
using fn_lua_settop = void(__cdecl*)(lua_State* L, int idx);

// lua_insert(L, idx) - moves the top element into the given position,
// shifting up elements above that position to open space.
// Confirmed: FUN_007b7f20, called as (mState, -2) in thunk_FUN_004869d0
// (the lua_setglobal macro expansion: pushstring, insert(-2), settable).
using fn_lua_insert = void(__cdecl*)(lua_State* L, int idx);

namespace lua_addrs {
   // --------------------------------------------------------------------------
   // FOR BF2_modtools exe - fill in from Ghidra
   // --------------------------------------------------------------------------
   namespace modtools {
      // LuaHelper::InitState - initializes Lua and registers all standard libs.
      // We hook this: call original first (fully inits Lua), then read gLuaState_Pointer.
      // Ghidra VA: 0x00486660
      constexpr uintptr_t init_state = 0x486660;

      // Pointer to the exe's global lua_State* variable.
      // Ghidra VA: 0x00B35A58  (RVA/imagebase-offset: 0x735A58, imagebase = 0x400000)
      constexpr uintptr_t g_lua_state_ptr = 0xB35A58;

      // lua_pushcclosure(L, fn, n)
      constexpr uintptr_t lua_pushcclosure = 0x7B86A0;

      // lua_pushlstring(L, s, len) - pushes a string of known length
      // NOTE: We previously mislabeled this as lua_setfield. It's actually pushlstring.
      // Confirmed: called as (L, "__index", 7) in Open_IO.
      constexpr uintptr_t lua_pushlstring = 0x7B8580;

      // lua_settable(L, idx) - sets t[k]=v, pops k and v from stack
      // Lua 5.0 has no lua_setfield; use lua_pushlstring+lua_pushcclosure+lua_settable.
      // Confirmed: PushValue in luaL_openlib loop, called after pushcclosure with (L, tableIdx).
      constexpr uintptr_t lua_settable = 0x7B8960;

      // lua_tolstring(L, idx, len)
      constexpr uintptr_t lua_tolstring = 0x7B7B00;

      // lua_pushnumber(L, n)
      constexpr uintptr_t lua_pushnumber = 0x7B8560;

      // lua_tonumber(L, idx)
      // Confirmed: first CALL inside luaL_checknumber (0x7B7BD0). Returns float in ST0.
      // luaL_checknumber calls it, stores result, compares to 0, checks lua_isnumber if 0.
      constexpr uintptr_t lua_tonumber = 0x7B82A0;

      // lua_gettop(L)
      constexpr uintptr_t lua_gettop = 0x7B7E60;

      // lua_pushnil(L)
      // Confirmed: 1 arg (L only), writes LUA_TNIL=0 to L->top, advances top by 8.
      // Sits 0x20 bytes before lua_pushnumber (0x7B8560) as expected from lapi.c layout.
      constexpr uintptr_t lua_pushnil = 0x7B8540;

      // lua_pushboolean(L, b)
      constexpr uintptr_t lua_pushboolean = 0x7B8720;

      // lua_toboolean(L, idx)
      // Confirmed: returns 0 for nil (tag==0) and boolean-false (tag==1, val==0),
      // returns 1 for everything else. No FPU, no error path. 81 XREFs.
      constexpr uintptr_t lua_toboolean = 0x7B82F0;

      // lua_touserdata(L, idx) - returns light userdata pointer or NULL
      // Confirmed: 0x7B8440
      constexpr uintptr_t lua_touserdata = 0x7B8440;

      // lua_pushlightuserdata(L, p) - pushes a raw void* as light userdata.
      // Confirmed: 0x7B8750
      constexpr uintptr_t lua_pushlightuserdata = 0x7B8750;

      // lua_isnumber(L, idx)
      // Confirmed: FUN_007b8070 — checks slot type tag == 3 (LUA_TNUMBER),
      // falls back to lua_str2num for string coercion.
      constexpr uintptr_t lua_isnumber = 0x7B8070;

      // lua_gettable(L, idx)
      // Confirmed: FUN_007b89a0 — 37 XREFs, matches Lua 5.0 lua_gettable signature.
      // Sits immediately after lua_settable (0x7B8960) in lapi.c layout.
      constexpr uintptr_t lua_gettable = 0x7B89A0;

      // lua_pcall(L, nargs, nresults, errfunc)
      // Confirmed: 0x7B8B60 — protected-mode call, returns 0 on success.
      constexpr uintptr_t lua_pcall = 0x7B8B60;

      // lua_rawgeti(L, idx, n) - pushes t[n] where t = stack[idx], no metamethods.
      // Confirmed: FUN_007b8810 — plain __cdecl(L, t, n), observed in luaL_ref source
      // and FUN_00486a30.
      constexpr uintptr_t lua_rawgeti = 0x7B8810;

      // lua_settop(L, idx) - sets/restores the stack top.
      // Confirmed: 0x7B7E70
      constexpr uintptr_t lua_settop = 0x7B7E70;

      // lua_insert(L, idx) - moves top element to position idx, shifting others up.
      // Confirmed: FUN_007b7f20, used in thunk_FUN_004869d0 as (mState, -2).
      constexpr uintptr_t lua_insert = 0x7B7F20;

      // Aimer::SetSoldierInfo(Aimer*, PblVector3* pos, PblVector3* dir)
      // Sets mFirePos, mRootPos, mDirection, and bDirect on the Aimer.
      constexpr uintptr_t aimer_set_soldier_info = 0x5EE9D0;

      // WeaponCannon vtable entry for OverrideAimer (vtable slot 0x70).
      // Patched at startup to point to our hooked function.
      constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0xA524D8;

      // Weapon::OverrideAimer implementation and thunk (for vtable validation).
      constexpr uintptr_t weapon_override_aimer_impl = 0x61CEE0;
      constexpr uintptr_t weapon_override_aimer_thunk = 0x4068DE;

      // Weapon::ZoomFirstPerson() — returns true if weapon is in first-person zoom.
      constexpr uintptr_t weapon_zoom_first_person = 0x61B640;

      // -----------------------------------------------------------------------
      // BF1 LoadDisplay extension
      // -----------------------------------------------------------------------

      // LoadDisplay::LoadDataFile(lvlPath) — MakeFullName→PblFile→ChunkProcessor pipeline.
      // Hooked to inject a second lvl load ("Load\load_bf1ext") for BF1 custom textures.
      constexpr uintptr_t load_data_file_real       = 0x0067e2b0;

      // LoadDisplay::LoadConfig real implementation (thunk 0x004102df → here)
      constexpr uintptr_t load_config_real          = 0x0067c650;

      // LoadDisplay::RenderScreen real implementation (thunk 0x00413dc7 → here)
      constexpr uintptr_t render_screen_real        = 0x0067a1b0;

      // LoadDisplay::End() body — jumped to from the thunk at 0x0041451f which
      // ScriptCB_ShowLoadDisplay(false) calls.  Hooking the body intercepts the
      // call via the thunk's unconditional JMP.
      constexpr uintptr_t load_end_real             = 0x0067de10;

      // LoadDisplay::ProgressIndicator::SetAllOn() — instantly fills every bar
      // segment to 100%.  ECX = LoadDisplay* + 0xd30 (the ProgressIndicator).
      // Called once in hooked_load_end before the spin-loop so the bar shows 100%
      // while we hold End() until the BF1 animation finishes.
      constexpr uintptr_t progress_set_all_on       = 0x0040786f;

      // LoadDisplay::Update() — QPC-timed: advances the "Loading..." blink timer
      // (at ecx+0x14cc) and ProgressIndicator, then calls Render() internally.
      // Called from hooked_load_end every ~200 ms to keep blink text animated.
      constexpr uintptr_t load_update_real          = 0x0067c1d0;

      // LoadDisplay::Render() — camera/lighting setup → PlatformRender() → restore.
      // NOT QPC-throttled; runs at vsync rate.  Called at most every 33 ms to
      // target ~30 fps in both the end spin-loop and the asset-loading phase.
      constexpr uintptr_t load_render_real          = 0x00402b71;

      // DAT_00ba2f60 — QPC low-word timestamp written by LoadDisplay::Update()
      // only when the 50 ms throttle passes and Render() is actually called.
      // hooked_load_update reads this before/after to detect whether Update()
      // rendered so we can avoid double-renders and correctly track g_lastRenderMs.
      constexpr uintptr_t load_update_qpc_stamp     = 0x00ba2f60;

      // PlatformRenderTexture thunk (→ 0x006d0650)
      // __stdcall(hash, x0,y0,x1,y1, color_ptr, alpha_blend, u0,v0,u1,v1, r,g,b,a)
      constexpr uintptr_t platform_render_texture   = 0x004165fe;

      // PblConfig::PblConfig(fh)            — ctor, RETN 4
      constexpr uintptr_t pbl_config_ctor           = 0x00821000;

      // PblConfig::PblConfig(parent, share) — copy ctor, RETN 8
      constexpr uintptr_t pbl_config_copy_ctor      = 0x00821080;

      // PblConfig::ReadNextData(data_buf)   — writes hash/argc/args, RETN 4
      constexpr uintptr_t pbl_read_next_data        = 0x008210f0;

      // PblConfig::ReadNextScope(temp_buf)  — enters scope, returns temp_buf, RETN 4
      constexpr uintptr_t pbl_read_next_scope       = 0x00821140;

      // HashString raw (pure hash computation, __cdecl(const char*) → uint32_t).
      // 0x007e1b70 = inner function called by the __thiscall wrapper at 0x007e1bd0.
      // The wrapper stores the result via ECX ptr; the inner function simply returns
      // the hash in EAX with one stack arg — correct for our fn_hash_string_t typedef.
      constexpr uintptr_t hash_string               = 0x007e1b70;

      // Snd::Sound::Properties::FindByHashID(hash) — returns Properties* or null
      constexpr uintptr_t snd_find_by_hash_id       = 0x0088c500;

      // Snd::Sound::Play(entity, props, p3, p4, p5)
      constexpr uintptr_t snd_sound_play            = 0x0088cc10;

      // PblHashTableCode::_Find(table_ptr, table_size, hash) — looks up texture by hash.
      // Used inside PlatformRenderTexture (confirmed via disasm at 006d07ea).
      // Global texture table: 0xd4f994, size=0x2000. Returns NULL if not found.
      constexpr uintptr_t pbl_hash_table_find       = 0x007e1a40;

      // Global texture hash table pointer (passed to pbl_hash_table_find)
      constexpr uintptr_t tex_hash_table            = 0x00d4f994;

      // Global color/render-state pointer passed as arg 6 to PlatformRenderTexture
      constexpr uintptr_t color_ptr_global          = 0xae2150;

      // _RedSetCurrentHeap(int heap) — __cdecl; switches the active allocator heap,
      // returns the previous heap index.  Used to redirect our g_prt calls' SortHeap
      // array growth to RunTimeHeap (persistent) instead of TempLoadHeap (destroyed at
      // end-of-load, which leaves the SortHeap array pointer dangling and corrupts
      // RunTimeHeap's free list → crash in _RedGetHeapFree).
      // Confirmed: FUN_007e2c70, decompiled as int __cdecl _RedSetCurrentHeap(int heap).
      constexpr uintptr_t red_set_current_heap      = 0x007e2c70;

      // Global int: RunTimeHeap handle/index (value = 2 after GameMemory::BuildHeaps).
      // Stored at this address by the MOV [0x00b30220],EAX after _RedCreateHeap("Runtime").
      // We read it at runtime so we pass the correct index to _RedSetCurrentHeap.
      constexpr uintptr_t runtime_heap_global       = 0x00b30220;

      // Global int: s_loadHeap — the heap index passed to _RedSetCurrentHeap by
      // LoadDisplay::Update before it starts rendering.  Normally = TempLoadHeap (3).
      // Confirmed: address of MOV EAX,[0x00ba111c] / PUSH EAX / CALL _RedSetCurrentHeap
      // inside LoadDisplay::Update at 0067c1d0 (see Ghidra decompile, uVar4 line).
      // We temporarily set this to *runtime_heap_global (= RunTimeHeap = 2) before
      // calling the original Update so every SortHeap allocation during loading-screen
      // rendering (loading text, progress bar, BF1 overlay) goes to RunTimeHeap.
      // After ReleaseTempHeap this keeps the SortHeap backing array valid, preventing
      // the 007E2D77 (_RedGetHeapFree) and 008024A6 (MemoryPool::Allocate) crashes.
      constexpr uintptr_t s_loadheap_global         = 0x00ba111c;
   }

   namespace steam {
      // LuaHelper::InitState equivalent for Steam exe.
      constexpr uintptr_t init_state = 0xDEAD0000;        // TODO: REPLACE

      // Pointer to the exe's global lua_State* variable.
      // FIND: Same method as modtools - XREF writes after lua_open.
      constexpr uintptr_t g_lua_state_ptr = 0xDEAD0000;   // TODO: REPLACE

      // lua_pushcclosure(L, fn, n)
      constexpr uintptr_t lua_pushcclosure = 0xDEAD0002;   // TODO: REPLACE

      // lua_pushlstring(L, s, len)
      constexpr uintptr_t lua_pushlstring = 0xDEAD0005;    // TODO: REPLACE

      // lua_settable(L, idx)
      constexpr uintptr_t lua_settable = 0xDEAD0003;       // TODO: REPLACE

      // lua_tolstring(L, idx, len)
      constexpr uintptr_t lua_tolstring = 0xDEAD0004;      // TODO: REPLACE

      // lua_pushnumber(L, n)
      constexpr uintptr_t lua_pushnumber = 0xDEAD0006;     // TODO: REPLACE

      // lua_tonumber(L, idx)
      constexpr uintptr_t lua_tonumber = 0xDEAD0007;       // TODO: REPLACE

      // lua_gettop(L)
      constexpr uintptr_t lua_gettop = 0xDEAD0008;         // TODO: REPLACE

      // lua_pushnil(L)
      constexpr uintptr_t lua_pushnil = 0xDEAD0009;        // TODO: REPLACE

      // lua_pushboolean(L, b)
      constexpr uintptr_t lua_pushboolean = 0xDEAD000A;    // TODO: REPLACE

      // lua_toboolean(L, idx)
      constexpr uintptr_t lua_toboolean = 0xDEAD000B;      // TODO: REPLACE

      // lua_touserdata(L, idx)
      constexpr uintptr_t lua_touserdata = 0xDEAD000C;     // TODO: REPLACE

      // lua_pushlightuserdata(L, p)
      constexpr uintptr_t lua_pushlightuserdata = 0xDEAD0013; // TODO: REPLACE

      // lua_isnumber(L, idx)
      constexpr uintptr_t lua_isnumber = 0xDEAD000D;       // TODO: REPLACE

      // lua_gettable(L, idx)
      constexpr uintptr_t lua_gettable = 0xDEAD000E;       // TODO: REPLACE

      // lua_pcall(L, nargs, nresults, errfunc)
      constexpr uintptr_t lua_pcall = 0xDEAD000F;          // TODO: REPLACE

      // lua_rawgeti(L, idx, n)
      constexpr uintptr_t lua_rawgeti = 0xDEAD0010;        // TODO: REPLACE

      // lua_settop(L, idx)
      constexpr uintptr_t lua_settop = 0xDEAD0011;         // TODO: REPLACE

      // lua_insert(L, idx)
      constexpr uintptr_t lua_insert = 0xDEAD0012;         // TODO: REPLACE

      constexpr uintptr_t aimer_set_soldier_info = 0xDEAD0014;                // TODO: REPLACE
      constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0xDEAD0015;  // TODO: REPLACE
      constexpr uintptr_t weapon_override_aimer_impl = 0xDEAD0016;            // TODO: REPLACE
      constexpr uintptr_t weapon_override_aimer_thunk = 0xDEAD0017;           // TODO: REPLACE
   }
}

// =============================================================================
// Lua globals - resolved at runtime from exe base + addresses above
// =============================================================================
struct lua_api {
   fn_lua_pushcclosure pushcclosure = nullptr;
   fn_lua_pushlstring  pushlstring  = nullptr;
   fn_lua_settable     settable     = nullptr;
   fn_lua_tolstring    tolstring    = nullptr;
   fn_lua_pushnumber   pushnumber   = nullptr;
   fn_lua_tonumber     tonumber     = nullptr;
   fn_lua_gettop       gettop       = nullptr;
   fn_lua_pushnil      pushnil      = nullptr;
   fn_lua_pushboolean  pushboolean  = nullptr;
   fn_lua_toboolean    toboolean    = nullptr;
   fn_lua_touserdata       touserdata       = nullptr;
   fn_lua_pushlightuserdata pushlightuserdata = nullptr;
   fn_lua_isnumber         isnumber         = nullptr;
   fn_lua_gettable     gettable     = nullptr;
   fn_lua_pcall        pcall        = nullptr;
   fn_lua_rawgeti      rawgeti      = nullptr;
   fn_lua_settop       settop       = nullptr;
   fn_lua_insert       insert       = nullptr;

   int tointeger(lua_State* L, int idx) const { return static_cast<int>(tonumber(L, idx)); }
};

// Global API instance - populated by lua_hooks_install()
extern lua_api g_lua;

// The captured lua_State pointer (set when hook fires)
extern lua_State* g_L;

// Barrel fire origin toggle — when true, WeaponCannon fires from barrel
// hardpoint (mBarrelPoseMatrix) instead of the default aimer position.
extern bool g_useBarrelFireOrigin;

// Modder-configurable load level path used by LoadDisplay::EnterState.
// Defaults to "Load\\load" (vanilla). Set via SetLoadDisplayLevel() before
// the load screen fires (e.g. from ScriptPreInit).
extern char g_loadDisplayPath[260];

// =============================================================================
// OnCharacterExitVehicle — custom callback storage
// =============================================================================

#define CEV_MAX_CBS 64

enum CEVFilterType { CEV_PLAIN = 0, CEV_NAME = 1, CEV_TEAM = 2, CEV_CLASS = 3 };

struct CEVCallback {
   int      regKey;       // Lua globals key (for rawgeti), 0 if empty slot
   int      filterType;   // CEVFilterType
   int      teamFilter;   // for CEV_TEAM
   uint32_t nameHash;     // for CEV_NAME  — PblHash of the instance name string
   void*    classPtr;     // for CEV_CLASS — resolved EntityClass* from registry
};

extern CEVCallback g_cevCallbacks[CEV_MAX_CBS];
extern int g_cevNextKey;

// =============================================================================
// Public interface
// =============================================================================

// Call from install_patches() in dllmain.cpp.
// exe_base = relocated base address of the game exe.
void lua_hooks_install(uintptr_t exe_base);

// Cleanup (optional, called on DLL_PROCESS_DETACH if desired)
void lua_hooks_uninstall();

// Register a single C function as a named Lua global.
void lua_register_func(lua_State* L, const char* name, lua_CFunction fn);