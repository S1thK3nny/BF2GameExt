// pch.h must be the first line and unconditional: with /Yu MSVC discards every
// token up to this include, so an #ifndef wrapped around it leaves its #endif
// orphaned (C1020).  The standalone tests put PatcherDLL/src/core (pch.h) and
// PatcherDLL/src (util/) on the include path and get the same header.
#include "pch.h"
#ifndef HUD_NUMBER_MATH_TEST
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#endif
#include "hud_number_math.hpp"
#include "hud_number_math_core.hpp"

#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <new>

namespace {
using namespace hud_number_math;

// Engine-allocated TransformNumberVector3 shell: two input handlers (+1C/+40),
// native Item/Transform list membership and destruction. No mappings are built;
// Item::Read replaces Vector3::Read. Arithmetic lives in a DLL-owned sidecar.
// DestroyAll calls virtual dtor(0), THEN HUD::Delete. DLL-new for the shell would
// cross heaps, even with a custom deleting destructor. See docs/RE/HUDSystem.md.
struct EventClass { uint32_t hash, type; };
struct Event { EventClass* cls; uint32_t bits; };
struct Handler {
   void(__cdecl* callback)(const Event*, void*);
   void* data;
   void* next;
   void* prev;
   EventClass* cls;
};
struct Shell {
   void** vtable;
   uint32_t nameHash;
   const char* name;
   const char* displayName;
   void* factory;
   void* itemNode;
   uint32_t flags;
   Handler inputA;
   EventClass* output;
   void* transformNode;
   float factorConstant, factor;
   Handler inputB;
   uint32_t wrap;
   void* mappings;
   uint32_t mappingCount;
};
static_assert(sizeof(Handler) == 20 && sizeof(Shell) == 96, "Native x86 HUD ABI");
static_assert(offsetof(Shell, inputA) == 0x1c && offsetof(Shell, output) == 0x30
              && offsetof(Shell, inputB) == 0x40, "Native handler offsets");

using CreateItem = Shell*(__fastcall*)(void*, void*, const char*, void*, void*);
using DestroyItem = void*(__fastcall*)(Shell*, void*, unsigned);
using FactoryCtor = void*(__fastcall*)(void*, void*, uint32_t);
using Send = void(__fastcall*)(Event*, void*);
using NativeCreateEvent = EventClass*(__cdecl*)(int, const char*, ...);
// The HUD editor's .hud writer (modtools): the indent writer is thiscall(count)
// on the file, RET 4; the format writer cdecl(file, format, ...).
using WriteIndent = void(__fastcall*)(void* file, void*, unsigned count);
using WriteFormat = void(__cdecl*)(void* file, const char* format, ...);

enum class Kind { Math, Lerp, Compare };

constexpr int kBool = 1;    // HUD::EventClass::Type
constexpr int kFloat = 4;
constexpr unsigned kMaxLines = 16;   // more than any kind has properties

// TransformNumberLerp has up to five event inputs where the shell has two
// handlers: the shell's inputA carries EventInput and inputB EventInputA, and
// B's event and InputRange's get handlers of their own here, bound through the
// same Item::ReadEvent and unbound in destroy before the native destructor runs.
struct Node {
   Shell* shell = nullptr;
   Node* next = nullptr;
   Kind kind = Kind::Math;
   State state;
   Lerp lerp;
   Compare compare;
   Handler endB{};
   Handler rangeEnd[2]{};
   EventClass* outputTrue = nullptr;    // Compare's Bool events, sent as the result turns
   EventClass* outputFalse = nullptr;
   bool outputIsAlpha = false;   // OutputIsAlpha: send every update, not only on change
   unsigned fields = 0;
   unsigned failures = 0;        // fail() calls, to tell which properties were read cleanly
   char* lines[kMaxLines]{};     // what the HUD editor writes back, in the order read
   unsigned lineCount = 0;
   bool invalid = false;
   bool ready = false;
   bool active = false;
   bool busy = false;
   bool pending = true;
   bool warnedRuntime = false;
};

Node* nodes = nullptr;
unsigned dispatchDepth = 0;
bool resolved = false;
bool modtools = false;
void* factoryAlloc = nullptr;
FactoryCtor factoryCtor = nullptr;
CreateItem createItem = nullptr;
DestroyItem destroyItem = nullptr;
void* readEvent = nullptr;
void* filterName = nullptr;
void* findEvent = nullptr;
NativeCreateEvent createEvent = nullptr;
Send sendEvent = nullptr;
WriteIndent writeIndent = nullptr;
WriteFormat writeFormat = nullptr;
void* factoryVtable[2]{};
void* lerpFactoryVtable[2]{};
void* compareFactoryVtable[2]{};
void* itemVtable[12]{};

// EventClass::UnregisterEventHandler (Phantom 0x0060F870) and EventHandler's
// destructor (modtools 0x006AD6B0, Steam 0x0055DB70, GOG 0x0055E8F0) are a plain
// unlink of the handler's {next, prev} node at +0x08: done here for the one
// handler the native destructor does not know about.
void unlink_handler(Handler* h)
{
   if (h->next && h->prev) {
      *reinterpret_cast<void**>(static_cast<char*>(h->next) + 4) = h->prev;   // next->prev
      *reinterpret_cast<void**>(h->prev) = h->next;                            // prev->next
   }
   h->next = h->prev = nullptr;
   h->cls = nullptr;
}
void (*unbindHandler)(Handler*) = unlink_handler;   // the tests swap in their own

// The longest step a lerp takes from one HUD update: a hitch or a pause must
// not jump it.
constexpr float kMaxTick = 0.1f;

void log(const char* format, ...)
{
   FILE* file = nullptr;
   if (fopen_s(&file, "BF2GameExt.log", "a") != 0 || !file) return;
   std::fputs("[HudNumberMath] ", file);
   va_list args;
   va_start(args, format);
   std::vfprintf(file, format, args);
   va_end(args);
   std::fputc('\n', file);
   std::fclose(file);
}

Node& node(Shell* shell) { return *static_cast<Node*>(shell->inputA.data); }
const char* name(const Node& n) { return n.shell->name ? n.shell->name : "unnamed"; }
void fail(Node& n, const char* message)
{
   log("%s: disabled: %s", name(n), message);
   n.invalid = true;
   ++n.failures;
}

EventClass* find(uint32_t id)
{
   if (modtools) return reinterpret_cast<EventClass*(__cdecl*)(uint32_t)>(findEvent)(id);
   return reinterpret_cast<EventClass*(__fastcall*)(uint32_t)>(findEvent)(id);
}

// Sends the node's result when it changed, and with `again` its last one even
// when it did not. OutputIsAlpha asks for that every update: a group's
// EventAlpha lasts one frame, because Element::Update rewrites the render
// element's alpha from the element's own fader every frame (modtools
// 0x006920F0), so a held value has to be sent again.
void publish(Node& n, bool again = false)
{
   if (!n.active || !n.ready || n.invalid || !(n.pending || again)) return;
   if (n.busy || dispatchDepth >= 32) {
      if (!n.warnedRuntime) log("%s: feedback/depth limit; update ignored", name(n));
      n.warnedRuntime = true;
      return;
   }
   n.pending = false;
   float result = 0.0f;
   bool changed = false, have = false;
   EventClass* twin = nullptr;   // Compare's Bool event for a new result
   switch (n.kind) {
   case Kind::Lerp:
      changed = n.lerp.next(result);
      have = n.lerp.sent;
      if (!changed) result = n.lerp.previous;
      break;
   case Kind::Compare: {
      bool on = false;
      changed = n.compare.next(on);
      have = n.compare.decided;
      result = n.compare.on ? 1.0f : 0.0f;
      if (changed) twin = on ? n.outputTrue : n.outputFalse;
      break;
   }
   default:
      changed = n.state.next(result);
      have = n.state.sent;
      if (!changed) result = n.state.previous;
   }
   // Only the Float repeats: an EventEnable restarts its fade each time it fires.
   if (!changed && !(again && have)) return;
   n.busy = true;
   ++dispatchDepth;
   if (n.shell->output) {
      Event event{ n.shell->output, 0 };
      std::memcpy(&event.bits, &result, sizeof(result));
      sendEvent(&event, nullptr);
   }
   if (twin) {
      Event event{ twin, 1 };
      sendEvent(&event, nullptr);
   }
   --dispatchDepth;
   n.busy = false;
}

void __cdecl input(const Event* event, void* context)
{
   Node& n = *static_cast<Node*>(context);
   if (n.invalid || !event || !event->cls) return;
   // Do not cache feedback either, or a later tick could restart it.
   if (n.busy || dispatchDepth >= 32) {
      if (!n.warnedRuntime) log("%s: feedback/depth limit; input ignored", name(n));
      n.warnedRuntime = true;
      return;
   }
   double value = 0;
   const bool acceptable = numeric_event(event->cls->type, event->bits, value);
   // Inputs bound to the SAME event must update together, not briefly publish
   // (new A op stale B) on the first of the native callbacks.
   switch (n.kind) {
   case Kind::Lerp:
      if (event->cls == n.shell->inputA.cls) n.lerp.input(acceptable, value);
      if (event->cls == n.shell->inputB.cls) n.lerp.end(0, acceptable, value);
      if (event->cls == n.endB.cls)          n.lerp.end(1, acceptable, value);
      if (event->cls == n.rangeEnd[0].cls)   n.lerp.bound(0, acceptable, value);
      if (event->cls == n.rangeEnd[1].cls)   n.lerp.bound(1, acceptable, value);
      break;
   case Kind::Compare:
      if (event->cls == n.shell->inputA.cls) n.compare.input(0, acceptable, value);
      if (event->cls == n.shell->inputB.cls) n.compare.input(1, acceptable, value);
      break;
   default:
      if (event->cls == n.shell->inputA.cls) n.state.input(0, acceptable, value);
      if (event->cls == n.shell->inputB.cls) n.state.input(1, acceptable, value);
   }
   n.pending = true;
   publish(n);
}

bool claim(Node& n, unsigned bit)
{
   if (n.fields & bit) { fail(n, "duplicate property or operand source"); return false; }
   n.fields |= bit;
   return true;
}

void bind(Node& n, const Data* data, Handler& handler)
{
   if (modtools) reinterpret_cast<bool(__cdecl*)(const Data*, Handler*)>(readEvent)(data, &handler);
   else reinterpret_cast<bool(__fastcall*)(const Data*, Handler*)>(readEvent)(data, &handler);
   if (!handler.cls) fail(n, "input event not found; define upstream transforms first");
   else if (handler.cls->type < 2 || handler.cls->type > 4)
      fail(n, "input must be Int, Uint or Float (not Bool, Vector3, etc.)");
}

bool name_ok(const char* text) { return text && *text && std::strlen(text) <= 240; }

// Item::ReadEvent reads its event name from argument 0 only (Phantom
// 0x00617E30), so an event a later argument names is bound through a copy of
// the property holding just that name.
Data one_argument(const Data& data, const char* text)
{
   Data single{ data.id, 1, { 4 } };
   std::memcpy(reinterpret_cast<char*>(single.args) + 4, text, std::strlen(text) + 1);
   return single;
}

// A new event this node writes. Never overwrite a stock event or allow two
// writers; that also stops two nodes building an indirect feedback graph.
EventClass* create_named(Node& n, const char* text, int type)
{
   if (!name_ok(text)) { fail(n, "output expects an event name (1..240 bytes)"); return nullptr; }
   char filtered[512]{};
   if (modtools) reinterpret_cast<void(__cdecl*)(const char*, char*, unsigned)>(filterName)(text, filtered, 511);
   else reinterpret_cast<void(__fastcall*)(const char*, char*)>(filterName)(text, filtered);
   filtered[511] = 0;
   if (find(hash(filtered))) { fail(n, "output event already exists; use a unique name"); return nullptr; }
   EventClass* cls = createEvent(type, "%s", filtered);
   if (!cls) fail(n, "could not create the output event");
   return cls;
}

void create_output(Shell* shell, Node& n, const char* text)
{
   shell->output = create_named(n, text, kFloat);
}

// OutputIsAlpha, on every kind: claim bit 64.
void read_output_is_alpha(Node& n, const Data* data, unsigned args)
{
   if (claim(n, 64) && (args != 1 || !data->flag(0, n.outputIsAlpha)))
      fail(n, "OutputIsAlpha expects 1 or 0 (or true or false)");
}

// TransformNumberLerp's properties. Claim bits: 1 EventInput, 2 A, 4 B,
// 8 EventOutput, 16 RiseTime, 32 FallTime, 64 OutputIsAlpha, 128 InputRange.
bool read_lerp(Shell* shell, Node& n, const Data* data)
{
   const uint32_t key = data->id;
   const unsigned args = data->arguments();
   const char* text = args == 1 ? data->string(0) : nullptr;
   float number;
   switch (key) {
   case hash("EventInput"):
      if (claim(n, 1)) {
         if (!name_ok(text)) fail(n, "input expects an event name (1..240 bytes)");
         else bind(n, data, shell->inputA);
      }
      break;
   case hash("ConstantA"):
   case hash("ConstantB"): {
      const unsigned end = key == hash("ConstantA") ? 0 : 1;
      if (claim(n, 2u << end)) {
         if (args != 1 || !data->number(0, number)) fail(n, "constant expects one finite number");
         else n.lerp.end(end, true, number);
      }
      break;
   }
   case hash("EventInputA"):
   case hash("EventInputB"): {
      const unsigned end = key == hash("EventInputA") ? 0 : 1;
      if (claim(n, 2u << end)) {
         if (!name_ok(text)) { fail(n, "input expects an event name (1..240 bytes)"); break; }
         n.lerp.known[end] = false;   // until its event first arrives
         bind(n, data, end ? n.endB : shell->inputB);
      }
      break;
   }
   case hash("RiseTime"):
   case hash("FallTime"): {
      const bool rise = key == hash("RiseTime");
      if (claim(n, rise ? 16 : 32)) {
         if (args != 1 || !data->number(0, number) || number < 0.0f)
            fail(n, "RiseTime and FallTime expect one number of seconds, 0 or more");
         else (rise ? n.lerp.riseTime : n.lerp.fallTime) = number;
      }
      break;
   }
   case hash("InputRange"):
      // Each end a number or the name of an Int, Uint or Float event.
      if (claim(n, 128)) {
         if (args != 2) { fail(n, "InputRange expects two ends, each a number or an event name"); break; }
         for (unsigned end = 0; end < 2 && !n.invalid; ++end) {
            if (const char* event = data->string(end)) {
               if (!name_ok(event)) { fail(n, "input expects an event name (1..240 bytes)"); break; }
               n.lerp.rangeKnown[end] = false;   // until its event first arrives
               const Data single = one_argument(*data, event);
               bind(n, &single, n.rangeEnd[end]);
            } else if (data->number(end, number)) {
               n.lerp.bound(end, true, number);
            } else {
               fail(n, "InputRange expects two ends, each a number or an event name");
            }
         }
         if (!n.invalid && !data->string(0) && !data->string(1) && n.lerp.range[0] == n.lerp.range[1])
            fail(n, "InputRange needs two different ends");
      }
      break;
   case hash("EventOutput"):
      if (claim(n, 8)) create_output(shell, n, text);
      break;
   case hash("OutputIsAlpha"):
      read_output_is_alpha(n, data, args);
      break;
   default:
      log("%s: unrecognised property 0x%08X", name(n), key);
      fail(n, "unsupported property");
      return false;
   }
   return true;
}

// TransformNumberCompare's properties. Claim bits: 1 Operation, 2 A, 4 B,
// 8 EventOutput, 16 EventOutputTrue, 32 EventOutputFalse, 64 OutputIsAlpha,
// 128 Hysteresis.
bool read_compare(Shell* shell, Node& n, const Data* data)
{
   const uint32_t key = data->id;
   const unsigned args = data->arguments();
   const char* text = args == 1 ? data->string(0) : nullptr;
   float number;
   switch (key) {
   case hash("Operation"):
      if (claim(n, 1) && !parse_comparison(text, n.compare.op))
         fail(n, "Operation expects Greater, GreaterOrEqual, Less, LessOrEqual, Equal or NotEqual");
      break;
   case hash("ConstantA"):
   case hash("ConstantB"): {
      const unsigned operand = key == hash("ConstantA") ? 0 : 1;
      if (claim(n, 2u << operand)) {
         if (args != 1 || !data->number(0, number)) fail(n, "constant expects one finite number");
         else n.compare.input(operand, true, number);
      }
      break;
   }
   case hash("EventInputA"):
   case hash("EventInputB"): {
      const unsigned operand = key == hash("EventInputA") ? 0 : 1;
      if (claim(n, 2u << operand)) {
         if (!name_ok(text)) fail(n, "input expects an event name (1..240 bytes)");
         else bind(n, data, operand ? shell->inputB : shell->inputA);
      }
      break;
   }
   case hash("EventOutput"):
      if (claim(n, 8)) create_output(shell, n, text);
      break;
   case hash("EventOutputTrue"):
      if (claim(n, 16)) n.outputTrue = create_named(n, text, kBool);
      break;
   case hash("EventOutputFalse"):
      if (claim(n, 32)) n.outputFalse = create_named(n, text, kBool);
      break;
   case hash("Hysteresis"):
      if (claim(n, 128)) {
         if (args != 1 || !data->number(0, number) || number < 0.0f)
            fail(n, "Hysteresis expects one number, 0 or more");
         else n.compare.hysteresis = number;
      }
      break;
   case hash("OutputIsAlpha"):
      read_output_is_alpha(n, data, args);
      break;
   default:
      log("%s: unrecognised property 0x%08X", name(n), key);
      fail(n, "unsupported property");
      return false;
   }
   return true;
}

// How many arguments each property takes; 0 for a key this kind does not
// read, which its reader rejects itself.
struct Arity { unsigned count; const char* name; };
Arity arity(Kind kind, uint32_t key)
{
   const unsigned math = kind == Kind::Math, lerp = kind == Kind::Lerp, compare = kind == Kind::Compare;
   switch (key) {
   case hash("ConstantA"):        return { 1, "ConstantA" };
   case hash("ConstantB"):        return { 1, "ConstantB" };
   case hash("EventInputA"):      return { 1, "EventInputA" };
   case hash("EventInputB"):      return { 1, "EventInputB" };
   case hash("EventOutput"):      return { 1, "EventOutput" };
   case hash("OutputIsAlpha"):    return { 1, "OutputIsAlpha" };
   case hash("Operation"):        return { math | compare, "Operation" };
   case hash("Clamp"):            return { math * 2, "Clamp" };
   case hash("EventInput"):       return { lerp, "EventInput" };
   case hash("RiseTime"):         return { lerp, "RiseTime" };
   case hash("FallTime"):         return { lerp, "FallTime" };
   case hash("InputRange"):       return { lerp * 2, "InputRange" };
   case hash("EventOutputTrue"):  return { compare, "EventOutputTrue" };
   case hash("EventOutputFalse"): return { compare, "EventOutputFalse" };
   case hash("Hysteresis"):       return { compare, "Hysteresis" };
   default:                       return { 0, nullptr };
   }
}

// TransformNumberMath's properties. Claim bits: 1 Operation, 2 A, 4 B,
// 8 EventOutput, 16 Clamp, 64 OutputIsAlpha.
bool read_math(Shell* shell, Node& n, const Data* data)
{
   const uint32_t key = data->id;
   const unsigned args = data->arguments();
   const char* text = args == 1 ? data->string(0) : nullptr;
   float number;
   switch (key) {
   case hash("Operation"):
      if (claim(n, 1) && !parse_operation(text, n.state.config.operation))
         fail(n, "Operation expects Add, Subtract, Multiply, Divide, Min or Max");
      break;
   case hash("ConstantA"):
   case hash("ConstantB"): {
      const unsigned operand = key == hash("ConstantA") ? 0 : 1;
      if (claim(n, 2u << operand)) {
         if (args != 1 || !data->number(0, number)) fail(n, "constant expects one finite number");
         else n.state.input(operand, true, number);
      }
      break;
   }
   case hash("EventInputA"):
   case hash("EventInputB"): {
      const unsigned operand = key == hash("EventInputA") ? 0 : 1;
      if (claim(n, 2u << operand)) {
         if (!name_ok(text)) fail(n, "input expects an event name (1..240 bytes)");
         else bind(n, data, operand ? shell->inputB : shell->inputA);
      }
      break;
   }
   case hash("EventOutput"):
      if (claim(n, 8)) create_output(shell, n, text);
      break;
   case hash("Clamp"):
      if (claim(n, 16)) {
         n.state.config.clamp = true;
         if (args != 2 || !data->number(0, n.state.config.minimum)
             || !data->number(1, n.state.config.maximum) || !valid(n.state.config))
            fail(n, "Clamp expects finite minimum, maximum with minimum <= maximum");
      }
      break;
   case hash("OutputIsAlpha"):
      read_output_is_alpha(n, data, args);
      break;
   default:
      log("%s: unrecognised property 0x%08X", name(n), key);
      fail(n, "unsupported property");
      return false;
   }
   return true;
}

// A property as the HUD editor writes it back: Name(args), strings quoted,
// numbers as the stock writer prints them (%f), OutputIsAlpha as 1 or 0.
void remember_line(Node& n, const char* property, const Data* data)
{
   if (n.lineCount == kMaxLines) return;
   char line[640];
   int used = std::snprintf(line, sizeof(line), "%s(", property);
   const unsigned args = data->arguments();
   for (unsigned i = 0; i < args; ++i) {
      if (used <= 0 || used >= static_cast<int>(sizeof(line))) return;
      const char* separator = i ? ", " : "";
      const size_t room = sizeof(line) - used;
      int wrote;
      if (const char* text = data->string(i)) {
         wrote = std::snprintf(line + used, room, "%s\"%s\"", separator, text);
      } else if (data->id == hash("OutputIsAlpha")) {
         bool on = false;
         data->flag(i, on);
         wrote = std::snprintf(line + used, room, "%s%d", separator, on ? 1 : 0);
      } else {
         float number = 0.0f;
         data->number(i, number);
         wrote = std::snprintf(line + used, room, "%s%f", separator, number);
      }
      if (wrote < 0) return;
      used += wrote;
   }
   if (used <= 0 || used + 2 > static_cast<int>(sizeof(line))) return;
   line[used++] = ')';
   line[used] = '\0';
   char* copy = new (std::nothrow) char[used + 1];
   if (!copy) return;
   std::memcpy(copy, line, used + 1);
   n.lines[n.lineCount++] = copy;
}

bool __fastcall read_data(Shell* shell, void*, void*, const Data* data)
{
   Node& n = node(shell);
   // Data::arguments() has already dropped a "// comment". Anything still
   // extra is almost always a comment with no space after its "//", which the
   // munger hashes into one word with the comment's first.
   const Arity want = arity(n.kind, data->id);
   if (want.count && data->arguments() > want.count) {
      log("%s: disabled: %s has more arguments than it takes; a comment after it on the "
          "same line needs a space after //", name(n), want.name);
      n.invalid = true;
      return true;
   }
   const unsigned failures = n.failures;
   const bool read = n.kind == Kind::Lerp    ? read_lerp(shell, n, data)
                   : n.kind == Kind::Compare ? read_compare(shell, n, data)
                                             : read_math(shell, n, data);
   // Each property read cleanly is kept for the HUD editor, as written.
   if (read && want.count && n.failures == failures) remember_line(n, want.name, data);
   return read;
}

// The HUD editor's writer for these items, in place of the stock transform's,
// whose lines (InputFactor, EventInput and the rest) these kinds do not read.
// HUD::Item::Write has written the header, Kind("name") from the factory's
// name, and its opening brace, and writes the closing one.
void __fastcall write_data(Shell* shell, void*, void* file, unsigned indent)
{
   if (!writeIndent || !writeFormat) return;
   const Node& n = node(shell);
   for (unsigned i = 0; i < n.lineCount; ++i) {
      writeIndent(file, nullptr, indent);
      writeFormat(file, "%s\n", n.lines[i]);
   }
}

void __fastcall post_read(Shell* shell, void*)
{
   Node& n = node(shell);
   switch (n.kind) {
   case Kind::Lerp:
      if ((n.fields & 9) != 9) fail(n, "EventInput and EventOutput are required");
      break;
   case Kind::Compare:
      if ((n.fields & 7) != 7 || !(n.fields & 56))
         fail(n, "Operation, A, B and at least one of EventOutput, EventOutputTrue and "
                 "EventOutputFalse are required");
      break;
   default:
      if ((n.fields & 15) != 15) fail(n, "Operation, A, B and EventOutput are all required");
   }
   const auto feeds = [&](const EventClass* out) {
      return out && (out == shell->inputA.cls || out == shell->inputB.cls || out == n.endB.cls
                     || out == n.rangeEnd[0].cls || out == n.rangeEnd[1].cls);
   };
   if (feeds(shell->output) || feeds(n.outputTrue) || feeds(n.outputFalse))
      fail(n, "an output cannot be its own input");
   n.ready = !n.invalid;
   // No send here: later items in this same HUD have not bound yet.
}

bool __fastcall write_enabled(Shell*, void*) { return false; }

void* __fastcall destroy(Shell* shell, void*, unsigned flags)
{
   Node* dead = &node(shell);
   Node** link = &nodes;
   while (*link && *link != dead) link = &(*link)->next;
   if (*link) *link = dead->next;
   unbindHandler(&dead->endB);
   unbindHandler(&dead->rangeEnd[0]);
   unbindHandler(&dead->rangeEnd[1]);
   // Native destructor unregisters BOTH shell handlers and removes both list nodes.
   void* result = destroyItem(shell, nullptr, flags);
   for (unsigned i = 0; i < dead->lineCount; ++i) delete[] dead->lines[i];
   delete dead;
   return result;
}

Shell* make(Kind kind, void* factory, const char* itemName, void* callback, void* argument)
{
   Node* n = new (std::nothrow) Node;
   if (!n) { log("out of memory allocating math state"); return nullptr; }
   Shell* shell = createItem(factory, nullptr, itemName, callback, argument);
   if (!shell) { delete n; return nullptr; }
   n->shell = shell;
   n->kind = kind;
   n->next = nodes;
   nodes = n;
   shell->output = nullptr; // Native base constructor does not initialise this.
   for (Handler* h : { &shell->inputA, &shell->inputB, &n->endB, &n->rangeEnd[0], &n->rangeEnd[1] }) {
      h->callback = input;
      h->data = n;
   }
   shell->vtable = itemVtable;
   return shell;
}

Shell* __fastcall create(void* factory, void*, const char* itemName, void* callback, void* argument)
{
   return make(Kind::Math, factory, itemName, callback, argument);
}

Shell* __fastcall create_lerp(void* factory, void*, const char* itemName, void* callback, void* argument)
{
   return make(Kind::Lerp, factory, itemName, callback, argument);
}

Shell* __fastcall create_compare(void* factory, void*, const char* itemName, void* callback, void* argument)
{
   return make(Kind::Compare, factory, itemName, callback, argument);
}

// Wall-clock seconds since the last HUD update, capped at kMaxTick. Steam and GOG
// dropped the update's dt, so every build measures its own.
float tick_seconds()
{
   static LARGE_INTEGER freq = {};
   static LARGE_INTEGER last = {};
   LARGE_INTEGER now;
   QueryPerformanceCounter(&now);
   if (freq.QuadPart == 0) {
      QueryPerformanceFrequency(&freq);
      last = now;
   }
   const double dt = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
   last = now;
   return dt < 0.0 ? 0.0f : dt > kMaxTick ? kMaxTick : float(dt);
}

void update(float dt)
{
   // Activate newcomers together, so constant nodes can initialise downstream
   // chains on the first update after all the HUD consumers have loaded.
   for (Node* n = nodes; n; n = n->next) if (n->ready) n->active = true;
   for (Node* n = nodes; n; n = n->next) {
      if (n->kind == Kind::Lerp && n->active && !n->invalid && n->lerp.advance(dt)) n->pending = true;
      publish(*n, n->outputIsAlpha);
   }
}

#ifndef HUD_NUMBER_MATH_TEST
bool guard(uintptr_t base, uintptr_t address, const char* bytes, const char* mask)
{
   if (!address) return false;
   const auto* code = static_cast<const unsigned char*>(resolve(base, address));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         log("unavailable: fingerprint mismatch at 0x%08X", static_cast<unsigned>(address));
         return false;
      }
   }
   return true;
}

