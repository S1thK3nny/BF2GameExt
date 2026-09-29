// Standalone Win32 test; compiles the real adapter against fake native callbacks.
// No game, DLL build, Detours, or full Visual Studio solution required.
#define HUD_NUMBER_MATH_TEST
#include "../PatcherDLL/src/render/hud_number_math.cpp"
#include <algorithm>
#include <cassert>
#include <cfloat>
#include <climits>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {
struct FakeClass {
   EventClass base;
   std::vector<Handler*> handlers;
   std::vector<float> values;
   unsigned sends = 0;   // every send, Bool ones included
};
std::map<uint32_t, FakeClass> events;
unsigned destroyed = 0;
bool factoryFails = false;
const char* playerFilter = "player1";

EventClass* __cdecl fake_find_cdecl(uint32_t id)
{
   auto it = events.find(id);
   return it == events.end() ? nullptr : &it->second.base;
}
EventClass* __fastcall fake_find_fast(uint32_t id) { return fake_find_cdecl(id); }
EventClass* __cdecl fake_create(int type, const char* format, ...)
{
   char nameBuffer[512];
   va_list args;
   va_start(args, format);
   vsprintf_s(nameBuffer, format, args);
   va_end(args);
   auto& e = events[hash(nameBuffer)];
   e.base = {hash(nameBuffer), static_cast<uint32_t>(type)};
   return &e.base;
}
void __fastcall fake_filter_fast(const char* name, char* output)
{
   if (std::strncmp(name, "player1.", 8) == 0)
      sprintf_s(output, 512, "%s%s", playerFilter, name + 7);
   else strcpy_s(output, 512, name);
}
void __cdecl fake_filter_cdecl(const char* name, char* output, unsigned length)
{
   assert(length == 511);
   fake_filter_fast(name, output);
}
bool __cdecl fake_bind_cdecl(const Data* data, Handler* handler)
{
   char filtered[512];
   fake_filter_fast(data->string(0), filtered);
   handler->cls = fake_find_cdecl(hash(filtered));
   if (handler->cls) events[handler->cls->hash].handlers.push_back(handler);
   return true;
}
bool __fastcall fake_bind_fast(const Data* data, Handler* handler) { return fake_bind_cdecl(data, handler); }

void __fastcall fake_send(Event* event, void*)
{
   auto& e = events.at(event->cls->hash);
   ++e.sends;
   double value = 0;
   if (numeric_event(event->cls->type, event->bits, value)) e.values.push_back(static_cast<float>(value));
   for (Handler* handler : e.handlers) handler->callback(event, handler->data);
}

Shell* __fastcall fake_create_item(void* factory, void*, const char* itemName, void*, void*)
{
   if (factoryFails) return nullptr;
   Shell* shell = new Shell{};
   shell->factory = factory;
   shell->name = itemName;
   return shell;
}
void* __fastcall fake_destroy_item(Shell* shell, void*, unsigned flags)
{
   for (auto& entry : events) {
      auto& handlers = entry.second.handlers;
      for (auto it = handlers.begin(); it != handlers.end();) {
         if (*it == &shell->inputA || *it == &shell->inputB) it = handlers.erase(it);
         else ++it;
      }
   }
   ++destroyed;
   if (flags & 1) delete shell;
   return shell;
}
// The fake classes keep handlers in vectors, not the engine's linked list.
void fake_unbind(Handler* handler)
{
   for (auto& entry : events) {
      auto& handlers = entry.second.handlers;
      handlers.erase(std::remove(handlers.begin(), handlers.end(), handler), handlers.end());
   }
   handler->cls = nullptr;
}

void setup(bool mt)
{
   assert(!nodes);
   modtools = mt;
   findEvent = mt ? reinterpret_cast<void*>(fake_find_cdecl) : reinterpret_cast<void*>(fake_find_fast);
   readEvent = mt ? reinterpret_cast<void*>(fake_bind_cdecl) : reinterpret_cast<void*>(fake_bind_fast);
   filterName = mt ? reinterpret_cast<void*>(fake_filter_cdecl) : reinterpret_cast<void*>(fake_filter_fast);
   createEvent = fake_create;
   sendEvent = fake_send;
   createItem = fake_create_item;
   destroyItem = fake_destroy_item;
   unbindHandler = fake_unbind;
   events.clear();
   dispatchDepth = 0;
   playerFilter = "player1";
}

void clear_mission()
{
   while (nodes) {
      Shell* shell = nodes->shell;
      destroy(shell, nullptr, 0); // Engine DestroyAll owns the actual free.
      delete shell;
   }
   for (auto& e : events) assert(e.second.handlers.empty());
   events.clear();
   assert(dispatchDepth == 0);
}

