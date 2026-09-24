#include "pch.h"
#include "lua_events.hpp"
#include "lua_funcs.hpp"
#include "core/resolve.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/layout/character.hpp"

#include <detours.h>

// =============================================================================
// Custom engine-backed Lua events. See lua_events.hpp and docs/RE/OnEventSystem.md.
// =============================================================================

// EventManager::Event<T,A>, 0x18 bytes, identical on all three builds.
// The embedded Node starts at +4, so every engine call that wants a Node* gets
// `&ev.mVtable`, never `&ev`.
#pragma pack(push, 1)
struct EngineEvent {
   const char* mName;      // +0x00  the only field the name generation reads
   void*       mVtable;    // +0x04  Node vptr, copied from a stock event of the family
   uint16_t    mRefWord;   // +0x08  PblRef: count in the high 14 bits, low 2 are flags
   uint16_t    mPad;       // +0x0a
   void*       mParent;    // +0x0c
   void*       mChild;     // +0x10  listener tree root
   void*       mSibling;   // +0x14
};
#pragma pack(pop)
static_assert(sizeof(EngineEvent) == 0x18, "EventManager::Event<T,A> is 0x18 bytes");

// A live, singly-owned event: count 1, both PblRef flag bits set. Trigger
// add-refs and releases around every dispatch, so a zero here would drive the
// count to 0 on the first fire and invoke the deleting destructor on a static.
static constexpr uint16_t kEventRefWordLive = 7;

struct LuaEventDef {
   const char*    name;      // becomes On<name>, On<name>Name/Team/Class, Release<name>
   LuaEventFamily family;
};

// ---------------------------------------------------------------------------
// The event table. Adding a callback is one row here plus a lua_event_fire()
// call from the hook that knows when the thing happened.
// ---------------------------------------------------------------------------
static const LuaEventDef kEvents[] = {
   // The engine fires CharacterEnterVehicle but never wired up the exit side,
   // so this is the one genuinely missing half of a stock pair.
   { "CharacterExitVehicle", LuaEventFamily::CharacterGameObject },
};
static_assert(_countof(kEvents) == (size_t)LuaEventId::Count,
              "kEvents[] and LuaEventId must stay in step");

static EngineEvent s_events[_countof(kEvents)] = {};
static bool        s_registered = false;

// ---------------------------------------------------------------------------
// Engine entry points
// ---------------------------------------------------------------------------

// __thiscall Node::Trigger(Node* this, void* arg0, void* arg1)
using fn_node_trigger = void(__fastcall*)(void* node, void* edx, void* a0, void* a1);
// __fastcall Event::Cleanup(Node* this) -- detaches every listener
using fn_listeners_cleanup = void(__fastcall*)(void* node, void* edx);
// EventManager::Init / Cleanup, both __cdecl(void)
using fn_event_manager = void(__cdecl*)();

// Registrars. The modtools build compiles them __cdecl(Event*, lua_CFunction);
// the retail LTCG builds folded the callback pointer in and made them
// __fastcall(Event* ECX). Both shapes are called through the same table below.
using fn_reg_cdecl    = void(__cdecl*)(void* ev, void* cb);
using fn_reg_fastcall = void(__fastcall*)(void* ev, void* edx);

static fn_node_trigger      s_trigger  = nullptr;
static fn_listeners_cleanup s_cleanup  = nullptr;

static fn_event_manager s_origInit    = nullptr;
static fn_event_manager s_origCleanup = nullptr;

// The five registrars for one family, plus the five callbacks they take on
// modtools (all null on retail, where they are folded into the registrar).
struct FamilyRegs {
   void* reg[5];
   void* cb[5];
};

static bool family_regs(LuaEventFamily family, FamilyRegs& out)
{
   switch (family) {
   case LuaEventFamily::CharacterGameObject: {
      const uintptr_t reg[5] = {
         g_addr->event_reg_char_go_release, g_addr->event_reg_char_go_on,
         g_addr->event_reg_char_go_name,    g_addr->event_reg_char_go_team,
         g_addr->event_reg_char_go_class,
      };
      const uintptr_t cb[5] = {
         g_addr->event_cb_char_go_release, g_addr->event_cb_char_go_on,
         g_addr->event_cb_char_go_name,    g_addr->event_cb_char_go_team,
         g_addr->event_cb_char_go_class,
      };
      for (int i = 0; i < 5; ++i) {
         if (!reg[i]) return false;
         // Only modtools needs the callback pointers; a missing one there would
         // register a null closure, so treat it as "family not available".
         if (g_build == GameBuild::Modtools && !cb[i]) return false;
         out.reg[i] = resolve(reg[i]);
         out.cb[i]  = cb[i] ? resolve(cb[i]) : nullptr;
      }
      return true;
   }
   default:
      return false;
   }
}

// The stock event whose vptr a family borrows. Reading it at runtime rather
// than hardcoding the vtable VA means one less address to port per build, and
// the vtable is the one the family's own registrars and Trigger expect.
static uintptr_t family_template(LuaEventFamily family)
{
   switch (family) {
   case LuaEventFamily::CharacterGameObject:
      return g_addr->event_char_gameobject_template;
   default:
      return 0;
   }
}

