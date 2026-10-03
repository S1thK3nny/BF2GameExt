#pragma once

#include <stdint.h>

// Modtools is a debug build, Steam and GOG are release builds with one shared
// layout. Set once by game_build_select(); Debug until then, like g_addr.
enum class Layout : uint8_t { Debug, Release };
extern Layout g_layout;

// A struct member whose offset can differ between the two layouts. Declared in
// the game-named header next to the struct; callers never check the build.
//
//    layout::EntityDroideka::mState(obj) = 3;
//    uint32_t off = layout::EntityDroideka::mState.off();   // for asm caves
template <class T>
struct Field {
   uint32_t dbg, rel;

   constexpr Field(uint32_t both) : dbg(both), rel(both) {}
   constexpr Field(uint32_t d, uint32_t r) : dbg(d), rel(r) {}

   uint32_t off() const { return g_layout == Layout::Release ? rel : dbg; }

   T&       operator()(void* obj) const       { return *(T*)((char*)obj + off()); }
   const T& operator()(const void* obj) const { return *(const T*)((const char*)obj + off()); }
};

// A struct size or array stride that can differ between the two layouts.
struct BuildSize {
   uint32_t dbg, rel;

   constexpr BuildSize(uint32_t both) : dbg(both), rel(both) {}
   constexpr BuildSize(uint32_t d, uint32_t r) : dbg(d), rel(r) {}

   uint32_t get() const { return g_layout == Layout::Release ? rel : dbg; }
};