Data text_data(const char* key, const char* value)
{
   Data data{hash(key), 1, {4}};
   strcpy_s(reinterpret_cast<char*>(data.args) + 4, 508, value);
   return data;
}
Data number_data(const char* key, float value)
{
   Data data{hash(key), 1, {}};
   std::memcpy(&data.args[0], &value, 4);
   return data;
}
// ConfigMunge's form of a comment after a property on its line (read off munged
// files): each word one more argument, hashed like a name, with any string
// the property had moved past the longer argument list.
Data with_comment(const Data& data, const char* comment)
{
   std::vector<uint32_t> words;
   for (const char* p = comment; *p;) {
      while (*p == ' ') ++p;
      const char* start = p;
      while (*p && *p != ' ') ++p;
      if (p > start) words.push_back(hash(std::string(start, p).c_str()));
   }
   const unsigned own = data.count;
   const unsigned shift = static_cast<unsigned>(words.size()) * 4;
   Data out{data.id, own + static_cast<unsigned>(words.size()), {}};
   for (unsigned i = 0; i < own; ++i)
      out.args[i] = data.string(i) ? data.args[i] + shift : data.args[i];
   std::memcpy(reinterpret_cast<char*>(out.args) + own * 4 + shift,
               reinterpret_cast<const char*>(data.args) + own * 4, sizeof(data.args) - own * 4 - shift);
   for (unsigned i = 0; i < words.size(); ++i) out.args[own + i] = words[i];
   return out;
}
// A property with several arguments, each a number, or a string when text is set.
struct Arg { float number; const char* text; };
Data args_data(const char* key, std::initializer_list<Arg> list)
{
   Data data{hash(key), static_cast<uint32_t>(list.size()), {}};
   unsigned offset = data.count * 4, i = 0;
   for (const Arg& a : list) {
      if (a.text) {
         data.args[i] = offset;
         strcpy_s(reinterpret_cast<char*>(data.args) + offset, sizeof(data.args) - offset, a.text);
         offset += static_cast<unsigned>(std::strlen(a.text)) + 1;
      } else {
         std::memcpy(&data.args[i], &a.number, 4);
      }
      ++i;
   }
   return data;
}
// An unquoted word, which ConfigMunge hashes like a name.
Data word_data(const char* key, const char* word)
{
   Data data{hash(key), 1, {}};
   data.args[0] = hash(word);
   return data;
}
void property(Shell* shell, const Data& data) { read_data(shell, nullptr, nullptr, &data); }
void text_property(Shell* shell, const char* key, const char* value) { property(shell, text_data(key, value)); }
void constant(Shell* shell, const char* key, float value) { property(shell, number_data(key, value)); }

Shell* math(const char* operation, const char* output)
{
   Shell* shell = create(nullptr, nullptr, output, nullptr, nullptr);
   assert(shell);
   text_property(shell, "Operation", operation);
   text_property(shell, "EventOutput", output);
   return shell;
}
void float_event(const char* name, float value)
{
   Event event{fake_find_cdecl(hash(name)), 0};
   assert(event.cls);
   std::memcpy(&event.bits, &value, 4);
   fake_send(&event, nullptr);
}
const std::vector<float>& output_values(const char* name) { return events.at(hash(name)).values; }

void core_tests()
{
   Operation op;
   assert(parse_operation("sUbTrAcT", op) && op == Operation::Subtract);
   assert(!parse_operation("Multiplie", op) && !parse_operation(nullptr, op));
   static_assert(hash("TransformNumberVector3") == 0x2ef8f006);
   Config config;
   float result = 123;
   const float expected[] = {9, 3, 18, 2, 3, 6};
   for (unsigned i = 0; i < 6; ++i) {
      config.operation = static_cast<Operation>(i);
      assert(calculate(config, 6, 3, result) && result == expected[i]);
   }
   config.operation = Operation::Divide;
   assert(!calculate(config, 1, 0, result));
   assert(!calculate(config, 1, -0.0, result));
   assert(calculate(config, 1e-20, 1e-20, result) && result == 1);
   assert(!calculate(config, FLT_MAX, FLT_MIN, result));
   config.clamp = true;
   assert(calculate(config, FLT_MAX, FLT_MIN, result) && result == 1);
   config.minimum = 3;
   assert(!calculate(config, 1, 1, result));
   config.minimum = 0;
   assert(!calculate(config, NAN, 1, result));
   assert(!calculate(config, 1, INFINITY, result));
   assert(!calculate(config, 1, 0, result)); // Clamp never masks divide-by-zero.
   config.operation = Operation::Multiply;
   assert(calculate(config, -FLT_MAX, FLT_MAX, result) && result == 0);
   config.clamp = false;
   assert(!calculate(config, FLT_MAX, FLT_MAX, result));

   double number;
   assert(numeric_event(2, 0x80000000, number) && number == INT_MIN);
   assert(numeric_event(2, 0xffffffff, number) && number == -1);
   assert(numeric_event(3, 0xffffffff, number) && number == UINT_MAX);
   assert(!numeric_event(1, 1, number));
   assert(!numeric_event(9, 0, number));
   assert(!numeric_event(4, 0x7f800000, number));
   assert(!numeric_event(4, 0x7fc00000, number));
   State state;
   assert(!state.next(result));
   state.input(1, true, 3);
   assert(!state.next(result));
   state.input(0, true, 2);
   assert(state.next(result) && result == 5);
   assert(!state.next(result));
   state.input(0, false, 2);
   state.input(1, true, 4);
   assert(!state.next(result));
   state.input(0, true, 1);
   assert(!state.next(result)); // Recovers, but unchanged from last publication.
   state.input(0, true, 2);
   assert(state.next(result) && result == 6);

   std::mt19937 rng(79231);
   std::uniform_real_distribution<double> dist(-10000, 10000);
   for (unsigned i = 0; i < 20000; ++i) {
      const double a = dist(rng), b = dist(rng);
      config.operation = static_cast<Operation>(i % 6);
      const double reference[] = {a+b, a-b, a*b, a/b, a < b ? a : b, a > b ? a : b};
      assert(calculate(config, a, b, result));
      assert(result == static_cast<float>(reference[i % 6]));
   }

   Data malformed{};
   malformed.count = 129;
   assert(!malformed.string(0) && !malformed.number(0, result));
   malformed.count = 1;
   malformed.args[0] = 512;
   assert(!malformed.string(0));
   malformed.args[0] = 3;
   assert(!malformed.string(0));
   malformed.args[0] = 4;
   std::memset(&malformed.args[1], 'x', 508);
   assert(!malformed.string(0));
   auto text = text_data("ConstantA", "10");
   assert(text.string(0) && !text.number(0, result));
   auto zero = number_data("ConstantA", 0);
   assert(zero.number(0, result) && result == 0);
}

