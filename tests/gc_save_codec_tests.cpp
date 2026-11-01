// Standalone Win32 test of the GC save codec against a fake Lua 5.0 stack.
// No game, DLL build or Detours needed. Also writes gc_save_sample.gc next to
// the exe, which tests/gc_save_limit_abi_tests.py reads back with Python's zlib.
#include "../PatcherDLL/src/shell/gc_save_codec.cpp"
#include "../PatcherDLL/src/util/miniz_impl.c"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <deque>
#include <random>
#include <string>
#include <vector>

using namespace gc_save;

// ---------------------------------------------------------------------------
// Fake Lua: values live on a deque so string pointers stay put, like Lua's.
// ---------------------------------------------------------------------------
namespace {

struct Value {
   int         type = 0;   // lua_type numbering
   bool        b    = false;
   float       n    = 0;
   std::string s;
   int         id   = 0;   // table or function id
   void*       p    = nullptr;
};

bool same_key(const Value& a, const Value& b)
{
   if (a.type != b.type) return false;
   switch (a.type) {
   case 1: return a.b == b.b;
   case 2: return a.p == b.p;
   case 3: return a.n == b.n;
   case 4: return a.s == b.s;
   case 5: case 6: return a.id == b.id;
   default: return true;
   }
}

struct Table { std::vector<std::pair<Value, Value>> entries; };

std::deque<Value>  g_stack;
std::vector<Table> g_tables;   // [0] is the globals table
size_t             g_stackLimit = 100000;
int                g_rawsets    = 0;

Value nil() { return Value{}; }
Value num(float f) { Value v; v.type = 3; v.n = f; return v; }
Value str(const char* s) { Value v; v.type = 4; v.s = s; return v; }
Value boolean(bool b) { Value v; v.type = 1; v.b = b; return v; }
Value table_value(int id) { Value v; v.type = 5; v.id = id; return v; }
Value function(int id) { Value v; v.type = 6; v.id = id; return v; }
Value light(void* p) { Value v; v.type = 2; v.p = p; return v; }
Value userdata() { Value v; v.type = 7; return v; }

int new_table() { g_tables.push_back({}); return (int)g_tables.size() - 1; }

void set(int t, const Value& k, const Value& v)
{
   for (auto& e : g_tables[t].entries)
      if (same_key(e.first, k)) { e.second = v; return; }
   g_tables[t].entries.push_back({k, v});
}

const Value* get(int t, const Value& k)
{
   for (auto& e : g_tables[t].entries)
      if (same_key(e.first, k)) return &e.second;
   return nullptr;
}

Value& at(int idx)
{
   static Value globals;
   if (idx == kGlobalsIndex) { globals = table_value(0); return globals; }
   const int i = idx > 0 ? idx - 1 : (int)g_stack.size() + idx;
   assert(i >= 0 && i < (int)g_stack.size());
   return g_stack[i];
}

int __cdecl f_gettop(lua_State*) { return (int)g_stack.size(); }
void __cdecl f_settop(lua_State*, int idx)
{
   const int want = idx >= 0 ? idx : (int)g_stack.size() + idx + 1;
   assert(want >= 0);
   g_stack.resize(want);
}
int __cdecl f_type(lua_State*, int idx) { return at(idx).type; }
int __cdecl f_toboolean(lua_State*, int idx)
{
   const Value& v = at(idx);
   return !(v.type == 0 || (v.type == 1 && !v.b));
}
float __cdecl f_tonumber(lua_State*, int idx) { return at(idx).type == 3 ? at(idx).n : 0.0f; }
const char* __cdecl f_tolstring(lua_State*, int idx, size_t*)
{
   assert(at(idx).type == 4);   // the codec must never convert in place
   return at(idx).s.c_str();
}
void* __cdecl f_touserdata(lua_State*, int idx) { return at(idx).p; }
const void* __cdecl f_topointer(lua_State*, int idx)
{
   const Value& v = at(idx);
   if (v.type == 5) return (const void*)(uintptr_t)(0x10000 + v.id * 16);
   if (v.type == 6) return (const void*)(uintptr_t)(0x20000 + v.id * 16);
   return v.p;
}
void __cdecl f_pushvalue(lua_State*, int idx) { Value v = at(idx); g_stack.push_back(v); }
int __cdecl f_next(lua_State*, int idx)
{
   const Value t = at(idx);
   assert(t.type == 5);
   const Value key = g_stack.back();
   g_stack.pop_back();
   auto& entries = g_tables[t.id].entries;
   size_t i = 0;
   if (key.type != 0) {
      while (i < entries.size() && !same_key(entries[i].first, key)) ++i;
      assert(i < entries.size());
      ++i;
   }
   if (i >= entries.size()) return 0;
   g_stack.push_back(entries[i].first);
   g_stack.push_back(entries[i].second);
   return 1;
}
int __cdecl f_checkstack(lua_State*, int n) { return g_stack.size() + n <= g_stackLimit; }
void __cdecl f_pushnil(lua_State*) { g_stack.push_back(nil()); }
void __cdecl f_pushboolean(lua_State*, int b) { g_stack.push_back(boolean(b != 0)); }
void __cdecl f_pushnumber(lua_State*, float n) { g_stack.push_back(num(n)); }
void __cdecl f_pushlstring(lua_State*, const char* s, size_t len)
{
   Value v; v.type = 4; v.s.assign(s, len); g_stack.push_back(v);
}
void __cdecl f_pushlightuserdata(lua_State*, void* p) { g_stack.push_back(light(p)); }
void __cdecl f_newtable(lua_State*) { g_stack.push_back(table_value(new_table())); }
void __cdecl f_rawset(lua_State*, int idx)
{
   const Value t = at(idx);
   assert(t.type == 5);
   const Value k = at(-2), v = at(-1);
   assert(k.type != 0 && !(k.type == 3 && std::isnan(k.n)));   // real Lua errors here
   set(t.id, k, v);
   g_stack.resize(g_stack.size() - 2);
   ++g_rawsets;
}
void __cdecl f_gettable(lua_State*, int idx)
{
   const Value t = at(idx);
   const Value k = g_stack.back();
   g_stack.pop_back();
   const Value* v = get(t.id, k);
   g_stack.push_back(v ? *v : nil());
}

GcLua g_lua = {
   (lua_State*)0x1234, f_gettop, f_settop, f_type, f_toboolean, f_tonumber, f_tolstring,
   f_touserdata, f_topointer, f_pushvalue, f_next, f_checkstack, f_pushnil, f_pushboolean,
   f_pushnumber, f_pushlstring, f_pushlightuserdata, f_newtable, f_rawset, f_gettable,
};

void reset()
{
   g_stack.clear();
   g_tables.clear();
   g_tables.push_back({});   // globals
   g_stackLimit = 100000;
   g_rawsets = 0;
}

// Deep equality, tables compared by content.
bool equal(const Value& a, const Value& b, int depth = 0)
{
   if (a.type != b.type) return false;
   if (a.type != 5) return a.type == 6 ? a.id == b.id : same_key(a, b);
   if (depth > 70) return false;
   const auto& ea = g_tables[a.id].entries;
   const auto& eb = g_tables[b.id].entries;
   if (ea.size() != eb.size()) return false;
   for (const auto& e : ea) {
      bool found = false;
      for (const auto& f : eb) {
         const bool keyMatch = e.first.type == 5 ? equal(e.first, f.first, depth + 1)
                                                 : same_key(e.first, f.first);
         if (keyMatch && equal(e.second, f.second, depth + 1)) { found = true; break; }
      }
      if (!found) return false;
   }
   return true;
}

std::vector<uint8_t> inflate_all(const uint8_t* src, uint32_t len)
{
   std::vector<uint8_t> out(kMaxRaw);
   mz_ulong outLen = kMaxRaw, srcLen = len;
   const int z = mz_uncompress2(out.data(), &outLen, src, &srcLen);
   assert(z == MZ_OK);
   out.resize(outLen);
   return out;
}

std::vector<uint8_t> deflate_raw(const std::vector<uint8_t>& raw)
{
   mz_ulong len = mz_compressBound((mz_ulong)raw.size());
   std::vector<uint8_t> out(len);
   assert(mz_compress2(out.data(), &len, raw.data(), (mz_ulong)raw.size(), 9) == MZ_OK);
   out.resize(len);
   return out;
}

uint32_t crc32_bzip2(const uint8_t* p, size_t n)
{
   uint32_t crc = 0xFFFFFFFF;
   for (size_t i = 0; i < n; ++i) {
      crc ^= (uint32_t)p[i] << 24;
      for (int b = 0; b < 8; ++b) crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
   }
   return ~crc;
}

// Pushes the mixed state used by several tests and returns its values.
std::vector<Value> push_sample_state()
{
   const int planets = new_table();
   set(planets, str("cor"), num(1));
   set(planets, str("kas"), num(2));
   set(planets, str("tat"), num(2));
   set(planets, str("yav"), num(1));
   const int inner = new_table();
   set(inner, str("x"), num(3));
   const int list = new_table();
   set(list, num(1), num(2.5f));
   set(list, num(2), table_value(inner));
   set(0, str("ScriptCB_DoFile"), function(7));

   std::vector<Value> v = {table_value(planets), str("TQN"), num(100), boolean(true), nil(),
                           table_value(list), function(7), userdata()};
   for (const Value& x : v) g_stack.push_back(x);
   return v;
}

// The fixture below is the same state deflated by Python's zlib 1.3.1, level 9.
const uint8_t kPythonZlib[] = {
   0x78, 0xDA, 0xE3, 0x60, 0x65, 0x49, 0xCE, 0x2F, 0x62, 0x60, 0x66, 0x60, 0x68, 0xB0, 0x67,
   0xC9, 0x4E, 0x2C, 0x06, 0xB1, 0x18, 0x1C, 0x58, 0x4A, 0x12, 0x4B, 0xA0, 0xAC, 0xCA, 0xC4,
   0x32, 0x88, 0x2C, 0x03, 0x4B, 0x48, 0xA0, 0x1F, 0x88, 0x79, 0xC2, 0x89, 0x91, 0x91, 0x81,
   0x15, 0x2C, 0x06, 0x24, 0x14, 0x1C, 0xC0, 0xEA, 0x58, 0x59, 0x2A, 0x40, 0x72, 0x0E, 0x0E,
   0x0C, 0x0C, 0x6C, 0xC1, 0xC9, 0x45, 0x99, 0x05, 0x25, 0xCE, 0x4E, 0xF1, 0x2E, 0xF9, 0x6E,
   0x99, 0x39, 0xA9, 0x0C, 0xEC, 0x00, 0x38, 0x13, 0x11, 0x54,
};

// Raw stream of push_sample_state() in the stock encoding, built by hand.
std::vector<uint8_t> sample_raw()
{
   std::vector<uint8_t> r;
   auto bytes = [&](std::initializer_list<uint8_t> b) { r.insert(r.end(), b); };
   auto s = [&](const char* x) { r.push_back(4); r.insert(r.end(), x, x + strlen(x) + 1); };
   auto n = [&](float f) { r.push_back(3); uint8_t b[4]; memcpy(b, &f, 4); r.insert(r.end(), b, b + 4); };
   r.push_back(8);
   r.push_back(5); s("cor"); n(1); s("kas"); n(2); s("tat"); n(2); s("yav"); n(1); r.push_back(0);
   s("TQN");
   n(100);
   bytes({1, 1});
   r.push_back(0);
   r.push_back(5); n(1); n(2.5f); n(2); r.push_back(5); s("x"); n(3); r.push_back(0); r.push_back(0);
   r.push_back(6); { const char* f = "ScriptCB_DoFile"; r.insert(r.end(), f, f + strlen(f) + 1); }
   r.push_back(7);
   return r;
}

void check_loaded(const std::vector<Value>& want)
{
   assert(g_stack.size() == want.size());
   for (size_t i = 0; i < want.size(); ++i) {
      // userdata, threads and unknown tags come back as nil, as stock
      const Value expect = want[i].type == 7 ? nil() : want[i];
      assert(equal(g_stack[i], expect));
   }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_round_trip_and_stock_encoding()
{
   reset();
   const std::vector<Value> state = push_sample_state();
   uint8_t buf[kVanillaSize];
   memset(buf, 0xCC, sizeof(buf));
   const SaveResult s = save(g_lua, buf, sizeof(buf));
   assert(s.status == Status::Ok && s.values == 8);
   assert(g_stack.size() == state.size());          // stack untouched
   assert(s.compressed > 0 && s.compressed <= kVanillaSize);
   for (uint32_t i = s.compressed; i < sizeof(buf); ++i) assert(buf[i] == 0);   // padded

   const std::vector<uint8_t> raw = inflate_all(buf, s.compressed);
   assert(raw == sample_raw());                       // byte-for-byte the stock encoding
   assert(s.raw == raw.size());

   g_stack.clear();
   const LoadResult l = load(g_lua, buf, sizeof(buf));
   assert(l.status == Status::Ok && l.values == 8);
   assert(l.consumed == s.compressed && l.raw == raw.size());
   check_loaded(state);
}

void test_reads_python_zlib()
{
   reset();
   const std::vector<Value> state = push_sample_state();
   g_stack.clear();
   uint8_t buf[kVanillaSize] = {};
   memcpy(buf, kPythonZlib, sizeof(kPythonZlib));
   const LoadResult l = load(g_lua, buf, sizeof(buf));
   assert(l.status == Status::Ok && l.values == 8 && l.consumed == sizeof(kPythonZlib));
   check_loaded(state);
}

void test_function_not_global_saves_empty_name()
{
   reset();
   g_stack.push_back(function(99));
   uint8_t buf[64];
   const SaveResult s = save(g_lua, buf, sizeof(buf));
   assert(s.status == Status::Ok);
   const std::vector<uint8_t> raw = inflate_all(buf, s.compressed);
   assert((raw == std::vector<uint8_t>{1, 6, 0}));
   g_stack.clear();
   assert(load(g_lua, buf, sizeof(buf)).values == 1);
   assert(g_stack.back().type == 0);
}

void test_light_userdata_and_numbers()
{
   reset();
   g_stack.push_back(light((void*)0x12345678));
   g_stack.push_back(num(-0.0f));
   g_stack.push_back(num(16777216.0f));
   g_stack.push_back(num(INFINITY));
   g_stack.push_back(str(""));
   uint8_t buf[128];
   const SaveResult s = save(g_lua, buf, sizeof(buf));
   assert(s.status == Status::Ok);
   g_stack.clear();
   assert(load(g_lua, buf, sizeof(buf)).values == 5);
   assert(g_stack[0].type == 2 && g_stack[0].p == (void*)0x12345678);
   assert(g_stack[1].n == 0.0f && std::signbit(g_stack[1].n));
   assert(g_stack[2].n == 16777216.0f);
   assert(std::isinf(g_stack[3].n));
   assert(g_stack[4].type == 4 && g_stack[4].s.empty());
}

void test_empty_saves_nothing()
{
   reset();
   uint8_t buf[16];
   memset(buf, 0xAB, sizeof(buf));
   const SaveResult s = save(g_lua, buf, sizeof(buf));
   assert(s.status == Status::Empty && s.compressed == 0);
   for (uint8_t b : buf) assert(b == 0xAB);
   assert(load(g_lua, buf, 0).status == Status::Empty);
}

void test_255_values_fit_256_do_not()
{
   reset();
   for (int i = 0; i < 255; ++i) g_stack.push_back(num((float)i));
   std::vector<uint8_t> buf(kMaxSize);
   assert(save(g_lua, buf.data(), kMaxSize).status == Status::Ok);
   g_stack.clear();
   assert(load(g_lua, buf.data(), kMaxSize).values == 255);
   assert(g_stack[254].n == 254.0f);

   reset();
   for (int i = 0; i < 256; ++i) g_stack.push_back(num((float)i));
   memset(buf.data(), 0xAB, kMaxSize);
   const SaveResult s = save(g_lua, buf.data(), kMaxSize);
   assert(s.status == Status::TooManyValues);
   for (uint8_t b : buf) assert(b == 0xAB);
   assert(g_stack.size() == 256);
}

void test_self_reference_and_depth()
{
   reset();
   const int t = new_table();
   set(t, str("self"), table_value(t));
   g_stack.push_back(str("before"));
   g_stack.push_back(table_value(t));
   uint8_t buf[256];
   memset(buf, 0xAB, sizeof(buf));
   const SaveResult s = save(g_lua, buf, sizeof(buf));
   assert(s.status == Status::TooDeep);
   assert(g_stack.size() == 2);                      // restored after the abort
   for (uint8_t b : buf) assert(b == 0xAB);

   // kMaxDepth nested tables, the top one included, still save
   reset();
   int top = new_table(), cur = top;
   for (int i = 0; i < kMaxDepth - 1; ++i) {
      const int next = new_table();
      set(cur, num(1), table_value(next));
      cur = next;
   }
   set(cur, str("leaf"), boolean(true));
   g_stack.push_back(table_value(top));
   std::vector<uint8_t> big(kMaxSize);
   assert(save(g_lua, big.data(), kMaxSize).status == Status::Ok);
   const Value saved = g_stack.back();
   g_stack.clear();
   assert(load(g_lua, big.data(), kMaxSize).status == Status::Ok);
   assert(equal(g_stack.back(), saved));

   // one deeper is refused
   reset();
   top = new_table(); cur = top;
   for (int i = 0; i < kMaxDepth; ++i) {
      const int next = new_table();
      set(cur, num(1), table_value(next));
      cur = next;
   }
   set(cur, str("leaf"), boolean(true));
   g_stack.push_back(table_value(top));
   assert(save(g_lua, big.data(), kMaxSize).status == Status::TooDeep);
}

void test_checkstack_failure()
{
   reset();
   const int t = new_table();
   set(t, num(1), str("a"));
   g_stack.push_back(table_value(t));
   g_stackLimit = 3;
   uint8_t buf[64];
   assert(save(g_lua, buf, sizeof(buf)).status == Status::TooDeep);
   assert(g_stack.size() == 1);
   g_stackLimit = 100000;
   assert(save(g_lua, buf, sizeof(buf)).status == Status::Ok);
   g_stack.clear();
   g_stackLimit = 2;
   assert(load(g_lua, buf, sizeof(buf)).status == Status::TooDeep);
   assert(g_stack.empty());
}

void test_size_limits()
{
   std::mt19937 rng(1234);
   auto random_string = [&](size_t n) {
      std::string s(n, ' ');
      for (char& c : s) c = (char)('!' + rng() % 90);
      return s;
   };

   // ~3 KB compressed: over the stock 512, within 16 KB
   reset();
   const int t = new_table();
   for (int i = 0; i < 40; ++i) set(t, num((float)i), str(random_string(80).c_str()));
   g_stack.push_back(table_value(t));
   std::vector<uint8_t> buf(kMaxSize);
   SaveResult s = save(g_lua, buf.data(), kVanillaSize);
   assert(s.status == Status::TooBig && s.compressed > kVanillaSize);
   s = save(g_lua, buf.data(), kMaxSize);
   assert(s.status == Status::Ok && s.compressed > kVanillaSize && s.compressed < kMaxSize);
   const Value saved = g_stack.back();
   g_stack.clear();
   LoadResult l = load(g_lua, buf.data(), kMaxSize);
   assert(l.status == Status::Ok && l.consumed == s.compressed);
   assert(equal(g_stack.back(), saved));

   // incompressible data over 16 KB is refused and the buffer kept
   reset();
   g_stack.push_back(str(random_string(20000).c_str()));
   memset(buf.data(), 0xAB, kMaxSize);
   s = save(g_lua, buf.data(), kMaxSize);
   assert(s.status == Status::TooBig && s.compressed > kMaxSize);
   for (uint8_t b : buf) assert(b == 0xAB);

   // over 1 MB before compression is refused, even though it would compress well
   reset();
   g_stack.push_back(str(std::string(kMaxRaw, 'a').c_str()));
   s = save(g_lua, buf.data(), kMaxSize);
   assert(s.status == Status::RawTooBig);
   assert(g_stack.size() == 1);
}

void test_bad_streams()
{
   reset();
   std::vector<uint8_t> buf(kMaxSize, 0);
   // all zeros, as a never-written buffer
   assert(load(g_lua, buf.data(), kMaxSize).status == Status::BadStream);
   // random garbage
   std::mt19937 rng(99);
   for (int round = 0; round < 200; ++round) {
      for (auto& b : buf) b = (uint8_t)rng();
      const LoadResult l = load(g_lua, buf.data(), kMaxSize);
      assert(l.status != Status::Ok || l.values == (int)g_stack.size());
      g_stack.clear();
   }
   // a valid stream cut short
   reset();
   push_sample_state();
   std::vector<uint8_t> good(kVanillaSize);
   const SaveResult s = save(g_lua, good.data(), kVanillaSize);
   g_stack.clear();
   for (uint32_t cut = 0; cut < s.compressed; ++cut) {
      const LoadResult l = load(g_lua, good.data(), cut);
      assert(l.status != Status::Ok && g_stack.empty());
   }
   // one flipped byte anywhere in the stream: never a crash, never a half-loaded stack
   for (uint32_t i = 0; i < s.compressed; ++i) {
      std::vector<uint8_t> bad = good;
      bad[i] ^= 0x5A;
      const LoadResult l = load(g_lua, bad.data(), kVanillaSize);
      if (l.status != Status::Ok) assert(g_stack.empty());
      g_stack.clear();
   }
}

void test_malformed_raw()
{
   auto try_raw = [](std::vector<uint8_t> raw) {
      reset();
      std::vector<uint8_t> z = deflate_raw(raw);
      z.resize(kVanillaSize, 0);
      g_stack.push_back(str("sentinel"));
      const LoadResult l = load(g_lua, z.data(), (uint32_t)z.size());
      if (l.status != Status::Ok) assert(g_stack.size() == 1);   // only the sentinel
      return l;
   };
   assert(try_raw({}).status == Status::BadData);                          // no count
   assert(try_raw({3, 0, 0}).status == Status::BadData);                   // count past end
   assert(try_raw({1, 4, 'a', 'b'}).status == Status::BadData);            // string without NUL
   assert(try_raw({1, 6, 'f'}).status == Status::BadData);                 // function name without NUL
   assert(try_raw({1, 3, 0, 0}).status == Status::BadData);                // short number
   assert(try_raw({1, 1}).status == Status::BadData);                      // short bool
   assert(try_raw({1, 2, 1, 2, 3}).status == Status::BadData);             // short light userdata
   assert(try_raw({1, 5, 4, 'k', 0, 3, 0, 0, 128, 63}).status == Status::BadData);   // table not closed
   assert(try_raw({1, 5, 4, 'k', 0}).status == Status::BadData);           // key without value

   // nil keys (a userdata key loads as nil; tag 0 would end the table) and NaN
   // keys are dropped, the rest of the table loads
   const float nanf = std::nanf("");
   std::vector<uint8_t> raw = {1, 5, 7, 3, 0, 0, 128, 63, 3};
   uint8_t nb[4]; memcpy(nb, &nanf, 4);
   raw.insert(raw.end(), nb, nb + 4);
   raw.insert(raw.end(), {3, 0, 0, 0, 64, 4, 'o', 'k', 0, 3, 0, 0, 64, 64, 0});
   LoadResult l = try_raw(raw);
   assert(l.status == Status::Ok && l.values == 1);
   const Value& t = g_stack.back();
   assert(t.type == 5 && g_tables[t.id].entries.size() == 1);
   assert(get(t.id, str("ok"))->n == 3.0f);

   // unknown, userdata and thread tags load as nil, as stock
   l = try_raw({3, 7, 8, 200});
   assert(l.status == Status::Ok && l.values == 3);
   assert(g_stack[1].type == 0 && g_stack[2].type == 0 && g_stack[3].type == 0);

   // nesting deeper than kMaxDepth in a crafted file is refused
   std::vector<uint8_t> deep = {1};
   for (int i = 0; i < 200; ++i) deep.insert(deep.end(), {5, 3, 0, 0, 128, 63});
   l = try_raw(deep);
   assert(l.status == Status::TooDeep);
}

void test_trailing_bytes_ignored()
{
   reset();
   push_sample_state();
   std::vector<uint8_t> buf(kMaxSize);
   const SaveResult s = save(g_lua, buf.data(), kMaxSize);
   // what a bigger earlier save leaves behind the new stream
   for (uint32_t i = s.compressed; i < kMaxSize; ++i) buf[i] = (uint8_t)(i * 7);
   g_stack.clear();
   const LoadResult l = load(g_lua, buf.data(), kMaxSize);
   assert(l.status == Status::Ok && l.values == 8 && l.consumed == s.compressed);
}

void test_gamestate_shape()
{
   // A GC-sized state: 65 planets, fleets, unlocks, a custom block like this.custom.
   reset();
   const int planets = new_table();
   const char* names[] = {"cor", "kas", "tat", "yav", "nab", "dag", "end", "fel", "geo", "hot",
                          "kam", "mus", "myg", "pol", "uta", "mus", "dea", "spa", "tan", "geo"};
   for (int i = 0; i < 65; ++i) {
      char n[8];
      sprintf_s(n, "%s%d", names[i % 20], i);
      set(planets, str(n), num((float)(1 + i % 2)));
   }
   const int custom = new_table();
   for (int i = 0; i < 300; ++i) {
      const int entry = new_table();
      set(entry, str("owner"), num((float)(i % 3)));
      set(entry, str("hp"), num((float)(i * 13 % 100)));
      set(entry, str("alive"), boolean(i % 5 != 0));
      set(custom, num((float)i), table_value(entry));
   }
   g_stack.push_back(table_value(planets));
   g_stack.push_back(str("cor"));
   g_stack.push_back(table_value(custom));
   std::vector<uint8_t> buf(kMaxSize);
   const SaveResult s = save(g_lua, buf.data(), kMaxSize);
   assert(s.status == Status::Ok && s.compressed > kVanillaSize);
   const std::vector<Value> saved(g_stack.begin(), g_stack.end());
   g_stack.clear();
   assert(load(g_lua, buf.data(), kMaxSize).values == 3);
   for (int i = 0; i < 3; ++i) assert(equal(g_stack[i], saved[i]));
   printf("GC-shaped state: %u raw -> %u compressed; ", s.raw, s.compressed);
}

void write_sample(const char* exeDir)
{
   // A .gc file as the save FSM writes it for a state that fits: CRC + 512 bytes.
   reset();
   push_sample_state();
   uint8_t file[4 + kVanillaSize];
   const SaveResult s = save(g_lua, file + 4, kVanillaSize);
   assert(s.status == Status::Ok);
   const uint32_t crc = crc32_bzip2(file + 4, kVanillaSize);
   memcpy(file, &crc, 4);
   char path[MAX_PATH];
   sprintf_s(path, "%s\\gc_save_sample.gc", exeDir);
   FILE* f = nullptr;
   assert(fopen_s(&f, path, "wb") == 0 && f);
   fwrite(file, 1, sizeof(file), f);
   fclose(f);
}

} // namespace

int main(int, char** argv)
{
   test_round_trip_and_stock_encoding();
   test_reads_python_zlib();
   test_function_not_global_saves_empty_name();
   test_light_userdata_and_numbers();
   test_empty_saves_nothing();
   test_255_values_fit_256_do_not();
   test_self_reference_and_depth();
   test_checkstack_failure();
   test_size_limits();
   test_bad_streams();
   test_malformed_raw();
   test_trailing_bytes_ignored();
   test_gamestate_shape();

   char dir[MAX_PATH];
   strcpy_s(dir, argv[0]);
   if (char* slash = strrchr(dir, '\\')) *slash = 0; else strcpy_s(dir, ".");
   write_sample(dir);
   printf("all gc_save codec tests passed\n");
   return 0;
}
