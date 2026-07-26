#include "pch.h"
#include "soldier_override_texture.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstring>
#include <detours.h>

// =============================================================================
// Extra soldier override-texture slots: OverrideTexture3 / 4 / 5.
//
// The stock engine gives soldier ODFs two texture-override slots:
//   OverrideTexture  = <texture name>
//   OverrideTexture2 = <texture name>
// Each is stored on EntitySoldierClass (fields +0x988/+0x98C modtools,
// +0x794/+0x798 Steam) as PblHash(name). At render, EntitySoldier::Render and
// SoldierElement::RenderUsingContext bind them like this (per slot):
//   state = RedShadingPose::CreateShadingState(pose, MATERIAL_NAME_HASH);
//   if (state) *RedShadingState::GetIntParam(state, 0x891891e9) = textureHash;
// where MATERIAL_NAME_HASH is the PblHash of a model material name:
//   slot1 = hash("override_texture"), slot2 = hash("override_texture2").
// RedShadingPose::CreateShadingState finds the model segment whose material
// name hashes to that key and returns its per-instance shading state; the
// material's shader then samples the texture whose hash we wrote to int-param
// 0x891891e9. If the model has no material with that name the call returns null
// and the slot is a silent no-op.
//
// RedShadingPose::CreateShadingState keeps its bindings in a growable linked
// list (one entry per material name), so the "2" is purely a hardcoded slot
// count, not an engine limit. This adds three more slots:
//   OverrideTexture3 -> model material "override_texture3"
//   OverrideTexture4 -> model material "override_texture4"
//   OverrideTexture5 -> model material "override_texture5"
//
// Note the stock naming asymmetry we mirror: the ODF *property* has no
// underscore (PblHash treats '_' oddly - it ORs 0x20, so 'override_texture'
// and 'overridetexture' hash differently), but the model *material* is the
// underscore form. So property "OverrideTextureN", material "override_textureN".
//
// Implementation: four hooks.
//   1. EntitySoldierClass::SetProperty - consume OverrideTexture3/4/5 and store
//      PblHash(value) per class in a side table (the class struct has no free
//      field to extend, and relocating a 5280-byte struct is not worth it).
//   2/3. EntitySoldier::Render / SoldierElement::RenderUsingContext - record the
//      class being rendered (this->class) for the duration of the call.
//   4. RedShadingPose::CreateShadingState - when the stock code creates the
//      slot-1 binding (key == hash("override_texture")), the render's stack
//      pose exists and is about to be drawn, so we add our extra slots to the
//      same pose right then, keyed by the recorded class's stored hashes.
//
// Because the extra slots piggyback on the stock slot-1 binding call, the class
// must set OverrideTexture (slot 1) to a non-empty value for slots 3-5 to apply
// (the whole stock block, and thus the slot-1 call, is gated on slot 1 != 0).
//
// Inheritance caveat: like the FP-anim-bank feature, storage is a side table
// keyed by class pointer, so a ClassParent child does NOT inherit these from a
// parent (stock OverrideTexture does, being a copied struct field). Set them on
// the concrete soldier class. (Add an EntitySoldierClass::Derive hook if this
// ever needs to inherit.)
// =============================================================================

static constexpr int      kNumExtraSlots = 3;          // OverrideTexture3,4,5
static constexpr uint32_t kIntParamKey   = 0x891891e9; // shader int-param (build-invariant)

// Property names hashed for SetProperty dispatch, and the matching model
// material names hashed for the render-time shading-state key.
static const char* const kPropNames[kNumExtraSlots] = {
   "OverrideTexture3", "OverrideTexture4", "OverrideTexture5",
};
static const char* const kMaterialNames[kNumExtraSlots] = {
   "override_texture3", "override_texture4", "override_texture5",
};

// ---------------------------------------------------------------------------
// Per-build offsets: this-relative EntitySoldierClass pointer inside each of
// the two render functions. Both read the class the same way the stock override
// block does two instructions later.
//   EntitySoldier::Render         soldierClassOff  modtools 0x3C4 / Steam 0x3AC
//   SoldierElement::RenderUsingCtx elementClassOff modtools 0x130 / Steam 0x130
// ---------------------------------------------------------------------------
struct OverrideLayout {
   uint32_t soldierClassOff;
   uint32_t elementClassOff;
};
static constexpr OverrideLayout kLayoutModtools = {0x3C4, 0x130};
// Shared by both retail builds: EntitySoldier::Render and SoldierElement::
// RenderUsingContext are instruction-identical between Steam and GOG.
static constexpr OverrideLayout kLayoutRelease  = {0x3AC, 0x130};

static OverrideLayout s_layout = kLayoutModtools;

// ---------------------------------------------------------------------------
// Game function types
// ---------------------------------------------------------------------------

using fn_hash_string_t = uint32_t(__cdecl*)(const char*);

// EntitySoldierClass::SetProperty - __thiscall(this, uint hash, const char* value)
using fn_SetProperty_t = void(__fastcall*)(void* ecx, void* edx,
                                           unsigned int hash, const char* value);