void adapter_tests(bool mt)
{
   setup(mt);
   fake_create(4, "%s", "player1.weapon1.heat");
   Shell* inverse = math("Subtract", "player1.heatRemaining");
   constant(inverse, "ConstantA", 1);
   text_property(inverse, "EventInputB", "player1.weapon1.heat");
   post_read(inverse, nullptr);
   assert(node(inverse).ready);
   hud_number_math_update();
   assert(output_values("player1.heatRemaining").empty());
   float_event("player1.weapon1.heat", .25f);
   assert(output_values("player1.heatRemaining").back() == .75f);
   float_event("player1.weapon1.heat", .25f);
   hud_number_math_update();
   assert(output_values("player1.heatRemaining").size() == 1);
   float_event("player1.weapon1.heat", NAN);
   assert(output_values("player1.heatRemaining").size() == 1);
   float_event("player1.weapon1.heat", .5f);
   assert(output_values("player1.heatRemaining").back() == .5f);

   // Initial publication waits until consumers load, including constant chains.
   Shell* initial = math("Multiply", "constant.result");
   constant(initial, "ConstantA", 2);
   constant(initial, "ConstantB", 3);
   post_read(initial, nullptr);
   assert(output_values("constant.result").empty());
   Shell* chain = math("Add", "chain.result");
   text_property(chain, "EventInputA", "constant.result");
   constant(chain, "ConstantB", 4);
   post_read(chain, nullptr);
   hud_number_math_update();
   assert(output_values("constant.result").back() == 6);
   assert(output_values("chain.result").back() == 10);
   hud_number_math_update();
   assert(output_values("chain.result").size() == 1);

   // Both inputs independently trigger recomputation; zero recovers cleanly.
   fake_create(4, "%s", "a"); fake_create(4, "%s", "b");
   Shell* ratio = math("Divide", "ratio");
   text_property(ratio, "EventInputA", "a");
   text_property(ratio, "EventInputB", "b");
   post_read(ratio, nullptr);
   hud_number_math_update();
   float_event("b", 2); assert(output_values("ratio").empty());
   float_event("a", 8); assert(output_values("ratio").back() == 4);
   float_event("b", 4); assert(output_values("ratio").back() == 2);
   float_event("b", 0); assert(output_values("ratio").size() == 2);
   float_event("a", 4); assert(output_values("ratio").size() == 2);
   float_event("b", 4); assert(output_values("ratio").back() == 1);

   // Signed/unsigned payloads are inline DWORDs too, not pointers or floats.
   fake_create(2, "%s", "signed"); fake_create(3, "%s", "unsigned");
   Shell* mixed = math("Add", "mixed");
   text_property(mixed, "EventInputA", "signed");
   text_property(mixed, "EventInputB", "unsigned");
   post_read(mixed, nullptr); hud_number_math_update();
   Event negative{fake_find_cdecl(hash("signed")), 0xffffffff};
   Event positive{fake_find_cdecl(hash("unsigned")), 3};
   fake_send(&negative, nullptr); fake_send(&positive, nullptr);
   assert(output_values("mixed").back() == 2);

   Shell* clamped = math("Multiply", "clamped");
   text_property(clamped, "EventInputA", "a");
   constant(clamped, "ConstantB", 2);
   Data clamp = number_data("Clamp", 0);
   clamp.count = 2;
   const float one = 1;
   std::memcpy(&clamp.args[1], &one, 4);
   property(clamped, clamp);
   post_read(clamped, nullptr); hud_number_math_update();
   float_event("a", .25f); assert(output_values("clamped").back() == .5f);
   float_event("a", 4); assert(output_values("clamped").back() == 1);
   float_event("a", -1); assert(output_values("clamped").back() == 0);

   // A comment after a property on its line is dropped, after both of Clamp's.
   Shell* commentedClamp = math("Multiply", "commented.clamp");
   text_property(commentedClamp, "EventInputA", "a");
   constant(commentedClamp, "ConstantB", 2);
   property(commentedClamp, with_comment(clamp, "// keep it 0 to 1"));
   post_read(commentedClamp, nullptr); hud_number_math_update();
   assert(!node(commentedClamp).invalid);
   float_event("a", 4); assert(output_values("commented.clamp").back() == 1);

   // OutputIsAlpha sends a Math result every update too; without it, only on
   // change (the "chain.result" checks above).
   Shell* alpha = math("Multiply", "math.alpha");
   text_property(alpha, "EventInputA", "a");
   constant(alpha, "ConstantB", .5f);
   constant(alpha, "OutputIsAlpha", 1);
   post_read(alpha, nullptr); hud_number_math_update();
   assert(output_values("math.alpha").empty());               // nothing yet to repeat
   float_event("a", 1);
   hud_number_math_update(); hud_number_math_update();
   assert(output_values("math.alpha").size() == 3 && output_values("math.alpha").back() == .5f);

   // Identical source in both slots must not produce a one-callback glitch.
   Shell* same = math("Subtract", "same");
   text_property(same, "EventInputA", "a");
   text_property(same, "EventInputB", "a");
   post_read(same, nullptr); hud_number_math_update();
   float_event("a", 17); float_event("a", 30);
   assert(output_values("same").size() == 1 && output_values("same").back() == 0);

   // Invalid configuration fails closed, not as an accidentally active zero.
   Shell* bad = math("Divide", "bad");
   constant(bad, "ConstantA", 1);
   constant(bad, "ConstantA", 2);
   constant(bad, "ConstantB", 1);
   post_read(bad, nullptr); assert(node(bad).invalid);
   Shell* missing = math("Add", "missing");
   constant(missing, "ConstantA", 0);
   text_property(missing, "EventInputB", "undefined");
   post_read(missing, nullptr); assert(node(missing).invalid);
   fake_create(1, "%s", "boolean");
   Shell* boolean = math("Add", "bad.boolean");
   constant(boolean, "ConstantA", 0);
   text_property(boolean, "EventInputB", "boolean");
   post_read(boolean, nullptr); assert(node(boolean).invalid);
   Shell* duplicate = math("Add", "ratio"); assert(node(duplicate).invalid);
   Shell* self = math("Add", "self");
   constant(self, "ConstantA", 1);
   text_property(self, "EventInputB", "self");
   post_read(self, nullptr); assert(node(self).invalid);
   Shell* badOp = math("Typo", "typo"); assert(node(badOp).invalid);
   Shell* absent = create(nullptr, nullptr, "absent", nullptr, nullptr);
   post_read(absent, nullptr); assert(node(absent).invalid);
   Shell* wrongType = math("Add", "wrong.type");
   text_property(wrongType, "ConstantA", "1"); assert(node(wrongType).invalid);
   Shell* unknown = math("Add", "unknown");
   constant(unknown, "NumberVector3", 3); assert(node(unknown).invalid);
   Shell* badClamp = math("Add", "bad.clamp");
   clamp.args[0] = 0x40000000; // 2 > 1
   property(badClamp, clamp); assert(node(badClamp).invalid);

   dispatchDepth = 32;
   const auto limitedCount = output_values("ratio").size();
   const double prior = node(ratio).state.values[0];
   float_event("a", 400);
   assert(output_values("ratio").size() == limitedCount);
   assert(node(ratio).state.values[0] == prior);
   dispatchDepth = 0;
   float_event("a", 16); assert(output_values("ratio").back() == 4);

   // Native player filtering must apply to output names as well as inputs.
   playerFilter = "player2";
   fake_create(4, "%s", "player2.weapon1.heat");
   Shell* filtered = math("Subtract", "player1.filtered");
   constant(filtered, "ConstantA", 1);
   text_property(filtered, "EventInputB", "player1.weapon1.heat");
   post_read(filtered, nullptr); hud_number_math_update();
   float_event("player2.weapon1.heat", .75f);
   assert(output_values("player2.filtered").back() == .25f);

   // A native transform could still feed back indirectly; simulate that wiring.
   events.at(hash("chain.result")).handlers.push_back(&initial->inputA);
   initial->inputA.cls = chain->output;
   node(initial).state.input(0, true, 5);
   node(initial).pending = true;
   hud_number_math_update();
   assert(dispatchDepth == 0 && !node(initial).busy && node(initial).warnedRuntime);
   const auto feedbackCount = output_values("constant.result").size();
   hud_number_math_update();
   assert(output_values("constant.result").size() == feedbackCount);

   factoryFails = true;
   assert(!create(nullptr, nullptr, "allocation failure", nullptr, nullptr));
   factoryFails = false;
   Shell* deleting = math("Add", "deleting");
   destroy(deleting, nullptr, 1); // Native scalar deleting destructor path.
   clear_mission();
   // Repeated mission teardown/reload leaves no handler or cached operand alive.
   for (unsigned i = 0; i < 100; ++i) {
      Shell* s = math("Add", "reload");
      constant(s, "ConstantA", 2); constant(s, "ConstantB", 4);
      post_read(s, nullptr); hud_number_math_update();
      assert(output_values("reload").size() == 1 && output_values("reload").back() == 6);
      clear_mission();
   }
}