bool entry_guard(uintptr_t base, void* entry, const char* bytes, const char* mask)
{
   // Modtools vtables contain incremental-link thunks. Follow just that single
   // JMP; reject pointers outside the image before inspecting their prologue.
   const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
   const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
   const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
   uintptr_t address = reinterpret_cast<uintptr_t>(entry);
   auto in_image = [base, end](uintptr_t p) { return p >= base && p < end && end - p >= 16; };
   if (!in_image(address)) return false;
   if (*reinterpret_cast<const uint8_t*>(address) == 0xe9) {
      int32_t displacement;
      std::memcpy(&displacement, reinterpret_cast<const void*>(address + 1), 4);
      address = address + 5 + displacement;
   }
   return in_image(address) && guard(base, address - base + 0x400000, bytes, mask);
}

// The HUD editor's .hud writers, the ones its stock items write with. Modtools
// only: GameExt keeps the editor off on Steam and GOG.
void resolve_writers(uintptr_t base)
{
   if (!modtools || !g_addr->hud_write_indent || !g_addr->hud_write_format) return;
   const auto* indent = static_cast<const unsigned char*>(resolve(base, g_addr->hud_write_indent));
   const auto* format = static_cast<const unsigned char*>(resolve(base, g_addr->hud_write_format));
   if (std::memcmp(indent, "\x56\x8b\x74\x24\x08\x85\xf6\x57\x8b\xf9", 10) != 0
       || std::memcmp(format, "\x8b\x4c\x24\x08\x81\xec\x00\x04\x00\x00", 10) != 0) {
      log("the HUD editor's writers are not where expected; it will not save these transforms");
      return;
   }
   writeIndent = reinterpret_cast<WriteIndent>(resolve(base, g_addr->hud_write_indent));
   writeFormat = reinterpret_cast<WriteFormat>(resolve(base, g_addr->hud_write_format));
}
#endif
} // namespace

