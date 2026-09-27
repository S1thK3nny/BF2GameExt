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

struct Node {
   Shell* shell = nullptr;
   Node* next = nullptr;
   State state;
   unsigned fields = 0;
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
void* factoryVtable[2]{};
void* itemVtable[12]{};

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
}

EventClass* find(uint32_t id)
{
   if (modtools) return reinterpret_cast<EventClass*(__cdecl*)(uint32_t)>(findEvent)(id);
   return reinterpret_cast<EventClass*(__fastcall*)(uint32_t)>(findEvent)(id);
}

void publish(Node& n)
{
   if (!n.active || !n.ready || n.invalid || !n.pending) return;
   if (n.busy || dispatchDepth >= 32) {
      if (!n.warnedRuntime) log("%s: feedback/depth limit; update ignored", name(n));
      n.warnedRuntime = true;
      return;
   }
   float result;
   n.pending = false;
   if (!n.state.next(result)) return;
   Event event{ n.shell->output, 0 };
   std::memcpy(&event.bits, &result, sizeof(result));
   n.busy = true;
   ++dispatchDepth;
   sendEvent(&event, nullptr);
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
   // A and B bound to the SAME event must update together, not briefly publish
   // (new A op stale B) on the first of the two native callbacks.
   if (event->cls == n.shell->inputA.cls) n.state.input(0, acceptable, value);
   if (event->cls == n.shell->inputB.cls) n.state.input(1, acceptable, value);
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

bool __fastcall read_data(Shell* shell, void*, void*, const Data* data)
{
   Node& n = node(shell);
   const uint32_t key = data->id;
   const char* text = data->count == 1 ? data->string(0) : nullptr;
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
         if (data->count != 1 || !data->number(0, number)) fail(n, "constant expects one finite number");
         else n.state.input(operand, true, number);
      }
      break;
   }
   case hash("EventInputA"):
   case hash("EventInputB"): {
      const unsigned operand = key == hash("EventInputA") ? 0 : 1;
      if (claim(n, 2u << operand)) {
         if (!text || !*text || std::strlen(text) > 240) fail(n, "input expects an event name (1..240 bytes)");
         else bind(n, data, operand ? shell->inputB : shell->inputA);
      }
      break;
   }
   case hash("EventOutput"):
      if (claim(n, 8)) {
         if (!text || !*text || std::strlen(text) > 240) { fail(n, "output expects an event name (1..240 bytes)"); break; }
         char filtered[512]{};
         if (modtools) reinterpret_cast<void(__cdecl*)(const char*, char*, unsigned)>(filterName)(text, filtered, 511);
         else reinterpret_cast<void(__fastcall*)(const char*, char*)>(filterName)(text, filtered);
         filtered[511] = 0;
         // Never overwrite a stock event or allow two writers. This also stops
         // two math nodes constructing an indirect feedback graph.
         if (find(hash(filtered))) { fail(n, "output event already exists; use a unique name"); break; }
         shell->output = createEvent(4, "%s", filtered);
         if (!shell->output) fail(n, "could not create Float output event");
      }
      break;
   case hash("Clamp"):
      if (claim(n, 16)) {
         n.state.config.clamp = true;
         if (data->count != 2 || !data->number(0, n.state.config.minimum)
             || !data->number(1, n.state.config.maximum) || !valid(n.state.config))
            fail(n, "Clamp expects finite minimum, maximum with minimum <= maximum");
      }
      break;
   default:
      log("%s: unrecognised property 0x%08X", name(n), key);
      fail(n, "unsupported property");
      return false;
   }
   return true;
}

void __fastcall post_read(Shell* shell, void*)
{
   Node& n = node(shell);
   if ((n.fields & 15) != 15) fail(n, "Operation, A, B and EventOutput are all required");
   if (shell->output && (shell->output == shell->inputA.cls || shell->output == shell->inputB.cls))
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
   // Native destructor unregisters BOTH handlers and removes both list nodes.
   void* result = destroyItem(shell, nullptr, flags);
   delete dead;
   return result;
}

Shell* __fastcall create(void* factory, void*, const char* itemName, void* callback, void* argument)
{
   Node* n = new (std::nothrow) Node;
   if (!n) { log("out of memory allocating math state"); return nullptr; }
   Shell* shell = createItem(factory, nullptr, itemName, callback, argument);
   if (!shell) { delete n; return nullptr; }
   n->shell = shell;
   n->next = nodes;
   nodes = n;
   shell->output = nullptr; // Native base constructor does not initialise this.
   shell->inputA.callback = shell->inputB.callback = input;
   shell->inputA.data = shell->inputB.data = n;
   shell->vtable = itemVtable;
   return shell;
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
   factoryVtable[1] = reinterpret_cast<void*>(create);
   itemVtable[0] = reinterpret_cast<void*>(destroy);
   itemVtable[2] = resolve(base, g_addr->hud_item_read);
   itemVtable[4] = reinterpret_cast<void*>(write_enabled); // No lossy stock-editor write.
   itemVtable[8] = reinterpret_cast<void*>(read_data);
   itemVtable[9] = reinterpret_cast<void*>(post_read);
   factoryAlloc = resolve(base, g_addr->hud_math_factory_alloc);
   factoryCtor = reinterpret_cast<FactoryCtor>(resolve(base, g_addr->hud_item_factory_ctor));
   readEvent = resolve(base, g_addr->hud_item_read_event);
   filterName = resolve(base, g_addr->hud_filter_event_name);
   findEvent = resolve(base, g_addr->hud_event_class_find);
   createEvent = reinterpret_cast<NativeCreateEvent>(resolve(base, g_addr->hud_event_class_create));
   sendEvent = reinterpret_cast<Send>(resolve(base, g_addr->hud_event_send));
   resolved = true;
   log("available: TransformNumberMath (Add/Subtract/Multiply/Divide/Min/Max)");
}
#endif

void hud_number_math_open()
{
   if (!resolved) return;
   // Never hide a native-lifetime bug by discarding a nonempty sidecar list.
   if (nodes) { log("new mission still has live math items; factory not registered"); return; }
   void* factory = modtools ? reinterpret_cast<void*(__cdecl*)(bool)>(factoryAlloc)(false)
                            : reinterpret_cast<void*(__cdecl*)(unsigned)>(factoryAlloc)(28);
   if (!factory) { log("could not allocate TransformNumberMath factory"); return; }
   factoryCtor(factory, nullptr, hash("TransformNumberMath"));
   *static_cast<void***>(factory) = factoryVtable;
   *reinterpret_cast<const char**>(static_cast<char*>(factory) + 24) = "TransformNumberMath";
   dispatchDepth = 0;
}

void hud_number_math_update()
{
   // Activate newcomers together, so constant nodes can initialise downstream
   // chains on the first update after all the HUD consumers have loaded.
   for (Node* n = nodes; n; n = n->next) if (n->ready) n->active = true;
   for (Node* n = nodes; n; n = n->next) publish(*n);
}