void lerp_core_tests()
{
   float result = 0;
   Lerp lerp;
   assert(!lerp.next(result) && !lerp.advance(1));         // nothing until the first input
   lerp.riseTime = .5f;
   lerp.fallTime = .25f;
   lerp.input(true, 1);                                     // the first value is taken at once
   assert(lerp.next(result) && result == 1 && !lerp.next(result));
   lerp.input(true, 0);
   assert(!lerp.next(result));                              // falls over FallTime
   assert(lerp.advance(.125) && lerp.next(result) && result == .5f);
   assert(lerp.advance(1) && lerp.next(result) && result == 0);   // stops at the input
   assert(!lerp.advance(1) && !lerp.next(result));
   lerp.input(true, 7);                                     // clamped to 0..1
   assert(lerp.advance(.25) && lerp.next(result) && result == .5f);
   lerp.input(true, 0);                                     // turns round mid-way
   assert(lerp.advance(.0625) && lerp.next(result) && result == .25f);
   lerp.input(false, 1);                                    // an invalid input holds
   assert(!lerp.advance(1));
   lerp.input(true, NAN);
   assert(!lerp.advance(1));
   lerp.input(true, -3);                                    // recovers, clamped to 0
   assert(lerp.advance(1) && lerp.next(result) && result == 0);
   lerp.input(true, 1);
   assert(!lerp.advance(0) && !lerp.advance(-1) && !lerp.next(result));

   // RiseTime 0: a pulse one update long still shows in full, then fades.
   Lerp pulse;
   pulse.fallTime = .5f;
   pulse.input(true, 0);
   assert(pulse.next(result) && result == 0);
   pulse.input(true, 1);
   assert(pulse.next(result) && result == 1);
   pulse.input(true, 0);
   assert(!pulse.next(result));
   assert(pulse.advance(.25) && pulse.next(result) && result == .5f);

   // A and B: either order, and nothing while either is unknown.
   Lerp ends;
   ends.end(0, true, 10);
   ends.end(1, true, 20);
   ends.input(true, .25);
   assert(ends.next(result) && result == 12.5f);
   ends.end(1, false, 0);
   ends.input(true, 1);
   assert(!ends.next(result));
   ends.end(1, true, -10);
   assert(ends.next(result) && result == -10);
   ends.end(0, true, NAN);
   assert(!ends.next(result));
   ends.end(2, true, 5);                                     // no third end
   ends.end(0, true, 10);
   assert(!ends.next(result));                               // unchanged: -10 again

   Lerp wide;                                               // the whole float range
   wide.end(0, true, -FLT_MAX);
   wide.end(1, true, FLT_MAX);
   wide.input(true, 1);
   assert(wide.next(result) && result == FLT_MAX);
   wide.input(true, .5);
   assert(wide.next(result) && result == 0);

   // unlink_handler mirrors EventClass::UnregisterEventHandler on the engine's
   // circular list of {next, prev} nodes at handler +0x08.
   struct Link { void* next; void* prev; } head{};
   Handler first{}, second{};
   void* h = &head;
   void* a = &first.next;
   void* b = &second.next;
   head = { a, b };
   first.next = b;  first.prev = h;
   second.next = h; second.prev = a;
   first.cls = second.cls = reinterpret_cast<EventClass*>(&head);
   unlink_handler(&first);
   assert(head.next == b && head.prev == b && second.next == h && second.prev == h);
   assert(!first.next && !first.prev && !first.cls);
   unlink_handler(&first);                                  // unbound: nothing to unlink
   assert(head.next == b && second.prev == h);
   unlink_handler(&second);
   assert(head.next == h && head.prev == h && !second.cls);
}

