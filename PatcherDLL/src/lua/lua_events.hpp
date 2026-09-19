#pragma once

#include "pch.h"

// =============================================================================
// Custom engine-backed Lua events
// =============================================================================
// The engine generates every On<Name> / On<Name>Name / On<Name>Team /
// On<Name>Class / Release<Name> global from one field: the name string inside a
// 0x18-byte EventManager::Event<T,A> object. The registrars read the object and
// never check where it lives, so an object we build in the DLL is
// indistinguishable from a stock one.
//
// Declaring a new callback is therefore one row in kEvents[] plus a fire call
// from wherever the engine actually does the thing. Filtering, refcounting,
// reentrancy, Release, luaL_ref bookkeeping, multiplayer client stubbing and
// argument marshalling are all the engine's.
//
// Full RE writeup, address table and layout: docs/RE/OnEventSystem.md.

// Which stock Event<T,A> instantiation a custom event borrows its registrars
// and vtable from. The pair decides what the Lua callback receives:
//
//   CharacterGameObject -> fn(charIndex:number, object:lightuserdata)
//                          filters match on the CHARACTER's name/team/class
//
// Adding a family means adding its five registrar addresses to game_addrs.hpp
// and a case to family_regs() in lua_events.cpp.
enum class LuaEventFamily {
   CharacterGameObject,
};

// Stable ids for the events in kEvents[]. Features fire by id.
enum class LuaEventId {
   CharacterExitVehicle,
   Count
};

// Install the EventManager::Init / EventManager::Cleanup detours. Call from
// lua_hooks_install(). No-ops on builds whose addresses are unknown.
void lua_events_install(uintptr_t exe_base);
void lua_events_uninstall();

// Broadcast an event to every registered Lua callback, honouring the filters.
//
// For CharacterGameObject: arg0 is the engine Character* (NOT the index, NOT
// the Controllable) and arg1 is the GameObject* passed through as light
// userdata. Safe to call before registration or on an unported build: it
// no-ops. Safe to call with no listeners: Node::Trigger walks an empty tree.
void lua_event_fire(LuaEventId id, void* arg0, void* arg1);

// Character index -> engine Character*, for callers that only have the index.
// Returns nullptr if the character array is not available on this build.
void* lua_event_character_from_index(int index);
