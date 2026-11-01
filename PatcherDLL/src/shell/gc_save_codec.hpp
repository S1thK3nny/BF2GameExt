#pragma once

#include <stddef.h>
#include <stdint.h>

// The stock ScriptCB_Save*State / Load*State encoding, rebuilt with bounds.
// Pure code: Lua is reached only through GcLua, so tests can run it on a fake.
//
// Raw stream: u8 value count, then one tagged item per value, tag = lua_type:
//   0 nil | 1 bool u8 | 2 lightuserdata u32 | 3 number f32 | 4 string, NUL ended
//   5 table: key/value item pairs, then a 0 byte | 6 function, its global name
//   7 userdata, 8 thread and anything else: the tag alone, loaded back as nil
// The raw stream is then one zlib stream (level 9), zero padded to the buffer.

struct lua_State;

namespace gc_save {

struct GcLua {
   lua_State* L;
   int         (__cdecl* gettop)(lua_State*);
   void        (__cdecl* settop)(lua_State*, int);
   int         (__cdecl* type)(lua_State*, int);
   int         (__cdecl* toboolean)(lua_State*, int);
   float       (__cdecl* tonumber)(lua_State*, int);
   const char* (__cdecl* tolstring)(lua_State*, int, size_t*);
   void*       (__cdecl* touserdata)(lua_State*, int);
   const void* (__cdecl* topointer)(lua_State*, int);
   void        (__cdecl* pushvalue)(lua_State*, int);
   int         (__cdecl* next)(lua_State*, int);
   int         (__cdecl* checkstack)(lua_State*, int);
   void        (__cdecl* pushnil)(lua_State*);
   void        (__cdecl* pushboolean)(lua_State*, int);
   void        (__cdecl* pushnumber)(lua_State*, float);
   void        (__cdecl* pushlstring)(lua_State*, const char*, size_t);
   void        (__cdecl* pushlightuserdata)(lua_State*, void*);
   void        (__cdecl* newtable)(lua_State*);
   void        (__cdecl* rawset)(lua_State*, int);
   void        (__cdecl* gettable)(lua_State*, int);
};

constexpr int      kGlobalsIndex = -10001;  // LUA_GLOBALSINDEX in Lua 5.0
constexpr uint32_t kVanillaSize  = 0x200;   // the stock metagame buffer
constexpr uint32_t kMaxSize      = 16 * 1024;
constexpr uint32_t kMaxRaw       = 1024 * 1024;
constexpr int      kMaxValues    = 255;     // the count is one byte
constexpr int      kMaxDepth     = 64;      // also stops self-referencing tables

enum class Status {
   Ok,
   Empty,          // nothing passed; stock saves nothing either
   TooManyValues,
   TooDeep,
   RawTooBig,
   TooBig,         // compressed does not fit the buffer
   OutOfMemory,
   BadStream,      // inflate failed
   BadData,        // inflated data is cut off or malformed
};

struct SaveResult {
   Status   status;
   uint32_t compressed;
   uint32_t raw;
   int      values;
};

struct LoadResult {
   Status   status;
   int      values;     // pushed on the Lua stack
   uint32_t consumed;   // compressed bytes the stream used
   uint32_t raw;
};

// Saves stack values 1..top into dst as one zlib stream, the rest of dst zeroed.
// dst is left untouched unless the result is Ok. The Lua stack is unchanged.
SaveResult save(const GcLua& lua, uint8_t* dst, uint32_t cap);

// Pushes the saved values. On failure nothing is pushed.
LoadResult load(const GcLua& lua, const uint8_t* src, uint32_t len);

const char* status_text(Status s);

} // namespace gc_save