// Node* for an event object: the embedded Node starts at +4.
static inline void* event_node(EngineEvent* ev)
{
   return (char*)ev + offsetof(EngineEvent, mVtable);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static void register_all()
{
   if (s_registered) return;

   for (size_t i = 0; i < _countof(kEvents); ++i) {
      const LuaEventDef& def = kEvents[i];

      const uintptr_t tmpl = family_template(def.family);
      FamilyRegs regs = {};
      if (!tmpl || !family_regs(def.family, regs)) continue;

      EngineEvent* stock = (EngineEvent*)resolve(tmpl);

      EngineEvent& ev = s_events[i];
      __try {
         ev.mName    = def.name;
         ev.mVtable  = stock->mVtable;   // borrow the family's Node vtable
         ev.mRefWord = kEventRefWordLive;
         ev.mPad     = 0;
         ev.mParent  = nullptr;
         ev.mChild   = nullptr;
         ev.mSibling = nullptr;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         ev.mVtable = nullptr;
      }
      if (!ev.mVtable) continue;        // stock event not where we expect it

      // Each registrar builds its own name from ev.mName, pushes ev as upvalue 1
      // and installs the global -- or installs DummyLuaCallback instead when this
      // is a networked client, exactly as it does for the stock events.
      __try {
         for (int r = 0; r < 5; ++r) {
            if (g_build == GameBuild::Modtools)
               ((fn_reg_cdecl)regs.reg[r])(&ev, regs.cb[r]);
            else
               ((fn_reg_fastcall)regs.reg[r])(&ev, nullptr);
         }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         continue;
      }
   }

   s_registered = true;
}

// Detach every listener while the Lua state that issued their registry refs is
// still alive. LuaCallback's destructor calls luaL_unref on it, so deferring
// this to the next mission's Init would unref stale indices into a fresh
// registry. Bound to EventManager::Cleanup for exactly that reason.
static void cleanup_all()
{
   if (!s_cleanup) return;

   for (size_t i = 0; i < _countof(s_events); ++i) {
      if (!s_events[i].mVtable) continue;
      __try {
         s_cleanup(event_node(&s_events[i]), nullptr);
      } __except (EXCEPTION_EXECUTE_HANDLER) {}
   }
   s_registered = false;
}

static void __cdecl hooked_event_manager_init()
{
   s_origInit();
   // GameLoop::Init runs once per mission, so this is also the earliest
   // unambiguous "a mission is starting" signal available in the DLL.
   script_name_mark_mission_started();
   // After the original: the stock families register first, so a custom event
   // can never shadow a stock global by accident.
   register_all();
}

static void __cdecl hooked_event_manager_cleanup()
{
   cleanup_all();
   s_origCleanup();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void lua_event_fire(LuaEventId id, void* arg0, void* arg1)
{
   const size_t i = (size_t)id;
   if (i >= _countof(s_events)) return;
   if (!s_registered || !s_trigger) return;

   EngineEvent& ev = s_events[i];
   if (!ev.mVtable || !ev.mChild) return;   // nothing registered, nothing to walk

   __try {
      s_trigger(event_node(&ev), nullptr, arg0, arg1);
   } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void* lua_event_character_from_index(int index)
{
   if (index < 0 || !g_addr->char_array_base || !g_addr->max_chars) return nullptr;

   __try {
      const uintptr_t base = *(uintptr_t*)resolve(g_addr->char_array_base);
      const int       max  = *(int*)      resolve(g_addr->max_chars);
      if (!base || index >= max) return nullptr;
      // Stride 0x1B0, the same divisor LuaPushItem<Character> uses to turn the
      // pointer back into the index the Lua callback receives.
      return (void*)(base + (uintptr_t)index * layout::Character::kSize);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return nullptr;
   }
}

void lua_events_install(uintptr_t exe_base)
{
   if (!g_addr->event_manager_init || !g_addr->event_manager_cleanup ||
       !g_addr->event_node_trigger || !g_addr->event_listeners_cleanup)
      return;

   s_trigger = (fn_node_trigger)     resolve(exe_base, g_addr->event_node_trigger);
   s_cleanup = (fn_listeners_cleanup)resolve(exe_base, g_addr->event_listeners_cleanup);

   s_origInit    = (fn_event_manager)resolve(exe_base, g_addr->event_manager_init);
   s_origCleanup = (fn_event_manager)resolve(exe_base, g_addr->event_manager_cleanup);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)s_origInit,    hooked_event_manager_init);
   DetourAttach(&(PVOID&)s_origCleanup, hooked_event_manager_cleanup);
   DetourTransactionCommit();
}

void lua_events_uninstall()
{
   if (!s_origInit && !s_origCleanup) return;

   // Drop our listeners first: the detours must still be live is irrelevant
   // here, but the engine's Event::Cleanup must run before we stop tracking the
   // events, or the LuaCallback nodes leak with their registry refs held.
   cleanup_all();

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (s_origInit)    DetourDetach(&(PVOID&)s_origInit,    hooked_event_manager_init);
   if (s_origCleanup) DetourDetach(&(PVOID&)s_origCleanup, hooked_event_manager_cleanup);
   DetourTransactionCommit();

   s_origInit    = nullptr;
   s_origCleanup = nullptr;
}