void comment_tests()
{
   // A comment after a property reaches the reader as more arguments, from a
   // hashed "//" on: 0xA2D266E3 is the value ConfigMunge wrote for it.
   static_assert(hash("//") == 0xA2D266E3u);
   float value = 0;
   const Data rise = with_comment(number_data("RiseTime", .15f), "// fade out as the sprint starts");
   assert(rise.count == 8 && rise.arguments() == 1 && rise.number(0, value) && value == .15f);
   const Data output = with_comment(text_data("EventOutput", "a.b"), "// the \"output\"");
   assert(output.arguments() == 1 && std::string(output.string(0)) == "a.b");
   assert(with_comment(number_data("RiseTime", .15f), "//fade").arguments() == 2);
   assert(number_data("RiseTime", .15f).arguments() == 1);
   const Data empty{hash("ConstantA"), 0, {}};
   assert(empty.arguments() == 0);

   // Flags, as ConfigMunge writes them: 1 and 0 as floats, true and false
   // hashed whatever their case, "1" as a string.
   static_assert(hash("true") == 0x4DB211E5u && hash("false") == 0x0B069958u);
   bool flag = false;
   assert(number_data("OutputIsAlpha", 1).flag(0, flag) && flag);
   assert(number_data("OutputIsAlpha", 0).flag(0, flag) && !flag);
   assert(number_data("OutputIsAlpha", .5f).flag(0, flag) && flag);
   assert(word_data("OutputIsAlpha", "TRUE").flag(0, flag) && flag);
   assert(word_data("OutputIsAlpha", "false").flag(0, flag) && !flag);
   assert(!text_data("OutputIsAlpha", "1").flag(0, flag));
   assert(!number_data("OutputIsAlpha", NAN).flag(0, flag));
   assert(!empty.flag(0, flag));
}

void range_and_compare_core_tests()
{
   float result = 0;
   // InputRange: where the input sits between its ends, either way round.
   Lerp ammo;
   ammo.bound(0, true, 0);
   ammo.bound(1, true, 30);
   ammo.input(true, 15);
   assert(ammo.next(result) && result == .5f);
   ammo.input(true, 45);                                    // past the end: B
   assert(ammo.next(result) && result == 1);
   Lerp reversed;
   reversed.bound(0, true, 30);
   reversed.bound(1, true, 0);
   reversed.input(true, 6);
   assert(reversed.next(result) && result == .8f);

   // An end not known yet, or a range with no width, holds. The first place
   // after that is still taken at once; a later range change is eased.
   Lerp later;
   later.riseTime = 1;
   later.bound(1, false, 0);
   later.input(true, 5);
   assert(!later.next(result) && !later.advance(1));
   later.bound(1, true, 10);
   assert(later.next(result) && result == .5f);
   later.bound(1, true, 5);                                 // 5 of 5: the top
   assert(!later.next(result));
   assert(later.advance(.25) && later.next(result) && result == .75f);
   later.bound(0, true, 5);                                 // no width
   assert(!later.advance(1));

   Comparison op;
   assert(parse_comparison("lessOrEqual", op) && op == Comparison::LessOrEqual);
   assert(parse_comparison("NOTEQUAL", op) && op == Comparison::NotEqual);
   assert(!parse_comparison("Less Or Equal", op) && !parse_comparison("<", op));
   assert(!parse_comparison("Add", op) && !parse_comparison(nullptr, op));

   // Every comparison with A = 2 against B = 1, 2 and 3.
   const bool expected[6][3] = {
      { true,  false, false },   // Greater
      { true,  true,  false },   // GreaterOrEqual
      { false, false, true  },   // Less
      { false, true,  true  },   // LessOrEqual
      { false, true,  false },   // Equal
      { true,  false, true  },   // NotEqual
   };
   for (int o = 0; o < 6; ++o) {
      for (int b = 1; b <= 3; ++b) {
         Compare c;
         c.op = static_cast<Comparison>(o);
         c.input(0, true, 2);
         c.input(1, true, b);
         bool on = !expected[o][b - 1];
         assert(c.next(on) && on == expected[o][b - 1] && !c.next(on));
      }
   }

   // Hysteresis: low health turns on under 0.25 and off only past 0.30.
   Compare low;
   low.op = Comparison::Less;
   low.hysteresis = .05f;
   low.input(1, true, .25);
   bool on = true;
   assert(!low.next(on));                  // A not known yet
   low.input(0, true, 1);
   assert(low.next(on) && !on);            // the first result
   low.input(0, true, .2);
   assert(low.next(on) && on);
   low.input(0, true, .28);
   assert(!low.next(on));                  // inside the margin: still on
   low.input(0, true, .31);
   assert(low.next(on) && !on);
   low.input(0, true, .26);
   assert(!low.next(on));                  // turning on needs the line itself
   low.input(0, false, 0);
   assert(!low.next(on) && !low.on);       // an invalid side holds

   // For Equal and NotEqual it is how close counts as equal.
   Compare equal;
   equal.op = Comparison::Equal;
   equal.hysteresis = .5f;
   equal.input(0, true, 10.4);
   equal.input(1, true, 10);
   assert(equal.next(on) && on);
   equal.input(0, true, 10.6);
   assert(equal.next(on) && !on);
}

