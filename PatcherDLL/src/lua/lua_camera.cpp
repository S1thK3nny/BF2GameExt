#include "pch.h"
#include "lua_camera.hpp"
#include "core/resolve.hpp"
#include "render/scripted_camera.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// See lua_camera.hpp. Arguments are read the way the stock functions and the
// other GameExt ones take them: a unit is an entity pointer, a world object's
// name or a character index; a point is a table {x, y, z}; a matrix is the
// stock one GetPathPoint, GetEntityMatrix and CreateMatrix return.

namespace {

using namespace scripted_camera;

constexpr int kGlobals = -10001;   // LUA_GLOBALSINDEX

int arg_count(lua_State* L) { return g_lua.gettop ? g_lua.gettop(L) : 0; }

int type_at(lua_State* L, int idx)
{
   if (g_lua.type) return g_lua.type(L, idx);
   // No lua_type on this build: numbers and strings are all that can be told.
   if (g_lua.isnumber(L, idx)) return lua_t::number;
   return g_lua.tolstring(L, idx, nullptr) ? lua_t::string : lua_t::nil;
}

int absolute(lua_State* L, int idx) { return idx < 0 ? arg_count(L) + idx + 1 : idx; }

template <class... A>
void warn(const char* fmt, A... args)
{
   warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__, fmt, args...);
}

// A stock matrix: 64 bytes of PblMatrix in a full userdata. Anything else
// fails the frame checks.
bool read_matrix(lua_State* L, int idx, float m[16])
{
   if (type_at(L, idx) != lua_t::userdata || !g_lua.touserdata) return false;
   const float* p = static_cast<const float*>(g_lua.touserdata(L, idx));
   if (!p) return false;
   __try {
      std::memcpy(m, p, 16 * sizeof(float));
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
   for (int i = 0; i < 16; ++i)
      if (!std::isfinite(m[i])) return false;
   for (int r = 0; r < 3; ++r) {
      const float len = std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]);
      if (len < 0.5f || len > 2.0f) return false;
   }
   return true;
}

// A table {x, y, z} or {x = .., y = .., z = ..}.
bool read_v3(lua_State* L, int idx, V3& out)
{
   idx = absolute(L, idx);
   if (type_at(L, idx) != lua_t::table) return false;
   float v[3];
   bool ok = true;
   for (int i = 0; i < 3 && ok; ++i) {
      g_lua.rawgeti(L, idx, i + 1);
      ok = type_at(L, -1) == lua_t::number;
      if (ok) v[i] = g_lua.tonumber(L, -1);
      g_lua.settop(L, -2);
   }
   if (!ok) {
      static const char* const keys[3] = { "x", "y", "z" };
      ok = true;
      for (int i = 0; i < 3 && ok; ++i) {
         g_lua.pushlstring(L, keys[i], 1);
         g_lua.gettable(L, idx);
         ok = type_at(L, -1) == lua_t::number;
         if (ok) v[i] = g_lua.tonumber(L, -1);
         g_lua.settop(L, -2);
      }
   }
   if (!ok) return false;
   out = { v[0], v[1], v[2] };
   return finite(out);
}

// A unit: an entity pointer, a character index, or a world object's name.
bool read_unit(lua_State* L, int idx, UnitRef& out)
{
   switch (type_at(L, idx)) {
   case lua_t::lightuserdata:
      out = unit_from_object(g_lua.touserdata(L, idx));
      return out.given();
   case lua_t::number:
      out = unit_from_character(g_lua.tointeger(L, idx));
      return out.given();
   case lua_t::string: {
      // A number written as a string is a character index, as the wingmen
      // functions take it.
      if (g_lua.isnumber(L, idx)) {
         out = unit_from_character(g_lua.tointeger(L, idx));
         return out.given();
      }
      const char* name = g_lua.tolstring(L, idx, nullptr);
      out = unit_from_name(name);
      return out.given();
   }
   default:
      return false;
   }
}

// A spot: a unit (aimed 1 m up from its origin unless told otherwise), a point
// {x, y, z}, or a matrix's position.
bool read_spot(lua_State* L, int idx, Spot& out)
{
   idx = absolute(L, idx);
   const int t = type_at(L, idx);
   if (t == lua_t::table) {
      out.type = Spot::Type::Point;
      return read_v3(L, idx, out.point);
   }
   if (t == lua_t::userdata) {
      float m[16];
      if (!read_matrix(L, idx, m)) return false;
      out.type = Spot::Type::Point;
      out.point = { m[12], m[13], m[14] };
      return true;
   }
   if (!read_unit(L, idx, out.unit)) return false;
   out.type = Spot::Type::Unit;
   out.local = { 0.0f, 1.0f, 0.0f };
   return true;
}