// EntitySoldier::Render - __thiscall(this, uchar, float, uint)
using fn_SoldierRender_t = void(__fastcall*)(void* ecx, void* edx,
                                             unsigned char p1, float p2, unsigned int p3);

// SoldierElement::RenderUsingContext - __thiscall(this, PblMatrix*, RedColor*)
using fn_ElementRender_t = void(__fastcall*)(void* ecx, void* edx, void* p1, void* p2);

// RedShadingPose::CreateShadingState - __thiscall(pose, uint materialNameHash)
// -> RedShadingState* (null if the model has no material with that name).
using fn_CreateShadingState_t = void*(__fastcall*)(void* ecx, void* edx,
                                                   unsigned int materialNameHash);

// RedShadingState::GetIntParam - __thiscall(state, uint paramHash) -> int*
// (address of the int param slot; caller writes through it).
using fn_GetIntParam_t = int*(__fastcall*)(void* ecx, void* edx, unsigned int paramHash);

// ---------------------------------------------------------------------------
// Resolved pointers / trampolines
// ---------------------------------------------------------------------------

static fn_hash_string_t        fn_hash_string             = nullptr;
static fn_GetIntParam_t        fn_GetIntParam             = nullptr;

static fn_SetProperty_t        original_SetProperty       = nullptr;
static fn_SoldierRender_t      original_SoldierRender     = nullptr;
static fn_ElementRender_t      original_ElementRender     = nullptr;
static fn_CreateShadingState_t original_CreateShadingState = nullptr;

// ---------------------------------------------------------------------------
// Hashes (computed lazily - cannot call game code during the install window)
// ---------------------------------------------------------------------------

static bool     g_hashesReady = false;
static uint32_t g_slot1Key    = 0;                 // hash("override_texture")
static uint32_t g_propHash[kNumExtraSlots] = {};   // dispatch hashes
static uint32_t g_matKey[kNumExtraSlots]   = {};   // material-name keys

static void ensureHashes()
{
   if (g_hashesReady || !fn_hash_string) return;
   g_slot1Key = fn_hash_string("override_texture");
   for (int i = 0; i < kNumExtraSlots; i++) {
      g_propHash[i] = fn_hash_string(kPropNames[i]);
      g_matKey[i]   = fn_hash_string(kMaterialNames[i]);
   }
   g_hashesReady = true;
}

// ---------------------------------------------------------------------------
// Per-class side table: extra texture hashes keyed by EntitySoldierClass*
// ---------------------------------------------------------------------------

static constexpr int kMaxClasses = 64;

struct ClassEntry {
   void*    cls;
   uint32_t hash[kNumExtraSlots];  // PblHash(texture name), 0 = slot unused
};

static ClassEntry g_entries[kMaxClasses] = {};
static int        g_entryCount = 0;

// Rendering is single-threaded; the class currently being rendered, recorded by
// the two render hooks and read by the CreateShadingState hook.
static void*      g_curClass = nullptr;

static ClassEntry* findEntry(void* cls)
{
   for (int i = 0; i < g_entryCount; i++)
      if (g_entries[i].cls == cls) return &g_entries[i];
   return nullptr;
}

static void setSlot(void* cls, int slot, uint32_t texHash)
{
   ClassEntry* e = findEntry(cls);
   if (!e) {
      if (texHash == 0) return; // nothing to store
      if (g_entryCount >= kMaxClasses) {
         get_gamelog()("[OverrideTexture] class table full (%d), ignoring\n", kMaxClasses);
         return;
      }
      e = &g_entries[g_entryCount++];
      e->cls = cls;
      for (int i = 0; i < kNumExtraSlots; i++) e->hash[i] = 0;
   }
   e->hash[slot] = texHash;
}

// ---------------------------------------------------------------------------
// Hook: EntitySoldierClass::SetProperty
// ---------------------------------------------------------------------------

static void __fastcall hooked_SetProperty(void* ecx, void* /*edx*/,
                                          unsigned int hash, const char* value)
{
   // Lazy init: install runs while .text is non-executable (see droideka note),
   // ODF parsing runs long after with the engine live.
   ensureHashes();

   if (g_hashesReady) {
      for (int i = 0; i < kNumExtraSlots; i++) {
         if (hash != g_propHash[i]) continue;
         // Consume it: the stock parser has no case for this hash. Store
         // PblHash(value) exactly as the engine stores OverrideTexture.
         if (ecx)
            setSlot(ecx, i, (value && value[0]) ? fn_hash_string(value) : 0);
         return;
      }
   }

   original_SetProperty(ecx, nullptr, hash, value);
}

// ---------------------------------------------------------------------------
// Hooks: record the class being rendered
// ---------------------------------------------------------------------------

static void __fastcall hooked_SoldierRender(void* ecx, void* edx,
                                            unsigned char p1, float p2, unsigned int p3)
{
   void* prev = g_curClass;
   if (g_entryCount > 0 && ecx) {
      __try { g_curClass = *(void**)((uintptr_t)ecx + s_layout.soldierClassOff); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_curClass = nullptr; }
   }
   original_SoldierRender(ecx, edx, p1, p2, p3);
   g_curClass = prev;
}