Shell* lerp_node(const char* input, const char* output)
{
   Shell* shell = create_lerp(nullptr, nullptr, output, nullptr, nullptr);
   assert(shell && node(shell).kind == Kind::Lerp);
   if (input) text_property(shell, "EventInput", input);
   text_property(shell, "EventOutput", output);
   return shell;
}

void lerp_adapter_tests(bool mt)
{
   setup(mt);
   // The sprint recipe: 1 while walking, easing to 0 over RiseTime once sprinting.
   fake_create(4, "%s", "player1.unit.state.sprint");
   Shell* alpha = lerp_node("player1.unit.state.sprint", "player1.sprintAlpha");
   constant(alpha, "ConstantA", 1);
   constant(alpha, "ConstantB", 0);
   constant(alpha, "RiseTime", .25f);
   constant(alpha, "FallTime", .5f);
   post_read(alpha, nullptr);
   assert(node(alpha).ready);
   update(.1f);
   assert(output_values("player1.sprintAlpha").empty());
   float_event("player1.unit.state.sprint", 0);
   assert(output_values("player1.sprintAlpha").back() == 1);
   float_event("player1.unit.state.sprint", 1);
   assert(output_values("player1.sprintAlpha").size() == 1);   // eases, no jump
   update(.125f);
   assert(output_values("player1.sprintAlpha").back() == .5f);
   update(.125f);
   assert(output_values("player1.sprintAlpha").back() == 0);
   update(.125f);
   assert(output_values("player1.sprintAlpha").size() == 3);   // settled: no resend
   float_event("player1.unit.state.sprint", 0);
   update(.25f);
   assert(output_values("player1.sprintAlpha").back() == .5f);
   update(1);
   assert(output_values("player1.sprintAlpha").back() == 1);

   // A one-update pulse (land, shot) with RiseTime 0 still shows, then fades.
   fake_create(4, "%s", "player1.unit.state.land");
   Shell* flash = lerp_node("player1.unit.state.land", "player1.landFlash");
   constant(flash, "FallTime", .5f);
   post_read(flash, nullptr);
   update(0);
   float_event("player1.unit.state.land", 0);
   float_event("player1.unit.state.land", 1);
   float_event("player1.unit.state.land", 0);
   assert(output_values("player1.landFlash").size() == 2 && output_values("player1.landFlash").back() == 1);
   update(.25f);
   assert(output_values("player1.landFlash").back() == .5f);

   // A and B from events: nothing until both arrive. B has the DLL's own handler.
   fake_create(4, "%s", "t"); fake_create(4, "%s", "lo"); fake_create(2, "%s", "hi");
   Shell* range = lerp_node("t", "range");
   text_property(range, "EventInputA", "lo");
   text_property(range, "EventInputB", "hi");
   post_read(range, nullptr);
   update(0);
   float_event("t", .5f);
   float_event("lo", 10);
   assert(output_values("range").empty());
   Event hi{fake_find_cdecl(hash("hi")), 20};               // an Int event
   fake_send(&hi, nullptr);
   assert(output_values("range").back() == 15);
   float_event("lo", 0);
   assert(output_values("range").back() == 10);

   // One event driving the input and an end updates both in one callback.
   Shell* shared = lerp_node("t", "shared");
   text_property(shared, "EventInputB", "t");               // t * t
   post_read(shared, nullptr);
   update(0);
   float_event("t", .5f);
   assert(output_values("shared").size() == 1 && output_values("shared").back() == .25f);

   // Math nodes in the same list do not move with time.
   Shell* sum = math("Add", "sum");
   text_property(sum, "EventInputA", "t");
   constant(sum, "ConstantB", 1);
   post_read(sum, nullptr);
   update(0);
   float_event("t", 1);
   update(5);
   assert(output_values("sum").size() == 1 && output_values("sum").back() == 2);

   // Invalid configurations fail closed.
   Shell* noInput = lerp_node(nullptr, "no.input");
   post_read(noInput, nullptr); assert(node(noInput).invalid);
   Shell* noOutput = create_lerp(nullptr, nullptr, "no.output", nullptr, nullptr);
   text_property(noOutput, "EventInput", "t");
   post_read(noOutput, nullptr); assert(node(noOutput).invalid);
   Shell* negative = lerp_node("t", "negative.time");
   constant(negative, "FallTime", -1); assert(node(negative).invalid);
   Shell* textTime = lerp_node("t", "text.time");
   text_property(textTime, "RiseTime", "1"); assert(node(textTime).invalid);
   Shell* twice = lerp_node("t", "twice");
   constant(twice, "ConstantA", 1);
   text_property(twice, "EventInputA", "lo"); assert(node(twice).invalid);
   Shell* operation = lerp_node("t", "lerp.operation");
   text_property(operation, "Operation", "Add"); assert(node(operation).invalid);
   fake_create(1, "%s", "flag");
   Shell* boolean = lerp_node("flag", "lerp.boolean");
   post_read(boolean, nullptr); assert(node(boolean).invalid);
   Shell* loop = lerp_node("t", "loop");
   text_property(loop, "EventInputB", "loop");
   post_read(loop, nullptr); assert(node(loop).invalid);
   const auto before = output_values("range").size();
   float_event("t", .25f);
   assert(output_values("range").size() == before + 1);      // valid nodes still run

   // A comment after a property on its line is dropped. One with no space after
   // its "//" is a single hashed word, which cannot be told from an argument.
   Shell* commented = create_lerp(nullptr, nullptr, "commented", nullptr, nullptr);
   property(commented, with_comment(text_data("EventInput", "t"), "// the sprint"));
   property(commented, with_comment(number_data("RiseTime", .15f), "// fade out as the sprint starts"));
   property(commented, with_comment(number_data("FallTime", .3f), "// 0.5 seconds"));
   property(commented, with_comment(text_data("EventOutput", "commented"), "// out"));
   post_read(commented, nullptr);
   assert(!node(commented).invalid);
   assert(node(commented).lerp.riseTime == .15f && node(commented).lerp.fallTime == .3f);
   Shell* spaceless = lerp_node("t", "spaceless");
   property(spaceless, with_comment(number_data("RiseTime", .15f), "//fade"));
   assert(node(spaceless).invalid);

   // OutputIsAlpha: a group's EventAlpha lasts one frame, so the value is sent
   // every update, held or not, and only once there is one.
   fake_create(4, "%s", "held.input");
   Shell* held = lerp_node("held.input", "held");
   property(held, with_comment(number_data("OutputIsAlpha", 1), "// for EventAlpha"));
   constant(held, "RiseTime", .5f);
   post_read(held, nullptr);
   assert(!node(held).invalid && node(held).outputIsAlpha);
   update(0);
   update(0);
   assert(output_values("held").empty());
   float_event("held.input", 0);
   update(.25f);
   update(.25f);
   assert(output_values("held").size() == 3 && output_values("held").back() == 0);
   float_event("held.input", 1);
   update(.25f);
   assert(output_values("held").size() == 4 && output_values("held").back() == .5f);
   update(.25f);
   update(.25f);
   assert(output_values("held").size() == 6 && output_values("held").back() == 1);

   // The flag takes a number or an unquoted true or false; a hashed false must
   // not read as on, as it does for the engine's GetBoolArg.
   Shell* off = lerp_node("held.input", "off");
   property(off, word_data("OutputIsAlpha", "false"));
   assert(!node(off).invalid && !node(off).outputIsAlpha);
   Shell* on = lerp_node("held.input", "on");
   property(on, word_data("OutputIsAlpha", "True"));
   assert(!node(on).invalid && node(on).outputIsAlpha);
   Shell* quoted = lerp_node("held.input", "quoted");
   text_property(quoted, "OutputIsAlpha", "1");
   assert(node(quoted).invalid);
   Shell* twiceAlpha = lerp_node("held.input", "twice.alpha");
   constant(twiceAlpha, "OutputIsAlpha", 1);
   constant(twiceAlpha, "OutputIsAlpha", 0);
   assert(node(twiceAlpha).invalid);

   // Teardown unbinds B's handler too (clear_mission checks none are left).
   Shell* deleting = lerp_node("t", "deleting");
   text_property(deleting, "EventInputB", "lo");
   const Handler* endB = &node(deleting).endB;
   const auto& lo = events.at(hash("lo")).handlers;
   assert(std::count(lo.begin(), lo.end(), endB) == 1);
   destroy(deleting, nullptr, 1);
   assert(std::count(lo.begin(), lo.end(), endB) == 0);
   clear_mission();
   for (unsigned i = 0; i < 100; ++i) {
      fake_create(4, "%s", "lo");
      Shell* s = lerp_node("lo", "reload");
      text_property(s, "EventInputB", "lo");
      post_read(s, nullptr); update(0);
      float_event("lo", .5f);
      assert(output_values("reload").size() == 1 && output_values("reload").back() == .25f);
      clear_mission();
   }
}