// opts[key] pushed (nil when opts is no table); pop it with settop(-2).
void push_field(lua_State* L, int opts, const char* key)
{
   if (opts > 0 && type_at(L, opts) == lua_t::table) {
      g_lua.pushlstring(L, key, std::strlen(key));
      g_lua.gettable(L, opts);
   } else {
      g_lua.pushnil(L);
   }
}

bool field_number(lua_State* L, int opts, const char* key, float& out)
{
   push_field(L, opts, key);
   const bool ok = type_at(L, -1) == lua_t::number;
   if (ok) out = g_lua.tonumber(L, -1);
   g_lua.settop(L, -2);
   return ok && std::isfinite(out);
}

bool field_bool(lua_State* L, int opts, const char* key, bool& out)
{
   push_field(L, opts, key);
   const int t = type_at(L, -1);
   const bool ok = t == lua_t::boolean || t == lua_t::number;
   if (t == lua_t::boolean) out = g_lua.toboolean(L, -1) != 0;
   else if (t == lua_t::number) out = g_lua.tonumber(L, -1) != 0.0f;
   g_lua.settop(L, -2);
   return ok;
}

bool field_string(lua_State* L, int opts, const char* key, std::string& out)
{
   push_field(L, opts, key);
   const char* s = type_at(L, -1) == lua_t::string ? g_lua.tolstring(L, -1, nullptr) : nullptr;
   if (s) out = s;
   g_lua.settop(L, -2);
   return s != nullptr;
}

bool field_v3(lua_State* L, int opts, const char* key, V3& out)
{
   push_field(L, opts, key);
   const bool ok = read_v3(L, -1, out);
   g_lua.settop(L, -2);
   return ok;
}

bool field_spot(lua_State* L, int opts, const char* key, Spot& out)
{
   push_field(L, opts, key);
   const bool present = type_at(L, -1) != lua_t::nil;
   const bool ok = present && read_spot(L, -1, out);
   g_lua.settop(L, -2);
   return ok;
}

// The options table, if the argument at idx is one.
int options_at(lua_State* L, int idx)
{
   return arg_count(L) >= idx && type_at(L, idx) == lua_t::table ? idx : 0;
}

// What every view order takes: lookAt (and lookAtOffset for a unit), fov,
// blend, time and release.
void read_common(lua_State* L, int opts, Common& c, const char* who)
{
   push_field(L, opts, "lookAt");
   const bool hasLookAt = type_at(L, -1) != lua_t::nil;
   g_lua.settop(L, -2);
   if (hasLookAt && !field_spot(L, opts, "lookAt", c.lookAt))
      warn("%s: lookAt is not a unit or a point; ignored\n", who);
   if (c.lookAt.type == Spot::Type::Unit) field_v3(L, opts, "lookAtOffset", c.lookAt.local);
   field_number(L, opts, "fov", c.fov);
   field_number(L, opts, "blend", c.blend);
   field_number(L, opts, "time", c.time);
   field_number(L, opts, "release", c.release);
   if (c.release < 0.0f) c.release = 0.0f;
}

void push_bool(lua_State* L, bool b)
{
   if (g_lua.pushboolean) g_lua.pushboolean(L, b ? 1 : 0);
   else if (b) g_lua.pushnumber(L, 1.0f);
   else g_lua.pushnil(L);
}

template <class... A>
int fail(lua_State* L, const char* fmt, A... args)
{
   warn(fmt, args...);
   g_lua.pushnil(L);
   return 1;
}

// Each node's facing, from the game's own GetPathPoint, called through Lua so
// its matrices are exactly the stock ones.
bool node_views(lua_State* L, const char* name, size_t count, std::vector<View>& out)
{
   out.clear();
   if (!g_lua.pcall) return false;
   const int base = arg_count(L);
   for (size_t i = 0; i < count; ++i) {
      g_lua.pushlstring(L, "GetPathPoint", 12);
      g_lua.gettable(L, kGlobals);
      if (type_at(L, -1) != lua_t::function) {
         g_lua.settop(L, base);
         return false;
      }
      g_lua.pushlstring(L, name, std::strlen(name));
      g_lua.pushnumber(L, (float)i);
      if (g_lua.pcall(L, 2, 1, 0) != 0) {
         g_lua.settop(L, base);
         return false;
      }
      float m[16];
      const bool ok = read_matrix(L, -1, m);
      g_lua.settop(L, base);
      if (!ok) return false;
      out.push_back(facing_view(m));
   }
   return true;
}

