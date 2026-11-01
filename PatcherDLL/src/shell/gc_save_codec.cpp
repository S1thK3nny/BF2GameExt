#include "pch.h"
#include "gc_save_codec.hpp"
#include "util/miniz_config.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Mirrors the stock WriteLuaItem / ReadLuaItem (modtools 0x468DC0 / 0x469290)
// item for item, so a save that fits 512 bytes is one vanilla can load. The
// stock pair writes into 64 KB on the stack with no bounds, recurses forever on
// a table that contains itself, never checks the deflate result and parses
// past the inflated end; each of those is a status here instead.

namespace gc_save {
namespace {

constexpr int kNil = 0, kBoolean = 1, kLightUserdata = 2, kNumber = 3,
              kString = 4, kTable = 5, kFunction = 6;

struct Writer {
   uint8_t* data   = nullptr;
   uint32_t size   = 0;
   uint32_t cap    = 0;
   Status   status = Status::Ok;

   ~Writer() { free(data); }

   bool reserve(uint32_t n)
   {
      if (status != Status::Ok) return false;
      if (n > kMaxRaw - size) { status = Status::RawTooBig; return false; }
      if (size + n <= cap) return true;
      uint32_t want = cap ? cap : 4096;
      while (want < size + n) want *= 2;
      if (want > kMaxRaw) want = kMaxRaw;
      void* grown = realloc(data, want);
      if (!grown) { status = Status::OutOfMemory; return false; }
      data = (uint8_t*)grown;
      cap  = want;
      return true;
   }
   void put(const void* src, uint32_t n)
   {
      if (!reserve(n)) return;
      memcpy(data + size, src, n);
      size += n;
   }
   void byte(uint8_t b) { put(&b, 1); }
   void string(const char* s) { put(s ? s : "", s ? (uint32_t)strlen(s) + 1 : 1); }
};

// A function is saved as the name of a global holding it, as stock does.
void write_function_name(const GcLua& lua, Writer& w)
{
   lua_State* L = lua.L;
   const void* fn = lua.topointer(L, -1);
   lua.pushnil(L);
   while (lua.next(L, kGlobalsIndex)) {
      if (lua.type(L, -2) == kString && lua.topointer(L, -1) == fn) {
         w.string(lua.tolstring(L, -2, nullptr));
         lua.settop(L, -3);
         return;
      }
      lua.settop(L, -2);
   }
   w.byte(0);
}

void write_item(const GcLua& lua, Writer& w, int idx, int depth)
{
   if (w.status != Status::Ok) return;
   lua_State* L = lua.L;
   if (depth > kMaxDepth || !lua.checkstack(L, 4)) { w.status = Status::TooDeep; return; }

   lua.pushvalue(L, idx);
   const int type = lua.type(L, -1);
   w.byte((uint8_t)type);
   switch (type) {
   case kBoolean:
      w.byte(lua.toboolean(L, -1) ? 1 : 0);
      break;
   case kLightUserdata: {
      const uint32_t p = (uint32_t)(uintptr_t)lua.touserdata(L, -1);
      w.put(&p, 4);
      break;
   }
   case kNumber: {
      const float f = lua.tonumber(L, -1);
      w.put(&f, 4);
      break;
   }
   case kString:
      w.string(lua.tolstring(L, -1, nullptr));
      break;
   case kTable:
      lua.pushnil(L);
      while (w.status == Status::Ok && lua.next(L, -2)) {
         write_item(lua, w, -2, depth + 1);
         write_item(lua, w, -1, depth + 1);
         lua.settop(L, -2);
      }
      w.byte(0);
      break;
   case kFunction:
      write_function_name(lua, w);
      break;
   default:
      break;
   }
   lua.settop(L, -2);
}

struct Reader {
   const uint8_t* data;
   uint32_t       size;
   uint32_t       pos;
   Status         status;
};

// A NUL ended string at the read position, or null if it runs off the end.
const char* read_string(Reader& r, size_t* len)
{
   const char* s   = (const char*)r.data + r.pos;
   const void* end = memchr(s, 0, r.size - r.pos);
   if (!end) { r.status = Status::BadData; return nullptr; }
   *len = (const char*)end - s;
   r.pos += (uint32_t)*len + 1;
   return s;
}

bool read_item(const GcLua& lua, Reader& r, int depth)
{
   lua_State* L = lua.L;
   if (depth > kMaxDepth || !lua.checkstack(L, 4)) { r.status = Status::TooDeep; return false; }
   if (r.pos >= r.size) { r.status = Status::BadData; return false; }

   const uint8_t type = r.data[r.pos++];
   switch (type) {
   case kNil:
      lua.pushnil(L);
      return true;
   case kBoolean:
      if (r.pos + 1 > r.size) break;
      lua.pushboolean(L, r.data[r.pos++] != 0);
      return true;
   case kLightUserdata: {
      if (r.pos + 4 > r.size) break;
      uint32_t p;
      memcpy(&p, r.data + r.pos, 4);
      r.pos += 4;
      lua.pushlightuserdata(L, (void*)(uintptr_t)p);
      return true;
   }
   case kNumber: {
      if (r.pos + 4 > r.size) break;
      float f;
      memcpy(&f, r.data + r.pos, 4);
      r.pos += 4;
      lua.pushnumber(L, f);
      return true;
   }
   case kString: {
      size_t len;
      const char* s = read_string(r, &len);
      if (!s) return false;
      lua.pushlstring(L, s, len);
      return true;
   }
   case kTable:
      lua.newtable(L);
      for (;;) {
         if (r.pos >= r.size) { r.status = Status::BadData; return false; }
         if (r.data[r.pos] == 0) { ++r.pos; return true; }
         if (!read_item(lua, r, depth + 1) || !read_item(lua, r, depth + 1)) return false;
         // Stock rawsets these too, and Lua errors out of the load on a nil or NaN key.
         const int keyType = lua.type(L, -2);
         if (keyType == kNil || (keyType == kNumber && isnan(lua.tonumber(L, -2))))
            lua.settop(L, -3);
         else
            lua.rawset(L, -3);
      }
   case kFunction: {
      size_t len;
      const char* name = read_string(r, &len);
      if (!name) return false;
      lua.pushlstring(L, name, len);
      lua.gettable(L, kGlobalsIndex);
      return true;
   }
   default:
      lua.pushnil(L);
      return true;
   }
   r.status = Status::BadData;
   return false;
}

} // namespace

SaveResult save(const GcLua& lua, uint8_t* dst, uint32_t cap)
{
   SaveResult result = {Status::Ok, 0, 0, 0};
   lua_State* L = lua.L;
   const int top = lua.gettop(L);
   result.values = top;
   if (top < 1) { result.status = Status::Empty; return result; }
   if (top > kMaxValues) { result.status = Status::TooManyValues; return result; }

   Writer w;
   w.byte((uint8_t)top);
   for (int i = 1; i <= top && w.status == Status::Ok; ++i)
      write_item(lua, w, i, 0);
   lua.settop(L, top);
   result.raw = w.size;
   if (w.status != Status::Ok) { result.status = w.status; return result; }

   mz_ulong outLen = mz_compressBound(w.size);
   uint8_t* out = (uint8_t*)malloc(outLen);
   if (!out) { result.status = Status::OutOfMemory; return result; }
   if (mz_compress2(out, &outLen, w.data, w.size, MZ_BEST_COMPRESSION) != MZ_OK) {
      free(out);
      result.status = Status::OutOfMemory;
      return result;
   }
   result.compressed = (uint32_t)outLen;
   if (outLen > cap) {
      free(out);
      result.status = Status::TooBig;
      return result;
   }
   memcpy(dst, out, outLen);
   memset(dst + outLen, 0, cap - outLen);
   free(out);
   return result;
}

LoadResult load(const GcLua& lua, const uint8_t* src, uint32_t len)
{
   LoadResult result = {Status::Ok, 0, 0, 0};
   if (!len) { result.status = Status::Empty; return result; }

   uint8_t* raw = (uint8_t*)malloc(kMaxRaw);
   if (!raw) { result.status = Status::OutOfMemory; return result; }
   mz_ulong rawLen = kMaxRaw, srcLen = len;
   const int z = mz_uncompress2(raw, &rawLen, src, &srcLen);
   if (z != MZ_OK) {
      free(raw);
      result.status = z == MZ_BUF_ERROR ? Status::RawTooBig
                    : z == MZ_MEM_ERROR ? Status::OutOfMemory : Status::BadStream;
      return result;
   }
   result.consumed = (uint32_t)srcLen;
   result.raw      = (uint32_t)rawLen;
   if (rawLen < 1) { free(raw); result.status = Status::BadData; return result; }

   lua_State* L = lua.L;
   const int base  = lua.gettop(L);
   const int count = raw[0];
   Reader r = {raw, (uint32_t)rawLen, 1, Status::Ok};
   for (int i = 0; i < count; ++i) {
      if (!read_item(lua, r, 0)) {
         lua.settop(L, base);
         free(raw);
         result.status = r.status;
         return result;
      }
   }
   free(raw);
   result.values = count;
   return result;
}

const char* status_text(Status s)
{
   switch (s) {
   case Status::Ok:            return "ok";
   case Status::Empty:         return "nothing to save";
   case Status::TooManyValues: return "more than 255 values were passed";
   case Status::TooDeep:       return "tables are nested more than 64 deep or contain themselves";
   case Status::RawTooBig:     return "the state is more than 1 MB before compression";
   case Status::TooBig:        return "the state does not fit the save";
   case Status::OutOfMemory:   return "out of memory";
   case Status::BadStream:     return "the save data is damaged";
   case Status::BadData:       return "the save data is cut off or malformed";
   }
   return "unknown";
}

} // namespace gc_save