void range_adapter_tests(bool mt)
{
   setup(mt);
   // InputRange with numbers: a clip of 30 goes straight in.
   fake_create(3, "%s", "clip");                             // a Uint count
   Shell* fill = lerp_node("clip", "fill");
   property(fill, args_data("InputRange", { { 0, nullptr }, { 30, nullptr } }));
   post_read(fill, nullptr);
   assert(!node(fill).invalid);
   update(0);
   Event clip{fake_find_cdecl(hash("clip")), 15};
   fake_send(&clip, nullptr);
   assert(output_values("fill").back() == .5f);

   // With an event for an end, the range can change, as a clip size does with
   // the weapon. The end's name is the property's second argument.
   fake_create(3, "%s", "clip.size");
   Shell* share = lerp_node("clip", "share");
   property(share, args_data("InputRange", { { 0, nullptr }, { 0, "clip.size" } }));
   post_read(share, nullptr);
   assert(!node(share).invalid);
   update(0);
   fake_send(&clip, nullptr);
   assert(output_values("share").empty());                   // the size has not arrived
   Event size{fake_find_cdecl(hash("clip.size")), 60};
   fake_send(&size, nullptr);
   assert(output_values("share").back() == .25f);            // 15 of 60
   size.bits = 20;
   fake_send(&size, nullptr);
   assert(output_values("share").back() == .75f);            // 15 of 20, at once with no times

   fake_create(4, "%s", "low"); fake_create(4, "%s", "high");
   Shell* both = lerp_node("clip", "both");
   property(both, args_data("InputRange", { { 0, "low" }, { 0, "high" } }));
   post_read(both, nullptr);
   update(0);
   float_event("low", 10);
   float_event("high", 20);
   fake_send(&clip, nullptr);
   assert(output_values("both").back() == .5f);

   Shell* one = lerp_node("clip", "range.one");
   property(one, args_data("InputRange", { { 5, nullptr } }));
   assert(node(one).invalid);
   Shell* flat = lerp_node("clip", "range.flat");
   property(flat, args_data("InputRange", { { 5, nullptr }, { 5, nullptr } }));
   assert(node(flat).invalid);
   Shell* missing = lerp_node("clip", "range.missing");
   property(missing, args_data("InputRange", { { 0, nullptr }, { 0, "no.such.event" } }));
   assert(node(missing).invalid);
   Shell* onMath = math("Add", "range.math");
   property(onMath, args_data("InputRange", { { 0, nullptr }, { 1, nullptr } }));
   assert(node(onMath).invalid);                             // a lerp property only
   clear_mission();                                          // range handlers unbound too
}