bool usable(lua_State* L)
{
   return L && g_lua.gettop && g_lua.settop && g_lua.gettable && g_lua.pushlstring && g_lua.rawgeti &&
          installed();
}

} // namespace

// CameraShot(where [, opts]): the view from a matrix, a unit or a point.
int lua_CameraShot(lua_State* L)
{
   if (!usable(L)) return 0;
   Shot o;
   if (type_at(L, 1) == lua_t::userdata) {
      if (!read_matrix(L, 1, o.matrix)) return fail(L, "CameraShot: that is not a matrix\n");
      o.fromMatrix = true;
   } else if (read_spot(L, 1, o.from)) {
      o.from.local = {};   // placed at the unit itself, then the offset
   } else {
      return fail(L, "CameraShot: give a matrix, a unit or a point {x, y, z}\n");
   }
   const int opts = options_at(L, 2);
   read_common(L, opts, o, "CameraShot");
   field_v3(L, opts, "offset", o.offset);
   shot(o);
   push_bool(L, true);
   return 1;
}

// CameraFollow(unit [, opts]): a chase camera on any unit.
int lua_CameraFollow(lua_State* L)
{
   if (!usable(L)) return 0;
   Follow o;
   if (!read_unit(L, 1, o.unit)) return fail(L, "CameraFollow: give a unit: an entity pointer, a name or a character index\n");
   const int opts = options_at(L, 2);
   read_common(L, opts, o, "CameraFollow");
   field_v3(L, opts, "offset", o.offset);
   field_v3(L, opts, "aim", o.aim);
   field_number(L, opts, "lag", o.lag);
   field_bool(L, opts, "world", o.world);
   if (o.lag < 0.0f) o.lag = 0.0f;
   follow(o);
   push_bool(L, true);
   return 1;
}

// CameraPath(path, seconds [, opts]): the camera flies a ZeroEditor path.
int lua_CameraPath(lua_State* L)
{
   if (!usable(L)) return 0;
   const char* name = type_at(L, 1) == lua_t::string ? g_lua.tolstring(L, 1, nullptr) : nullptr;
   if (!name || !*name) return fail(L, "CameraPath: give a path's name first\n");
   const std::string pathName = name;
   const float seconds = type_at(L, 2) == lua_t::number ? g_lua.tonumber(L, 2) : 0.0f;
   if (!(seconds > 0.0f)) return fail(L, "CameraPath(\"%s\"): give how many seconds it takes\n", pathName.c_str());

   Path o;
   o.seconds = seconds;
   if (!read_path(pathName.c_str(), o.nodes)) return fail(L, "CameraPath: the map has no path \"%s\"\n", pathName.c_str());
   if (o.nodes.size() < 2) return fail(L, "CameraPath: \"%s\" needs at least two nodes\n", pathName.c_str());

   const int opts = options_at(L, 3);
   const float defaultRelease = o.release;
   read_common(L, opts, o, "CameraPath");
   float endRelease = -1.0f;
   if (field_number(L, opts, "release", endRelease) && endRelease >= 0.0f) o.endRelease = endRelease;
   else o.release = defaultRelease;
   field_bool(L, opts, "loop", o.loop);
   std::string ease;
   if (field_string(L, opts, "ease", ease)) {
      if (ease == "linear") o.ease = Ease::Linear;
      else if (ease == "in") o.ease = Ease::In;
      else if (ease == "out") o.ease = Ease::Out;
      else if (ease != "inout") warn("CameraPath: ease \"%s\" is not inout, in, out or linear; inout it is\n", ease.c_str());
   }
   std::string aim;
   if (field_string(L, opts, "aim", aim) && aim == "nodes" && !o.lookAt.given()) {
      if (!node_views(L, pathName.c_str(), o.nodes.size(), o.nodeViews))
         warn("CameraPath(\"%s\"): its nodes' rotations could not be read; it looks the way it flies\n",
              pathName.c_str());
   }
   path(o);
   push_bool(L, true);
   return 1;
}

