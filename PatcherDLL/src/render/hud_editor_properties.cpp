#include "pch.h"
#include "hud_editor_properties.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <detours.h>

#include <string.h>

// =============================================================================
// The modtools HUD editor's panel lists an element's properties from its item
// factory, the object a .hud keyword makes its items with. Read on modtools,
// names from the Phantom PDB:
//
//   Item::Factory    +0x04  the terminator of its property list
//                           (PropertyGetFirst 0x006B6A50 reads it)
//                    +0x08  its type's PblHash
//                    +0x0C  its node on the list of factories, whose
//                           terminator is 0x00AD87BC (FindByHashID 0x006B6AE0)
//   Property                { next, Data* }, the list singly linked and closed
//                           by the terminator
//   Property::Data          { name, PblHash, type (2 = enum), enum names, min,
//                           max, ... }, 0x3C bytes (Property::Init for an enum,
//                           0x006B6810)
//
// For each property the panel asks the element's GetProperty (vtable +0x18)
// and shows the line only if it answers; changing one calls SetProperty
// (+0x14), and an enum value is named by PropertyTranslateEnum (+0x1C). Every
// element type ends up in Element's for a property it does not know: its
// PropertyTranslateEnum (0x00692670) is hooked here, its GetProperty and
// SetProperty by the module that owns the property.
//
// HUD::Manager::Open makes the factories and Close frees them, so GameExt's
// properties are linked into their lists each time the HUD opens: taken off
// any list that still holds them, then added at the end. ScreenAnchor goes on
// every factory, since any element can be a piece and the owner hides the line
// where it does nothing; FillFrom on BarBitmap's and ProceduralBarBitmap's.
// When a factory is freed, its own properties unlink themselves by walking the
// list round, through GameExt's, which lie in this module's memory.
// =============================================================================