Shell* compare_node(const char* operation, const char* output)
{
   Shell* shell = create_compare(nullptr, nullptr, output, nullptr, nullptr);
   assert(shell && node(shell).kind == Kind::Compare);
   text_property(shell, "Operation", operation);
   return shell;
}

void compare_adapter_tests(bool mt)
{
   setup(mt);
   // Low health: on under 0.25, off only past 0.30, as a Float and Bool events.
   fake_create(4, "%s", "hp");
   Shell* low = compare_node("Less", "low");
   text_property(low, "EventInputA", "hp");
   constant(low, "ConstantB", .25f);
   constant(low, "Hysteresis", .05f);
   text_property(low, "EventOutput", "low");
   text_property(low, "EventOutputTrue", "low.on");
   text_property(low, "EventOutputFalse", "low.off");
   post_read(low, nullptr);
   assert(!node(low).invalid);
   assert(events.at(hash("low")).base.type == 4 && events.at(hash("low.on")).base.type == 1);
   update(0);
   assert(output_values("low").empty() && events.at(hash("low.off")).sends == 0);   // no A yet
   float_event("hp", 1);
   assert(output_values("low").back() == 0 && events.at(hash("low.off")).sends == 1);
   float_event("hp", .2f);
   assert(output_values("low").back() == 1 && events.at(hash("low.on")).sends == 1);
   float_event("hp", .28f);
   float_event("hp", .15f);
   assert(output_values("low").size() == 2 && events.at(hash("low.on")).sends == 1);
   float_event("hp", .31f);
   assert(output_values("low").back() == 0 && events.at(hash("low.off")).sends == 2);
   update(0);
   assert(output_values("low").size() == 3);                 // change-only by default

   // OutputIsAlpha repeats the Float every update, never the Bool events.
   Shell* alpha = compare_node("Greater", "cmp.alpha");
   text_property(alpha, "EventInputA", "hp");
   constant(alpha, "ConstantB", .5f);
   text_property(alpha, "EventOutput", "cmp.alpha");
   text_property(alpha, "EventOutputTrue", "cmp.alpha.on");
   constant(alpha, "OutputIsAlpha", 1);
   post_read(alpha, nullptr);
   update(0);
   float_event("hp", .9f);
   update(0);
   update(0);
   assert(output_values("cmp.alpha").size() == 3 && events.at(hash("cmp.alpha.on")).sends == 1);

   // Constants alone publish on the first update, as Math's do.
   Shell* always = compare_node("Equal", "always");
   constant(always, "ConstantA", 1);
   constant(always, "ConstantB", 1);
   text_property(always, "EventOutput", "always");
   post_read(always, nullptr);
   update(0);
   assert(output_values("always").size() == 1 && output_values("always").back() == 1);

   // Bool events alone are enough; no output at all is not.
   Shell* boolOnly = compare_node("Equal", "bool.only");
   text_property(boolOnly, "EventInputA", "hp");
   constant(boolOnly, "ConstantB", 0);
   text_property(boolOnly, "EventOutputTrue", "dead");
   post_read(boolOnly, nullptr);
   assert(!node(boolOnly).invalid);
   Shell* silent = compare_node("Equal", "silent");
   constant(silent, "ConstantA", 0);
   constant(silent, "ConstantB", 0);
   post_read(silent, nullptr);
   assert(node(silent).invalid);

   Shell* typo = compare_node("Lesser", "typo");
   assert(node(typo).invalid);
   Shell* mathOp = compare_node("Add", "math.op");
   assert(node(mathOp).invalid);
   Shell* negative = compare_node("Less", "negative.hysteresis");
   constant(negative, "Hysteresis", -1);
   assert(node(negative).invalid);
   Shell* lerpKey = compare_node("Less", "lerp.key");
   constant(lerpKey, "RiseTime", 1);
   assert(node(lerpKey).invalid);
   Shell* sameName = compare_node("Less", "same.name");
   text_property(sameName, "EventOutput", "same.name.out");
   text_property(sameName, "EventOutputTrue", "same.name.out");
   assert(node(sameName).invalid);
   Shell* loop = compare_node("Less", "loop");
   text_property(loop, "EventOutput", "loop");
   text_property(loop, "EventInputA", "loop");
   constant(loop, "ConstantB", 1);
   post_read(loop, nullptr);
   assert(node(loop).invalid);
   clear_mission();
}
} // namespace

int main()
{
   core_tests();
   adapter_tests(true);
   adapter_tests(false);
   lerp_core_tests();
   comment_tests();
   range_and_compare_core_tests();
   lerp_adapter_tests(true);
   lerp_adapter_tests(false);
   range_adapter_tests(true);
   range_adapter_tests(false);
   compare_adapter_tests(true);
   compare_adapter_tests(false);
   assert(destroyed > 400);
   std::puts("HUD NumberMath: arithmetic, parser, trailing comments, lerp easing, ends, ranges and resends, comparisons with hysteresis, both ABIs, event chains, feedback, and 400 reloads passed.");
}