// CameraLookAt(target [, opts]): aims a scripted view, or turns the player's
// own toward target for a while. nil: a scripted view goes back to its own aim.
int lua_CameraLookAt(lua_State* L)
{
   if (!usable(L)) return 0;
   if (arg_count(L) < 1 || type_at(L, 1) == lua_t::nil) {
      aim(nullptr);
      push_bool(L, true);
      return 1;
   }
   Look o;
   if (!read_spot(L, 1, o.target)) return fail(L, "CameraLookAt: give a unit or a point {x, y, z}\n");
   const int opts = options_at(L, 2);
   if (o.target.type == Spot::Type::Unit) field_v3(L, opts, "offset", o.target.local);
   field_number(L, opts, "time", o.time);
   field_number(L, opts, "blend", o.blend);
   field_number(L, opts, "release", o.release);
   if (o.time < 0.0f) o.time = 0.0f;
   if (o.blend < 0.0f) o.blend = 0.0f;
   if (o.release < 0.0f) o.release = 0.0f;
   look(o);
   push_bool(L, true);
   return 1;
}

// CameraFOV([fov [, seconds]]): the FOV of the view on screen, in the game's
// measure (48 is its default); with no FOV the game has its own back.
int lua_CameraFOV(lua_State* L)
{
   if (!usable(L)) return 0;
   if (arg_count(L) < 1 || type_at(L, 1) != lua_t::number) {
      fov_off();
      push_bool(L, true);
      return 1;
   }
   const float degrees = g_lua.tonumber(L, 1);
   const float seconds = type_at(L, 2) == lua_t::number ? g_lua.tonumber(L, 2) : 0.0f;
   if (!(degrees > 0.0f)) return fail(L, "CameraFOV: give a field of view in degrees, 48 being the game's\n");
   fov(degrees, seconds);
   push_bool(L, true);
   return 1;
}

// CameraShake(amount [, seconds]): a shake through the stock queue.
int lua_CameraShake(lua_State* L)
{
   if (!usable(L)) return 0;
   const float amount = type_at(L, 1) == lua_t::number ? g_lua.tonumber(L, 1) : 0.0f;
   const float seconds = type_at(L, 2) == lua_t::number ? g_lua.tonumber(L, 2) : 1.0f;
   if (!(amount > 0.0f)) return fail(L, "CameraShake: give an amount above 0 (a grenade's is 0.5)\n");
   shake(amount, seconds);
   push_bool(L, true);
   return 1;
}

// CameraRelease([seconds]): back to the player's own camera, over seconds.
int lua_CameraRelease(lua_State* L)
{
   if (!usable(L)) return 0;
   const float seconds = type_at(L, 1) == lua_t::number ? g_lua.tonumber(L, 1) : 1.0f;
   release(seconds > 0.0f ? seconds : 0.0f);
   push_bool(L, true);
   return 1;
}

// GetCameraMatrix(): the view drawn last frame as a stock matrix, facing the
// way the camera looks, so CameraShot and SetEntityMatrix take it.
int lua_GetCameraMatrix(lua_State* L)
{
   if (!usable(L) || !g_lua.newuserdata) return 0;
   View v;
   if (!shown(v)) {
      g_lua.pushnil(L);
      return 1;
   }
   float* m = static_cast<float*>(g_lua.newuserdata(L, 16 * sizeof(float)));
   if (!m) return 0;
   const V3 rows[4] = { -v.right, v.up, -v.back, v.pos };
   for (int i = 0; i < 4; ++i) {
      m[i * 4 + 0] = rows[i].x;
      m[i * 4 + 1] = rows[i].y;
      m[i * 4 + 2] = rows[i].z;
      m[i * 4 + 3] = i == 3 ? 1.0f : 0.0f;
   }
   return 1;
}

// GetCameraPosition(): x, y, z of the view drawn last frame.
int lua_GetCameraPosition(lua_State* L)
{
   if (!usable(L)) return 0;
   View v;
   if (!shown(v)) {
      g_lua.pushnil(L);
      return 1;
   }
   g_lua.pushnumber(L, v.pos.x);
   g_lua.pushnumber(L, v.pos.y);
   g_lua.pushnumber(L, v.pos.z);
   return 3;
}

// IsCameraScripted(): a script holds the view, a release included.
int lua_IsCameraScripted(lua_State* L)
{
   if (!usable(L)) return 0;
   push_bool(L, holds());
   return 1;
}