namespace {

constexpr uint32_t kTypeEnum          = 2;
constexpr uint32_t kFactoryProperties = 0x04;
constexpr uint32_t kFactoryType       = 0x08;
constexpr uint32_t kFactoryNode       = 0x0C;
constexpr int      kMaxFactories      = 96;
constexpr int      kMaxList           = 4096;

struct PropertyData {
   const char*        name;
   uint32_t           hash;
   uint32_t           type;
   const char* const* names;
   uint32_t           minValue;
   uint32_t           maxValue;
   uint8_t            unused[0x24];   // the float and vector ranges
};
static_assert(sizeof(PropertyData) == 0x3C, "Property::Data");

struct PropertyNode {
   PropertyNode*       next;
   const PropertyData* data;
};

// None, then shares 0 to 1 in steps of 0.05 with the three edges by name: the
// order hud_true_widescreen_core.hpp's anchor_index counts in.
const char* const kAnchorNames[] = {
   "None", "Left", "0.05", "0.1", "0.15", "0.2", "0.25", "0.3", "0.35", "0.4", "0.45", "Center",
   "0.55", "0.6", "0.65", "0.7", "0.75", "0.8", "0.85", "0.9", "0.95", "Right",
};
const char* const kFillNames[] = { "Left", "Right", "Bottom", "Top" };
constexpr uint32_t kAnchorCount = sizeof(kAnchorNames) / sizeof(kAnchorNames[0]);
constexpr uint32_t kFillCount   = sizeof(kFillNames) / sizeof(kFillNames[0]);
static_assert(kAnchorCount == 22, "ScreenAnchor's choices");

PropertyData s_anchor = { "ScreenAnchor", pbl_hash("ScreenAnchor"), kTypeEnum, kAnchorNames, 0, kAnchorCount - 1, {} };
PropertyData s_fill   = { "FillFrom", pbl_hash("FillFrom"), kTypeEnum, kFillNames, 0, kFillCount - 1, {} };
const uint32_t kFillFactories[] = { pbl_hash("BarBitmap"), pbl_hash("ProceduralBarBitmap") };

PropertyNode s_nodes[kMaxFactories * 2];
int          s_nodeCount = 0;
bool         s_warnedFull = false;

using TranslateFn = const char*(__fastcall*)(void* self, void* edx, uint32_t property, uint32_t value);
TranslateFn      s_translate = nullptr;
const uintptr_t* s_factories = nullptr;   // the list's terminator node: { next, prev }

const char* __fastcall hooked_Translate(void* self, void* edx, uint32_t property, uint32_t value)
{
   if (property == s_anchor.hash) return value < kAnchorCount ? kAnchorNames[value] : "";
   if (property == s_fill.hash) return value < kFillCount ? kFillNames[value] : "";
   return s_translate(self, edx, property, value);
}

bool ours(const PropertyNode* node)
{
   return node >= s_nodes && node < s_nodes + s_nodeCount;
}

// The factory's property list, from its terminator.
PropertyNode* list_of(uint8_t* factory)
{
   return reinterpret_cast<PropertyNode*>(factory + kFactoryProperties);
}

// Takes GameExt's properties off a factory's list.
void unlink(uint8_t* factory)
{
   PropertyNode* const end = list_of(factory);
   PropertyNode* prev = end;
   for (int n = 0; prev->next && prev->next != end && n < kMaxList; ++n) {
      PropertyNode* node = prev->next;
      if (ours(node)) prev->next = node->next;
      else prev = node;
   }
}

// Adds a property at the end of a factory's list.
void append(uint8_t* factory, const PropertyData* data)
{
   if (s_nodeCount == sizeof(s_nodes) / sizeof(s_nodes[0])) {
      if (!s_warnedFull) install_log("[HudEditorProperties] over %d item factories; the rest list no GameExt "
                                     "properties", kMaxFactories);
      s_warnedFull = true;
      return;
   }
   PropertyNode* const end = list_of(factory);
   PropertyNode* last = end;
   for (int n = 0; last->next && last->next != end && n < kMaxList; ++n) last = last->next;
   if (last->next != end) return;   // not a list that can be read to its end
   PropertyNode* node = &s_nodes[s_nodeCount++];
   node->data = data;
   node->next = end;
   last->next = node;
}

template <class Visit>
void for_each_factory(Visit visit)
{
   const uintptr_t end = reinterpret_cast<uintptr_t>(s_factories);
   int n = 0;
   for (uintptr_t node = s_factories[0]; node && node != end && n < kMaxFactories * 4;
        node = *reinterpret_cast<const uintptr_t*>(node), ++n)
      visit(reinterpret_cast<uint8_t*>(node - kFactoryNode));
}

void link_all()
{
   for_each_factory(unlink);
   s_nodeCount = 0;
   for_each_factory([](uint8_t* factory) {
      append(factory, &s_anchor);
      const uint32_t type = *reinterpret_cast<const uint32_t*>(factory + kFactoryType);
      for (uint32_t bar : kFillFactories)
         if (type == bar) append(factory, &s_fill);
   });
}

bool code_is(uintptr_t base, uintptr_t va, const char* what, const char* bytes, size_t length)
{
   __try {
      if (memcmp(resolve(base, va), bytes, length) == 0) return true;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   install_log("[HudEditorProperties] NOT installed: unexpected code at %s 0x%08X", what, (unsigned)va);
   return false;
}

} // namespace

void hud_editor_properties_open()
{
   if (!s_factories) return;
   __try {
      link_all();
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      install_log("[HudEditorProperties] the item factories could not be read; the HUD editor lists no "
                  "GameExt properties");
      s_nodeCount = 0;
   }
}

void hud_editor_properties_install(uintptr_t base)
{
   if (g_build != GameBuild::Modtools) return;   // the editor is off on Steam and GOG
   const auto& a = *g_addr;
   if (!a.hud_element_translate_enum || !a.hud_factory_find || !a.hud_factory_property_first ||
       !a.hud_factory_list || !a.hud_property_init_enum) {
      install_log("[HudEditorProperties] NOT installed: no address set for this build");
      return;
   }
   // The list of factories and where a factory keeps its type and node,
   // FindByHashID; its property list, PropertyGetFirst; the layout of a
   // property's Data, Property::Init for an enum; and the base enum names.
   if (!code_is(base, a.hud_factory_find, "Item::Factory::FindByHashID",
                "\x8B\x0D\xBC\x87\xAD\x00\x81\xF9\xBC\x87\xAD\x00\x74\x22\x8B\x54\x24\x04\x81\xF9\xBC\x87\xAD\x00"
                "\x75\x04\x33\xC0\xEB\x03\x8D\x41\xF4\x39\x50\x08", 36) ||
       !code_is(base, a.hud_factory_property_first, "Item::Factory::PropertyGetFirst",
                "\x8B\x41\x04\x83\xC1\x04\x3B\xC1\x75\x03\x33\xC0\xC3", 13) ||
       !code_is(base, a.hud_property_init_enum, "Item::Factory::Property::Init (enum)",
                "\x56\x8B\xF1\x8B\x4E\x04\x85\xC9\x74\x54\x8B\x44\x24\x08\x89\x01\x50\x8D\x4C\x24\x0C\xE8\xA6\xB3"
                "\x12\x00\x8B\x10\x8B\x4E\x04\x89\x51\x04\x8B\x46\x04\x8B\x54\x24\x10\xC7\x40\x08\x02\x00\x00\x00"
                "\x8B\x4E\x04\x89\x51\x0C\x8B\x46\x04\x8B\x4C\x24\x14\x89\x48\x10\x8B\x44\x24\x18\x8B\x56\x04\x8B"
                "\x4C\x24\x0C\x89\x42\x14", 78) ||
       !code_is(base, a.hud_element_translate_enum, "HUD::Element::PropertyTranslateEnum",
                "\x8B\x44\x24\x04\x3D\xFA\x1F\xF4\xF9\x74\x1A\x3D\xAB\x4E\x78\xFA\x74\x05\x33\xC0\xC2\x08\x00", 23))
      return;

   s_translate = reinterpret_cast<TranslateFn>(resolve(base, a.hud_element_translate_enum));
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (DetourAttach(&(PVOID&)s_translate, hooked_Translate) != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[HudEditorProperties] NOT installed: the detour failed");
      return;
   }
   s_factories = static_cast<const uintptr_t*>(resolve(base, a.hud_factory_list));
   install_log("[HudEditorProperties] installed: the HUD editor's panel lists FillFrom on bars and ScreenAnchor "
               "on the pieces of an opted-in file");
}