static void __fastcall hooked_ElementRender(void* ecx, void* edx, void* p1, void* p2)
{
   void* prev = g_curClass;
   if (g_entryCount > 0 && ecx) {
      __try { g_curClass = *(void**)((uintptr_t)ecx + s_layout.elementClassOff); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_curClass = nullptr; }
   }
   original_ElementRender(ecx, edx, p1, p2);
   g_curClass = prev;
}

// ---------------------------------------------------------------------------
// Hook: RedShadingPose::CreateShadingState
// ---------------------------------------------------------------------------
// Fires for every shading-state creation game-wide; gated to near-zero cost when
// unused. When the stock render creates the slot-1 binding for a soldier class
// that has extra slots, add slots 3-5 to the same pose.

static void* __fastcall hooked_CreateShadingState(void* ecx, void* edx,
                                                  unsigned int materialNameHash)
{
   void* state = original_CreateShadingState(ecx, edx, materialNameHash);

   if (g_entryCount > 0 && g_curClass && materialNameHash == g_slot1Key) {
      ClassEntry* e = findEntry(g_curClass);
      if (e) {
         for (int i = 0; i < kNumExtraSlots; i++) {
            if (e->hash[i] == 0) continue;
            void* s = original_CreateShadingState(ecx, nullptr, g_matKey[i]);
            if (s) {
               int* p = fn_GetIntParam(s, nullptr, kIntParamKey);
               if (p) *p = (int)e->hash[i];
            }
         }
      }
   }

   return state;
}

// ---------------------------------------------------------------------------
// init_state self-detour (non-modtools only; modtools resets via lua_hooks)
// ---------------------------------------------------------------------------

using fn_init_state_t = void(__cdecl*)();
static fn_init_state_t original_init_state = nullptr;

static void __cdecl hooked_init_state()
{
   original_init_state();
   soldier_override_texture_reset();
}

// ---------------------------------------------------------------------------
// Install / Uninstall / Reset
// ---------------------------------------------------------------------------

void soldier_override_texture_install(uintptr_t exe_base)
{
   switch (g_build) {
   case GameBuild::Modtools: s_layout = kLayoutModtools; break;
   case GameBuild::Steam:
   case GameBuild::GOG:      s_layout = kLayoutRelease;  break;
   default: return; // unknown build
   }

   if (g_addr->hash_string == 0 || g_addr->soldier_class_set_property == 0 ||
       g_addr->soldier_render == 0 || g_addr->soldier_element_render_ctx == 0 ||
       g_addr->shading_pose_create_state == 0 || g_addr->shading_state_get_int_param == 0)
      return;

   fn_hash_string = (fn_hash_string_t)resolve(exe_base, g_addr->hash_string);
   fn_GetIntParam = (fn_GetIntParam_t)resolve(exe_base, g_addr->shading_state_get_int_param);

   original_SetProperty        = (fn_SetProperty_t)       resolve(exe_base, g_addr->soldier_class_set_property);
   original_SoldierRender      = (fn_SoldierRender_t)     resolve(exe_base, g_addr->soldier_render);
   original_ElementRender      = (fn_ElementRender_t)     resolve(exe_base, g_addr->soldier_element_render_ctx);
   original_CreateShadingState = (fn_CreateShadingState_t)resolve(exe_base, g_addr->shading_pose_create_state);

   // On modtools lua_hooks owns init_state and calls our reset; elsewhere take it
   // ourselves so stale class pointers don't survive a level change.
   const bool ownInitState = (g_build != GameBuild::Modtools) && g_addr->init_state != 0;
   if (ownInitState)
      original_init_state = (fn_init_state_t)resolve(exe_base, g_addr->init_state);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SetProperty,        hooked_SetProperty);
   DetourAttach(&(PVOID&)original_SoldierRender,      hooked_SoldierRender);
   DetourAttach(&(PVOID&)original_ElementRender,      hooked_ElementRender);
   DetourAttach(&(PVOID&)original_CreateShadingState, hooked_CreateShadingState);
   if (ownInitState)
      DetourAttach(&(PVOID&)original_init_state, hooked_init_state);
   DetourTransactionCommit();
}

void soldier_override_texture_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_SetProperty)        DetourDetach(&(PVOID&)original_SetProperty,        hooked_SetProperty);
   if (original_SoldierRender)      DetourDetach(&(PVOID&)original_SoldierRender,      hooked_SoldierRender);
   if (original_ElementRender)      DetourDetach(&(PVOID&)original_ElementRender,      hooked_ElementRender);
   if (original_CreateShadingState) DetourDetach(&(PVOID&)original_CreateShadingState, hooked_CreateShadingState);
   if (original_init_state)         DetourDetach(&(PVOID&)original_init_state,         hooked_init_state);
   DetourTransactionCommit();
}

void soldier_override_texture_reset()
{
   std::memset(g_entries, 0, sizeof(g_entries));
   g_entryCount = 0;
   g_curClass   = nullptr;
}
