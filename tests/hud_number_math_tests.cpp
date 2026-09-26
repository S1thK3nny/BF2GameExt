// Standalone Win32 test; compiles the real adapter against fake native callbacks.
// No game, DLL build, Detours, or full Visual Studio solution required.
#define HUD_NUMBER_MATH_TEST
#include "../PatcherDLL/src/render/hud_number_math.cpp"
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
} // namespace

int main()
{
   core_tests();
   adapter_tests(true);
   adapter_tests(false);
   assert(destroyed > 200);
   std::puts("HUD NumberMath: arithmetic, parser, both ABIs, event chains, feedback, and 200 reloads passed.");
}