#ifndef HUD_NUMBER_MATH_TEST
void hud_number_math_resolve(uintptr_t base)
{
   if (resolved) return;
   modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   // Resolve only: EXE sections may be non-executable during installation.
   if (!guard(base, g_addr->hud_math_factory_alloc,
              modtools ? "\x8d\x44\x24\x04\x50\x6a\x1c" : "\x55\x8b\xec\x6a\x00\xff\x75\x08", modtools ? "xxxxxxx" : "xxxxxxxx")
       || !guard(base, g_addr->hud_item_factory_ctor,
              modtools ? "\x8b\xc1\xc7\x00\0\0\0\0\x8d\x48\x04" : "\x55\x8b\xec\x56\x8b\xf1\x8d\x56\x0c",
              modtools ? "xxxx????xxx" : "xxxxxxxxx")
       || !guard(base, g_addr->hud_item_read,
              modtools ? "\x81\xec\x68\x02\0\0\x56" : "\x55\x8b\xec\x83\xe4\xf8\x81\xec\x6c\x02\0\0",
              modtools ? "xxxxxxx" : "xxxxxxxxxxxx")
       || !guard(base, g_addr->hud_item_read_event,
              modtools ? "\x81\xec\x04\x02\0\0\x56" : "\x55\x8b\xec\x8b\x41\x08\x81\xec\x04\x02\0\0",
              modtools ? "xxxxxxx" : "xxxxxxxxxxxx")
       || !guard(base, g_addr->hud_filter_event_name,
              modtools ? "\x53\x56\x8b\x74\x24\x0c\x57" : "\x55\x8b\xec\x51\xa1", modtools ? "xxxxxxx" : "xxxxx")
       || !g_addr->hud_vector3_factory_vtable || !g_addr->hud_vector3_vtable) return;

   std::memcpy(factoryVtable, resolve(base, g_addr->hud_vector3_factory_vtable), sizeof(factoryVtable));
   std::memcpy(itemVtable, resolve(base, g_addr->hud_vector3_vtable), sizeof(itemVtable));
   if (!entry_guard(base, factoryVtable[1],
                    modtools ? "\x56\x6a\x00\x8b\xf1\xe8" : "\x55\x8b\xec\x6a\xff\x68", "xxxxxx")
       || !entry_guard(base, factoryVtable[0],
                    modtools ? "\x56\x8b\xf1\xe8" : "\x55\x8b\xec\x56\x8b\xf1\xe8", modtools ? "xxxx" : "xxxxxxx")
       || !entry_guard(base, itemVtable[0],
                    modtools ? "\x56\x8b\xf1\xe8" : "\x55\x8b\xec\x56\x8b\xf1\xe8", modtools ? "xxxx" : "xxxxxxx")
       || !guard(base, g_addr->hud_event_send, "\x51\x8b\x09\xe8", "xxxx")) {
      log("unavailable: native virtual-method/event-send fingerprint mismatch");
      return;
   }
   createItem = reinterpret_cast<CreateItem>(factoryVtable[1]);
   destroyItem = reinterpret_cast<DestroyItem>(itemVtable[0]);
   std::memcpy(lerpFactoryVtable, factoryVtable, sizeof(lerpFactoryVtable));
   std::memcpy(compareFactoryVtable, factoryVtable, sizeof(compareFactoryVtable));
   factoryVtable[1] = reinterpret_cast<void*>(create);
   lerpFactoryVtable[1] = reinterpret_cast<void*>(create_lerp);
   compareFactoryVtable[1] = reinterpret_cast<void*>(create_compare);
   // The HUD editor saves a file by walking every item: one at the top of a
   // file is written when its write flag (+0x18 bit 0, set by HUD::Item's
   // constructor) is on, and a ViewPort writes its own items whatever theirs.
   // write_data writes these items' own lines; without the editor's writers it
   // writes none, and the items are then kept off the top level, so a save
   // never writes the stock transform's lines in their place.
   resolve_writers(base);
   itemVtable[0] = reinterpret_cast<void*>(destroy);
   itemVtable[2] = resolve(base, g_addr->hud_item_read);
   if (!writeIndent) itemVtable[4] = reinterpret_cast<void*>(write_enabled);
   itemVtable[8] = reinterpret_cast<void*>(read_data);
   itemVtable[9] = reinterpret_cast<void*>(post_read);
   itemVtable[10] = reinterpret_cast<void*>(write_data);
   factoryAlloc = resolve(base, g_addr->hud_math_factory_alloc);
   factoryCtor = reinterpret_cast<FactoryCtor>(resolve(base, g_addr->hud_item_factory_ctor));
   readEvent = resolve(base, g_addr->hud_item_read_event);
   filterName = resolve(base, g_addr->hud_filter_event_name);
   findEvent = resolve(base, g_addr->hud_event_class_find);
   createEvent = reinterpret_cast<NativeCreateEvent>(resolve(base, g_addr->hud_event_class_create));
   sendEvent = reinterpret_cast<Send>(resolve(base, g_addr->hud_event_send));
   resolved = true;
   log("available: TransformNumberMath (Add/Subtract/Multiply/Divide/Min/Max), TransformNumberLerp, "
       "TransformNumberCompare%s", writeIndent ? "; the HUD editor saves them" : "");
}
#endif

void hud_number_math_open()
{
   if (!resolved) return;
   // Never hide a native-lifetime bug by discarding a nonempty sidecar list.
   if (nodes) { log("new mission still has live math items; factory not registered"); return; }
   struct { void** vtable; const char* name; } kinds[] = {
      { factoryVtable, "TransformNumberMath" },
      { lerpFactoryVtable, "TransformNumberLerp" },
      { compareFactoryVtable, "TransformNumberCompare" },
   };
   for (const auto& kind : kinds) {
      void* factory = modtools ? reinterpret_cast<void*(__cdecl*)(bool)>(factoryAlloc)(false)
                               : reinterpret_cast<void*(__cdecl*)(unsigned)>(factoryAlloc)(28);
      if (!factory) { log("could not allocate %s factory", kind.name); continue; }
      factoryCtor(factory, nullptr, hash(kind.name));
      *static_cast<void***>(factory) = kind.vtable;
      *reinterpret_cast<const char**>(static_cast<char*>(factory) + 24) = kind.name;
   }
   dispatchDepth = 0;
}

void hud_number_math_update()
{
   update(tick_seconds());
}
