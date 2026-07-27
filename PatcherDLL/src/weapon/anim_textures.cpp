#include "pch.h"
#include "anim_textures.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <detours.h>
#include <unordered_map>

// =============================================================================
// Animated lightsaber blade textures (Xbox AnimTexture1-3 port)
// =============================================================================
// Xbox SWBF2 gives lightsaber blades a 4-frame animated texture cycle via
// AnimTexture1-3 WeaponMelee ODF properties; PC uses a single static texture.
//
// Implementation:
//   1. Hook WeaponMeleeClass::SetProperty - intercept the AnimTexture1/2/3
//      hashes, read the blade's base texture hash, store the anim set in an
//      external map keyed by the base texture hash.
//   2. Hook _RenderLightsabre - look up the incoming texture hash in the map
//      and substitute the current animation frame based on a timer.
//
// The blade struct is never modified; all anim data lives in s_animTextures.
// This patch is always toggled; No need to disable it in the ini.
// =============================================================================

// Runtime-only logger (never called during install; see the install-window
// no-game-code rule).  Resolved in anim_textures_install.
static GameLog_t g_log = nullptr;

// ---------------------------------------------------------------------------
// Property hashes (FNV-1a of "AnimTexture1/2/3")
// ---------------------------------------------------------------------------
static constexpr uint32_t HASH_ANIM_TEXTURE_1 = 0x2c47e9ee;
static constexpr uint32_t HASH_ANIM_TEXTURE_2 = 0x2b47e85b;
static constexpr uint32_t HASH_ANIM_TEXTURE_3 = 0x2a47e6c8;

// FNV-1a hash (PblHash) - case-insensitive, matches the game's texture hash.
static uint32_t pbl_hash(const char* str)
{
   uint32_t h = 0x811c9dc5u;
   for (; *str; ++str)
      h = (h ^ ((uint8_t)*str | 0x20)) * 0x01000193u;
   return h;
}

// ---------------------------------------------------------------------------
// Animation data - keyed by base texture hash
// ---------------------------------------------------------------------------
struct AnimTextureSet {
   uint32_t frames[4]; // [0]=base, [1]=AnimTex1, [2]=AnimTex2, [3]=AnimTex3
};

static std::unordered_map<uint32_t, AnimTextureSet> s_animTextures;

// Simple 4-frame cycle at ~15 FPS.
static DWORD s_lastFrameTick  = 0;
static int   s_animFrameIndex = 0;
static constexpr DWORD ANIM_FRAME_MS = 66;

static void update_anim_timer()
{
   DWORD now = GetTickCount();
   if (now - s_lastFrameTick >= ANIM_FRAME_MS) {
      s_lastFrameTick = now;
      s_animFrameIndex = (s_animFrameIndex + 1) & 3;
   }
}

// Returns the animated texture hash for this frame, or the original hash if no
// anim textures are registered for it.  __cdecl - called from the naked hook.
static int __cdecl resolve_anim_texture(int texHash)
{
   update_anim_timer();

   auto it = s_animTextures.find((uint32_t)texHash);
   if (it != s_animTextures.end()) {
      const auto& anim = it->second;
      if (anim.frames[1] || anim.frames[2] || anim.frames[3]) {
         int resolved = (int)anim.frames[s_animFrameIndex];
         if (resolved) return resolved;
         return (int)anim.frames[0];
      }
   }
   return texHash;
}

// ---------------------------------------------------------------------------
// Hook: _RenderLightsabre - modtools (pure __cdecl, all 7 params on stack)
// ---------------------------------------------------------------------------
using fn_RenderLightsabre_cdecl = void(__cdecl*)(float*, float*, int, int, float, float, int);
static fn_RenderLightsabre_cdecl original_RenderLightsabre_cdecl = nullptr;

static void __cdecl hooked_RenderLightsabre_cdecl(
   float* pos, float* dir, int texHash, int trailTexHash,
   float length, float width, int flags)
{
   texHash = resolve_anim_texture(texHash);
   original_RenderLightsabre_cdecl(pos, dir, texHash, trailTexHash, length, width, flags);
}

// ---------------------------------------------------------------------------
// Hook: _RenderLightsabre - Steam/GOG (LTCG hybrid: ECX=pos, EDX=dir,
//   5 stack args [texHash, trailTex, length, width, flags], caller-clean)
// ---------------------------------------------------------------------------
static void* original_RenderLightsabre_regcall = nullptr;

__declspec(naked) static void hooked_RenderLightsabre_regcall()
{
   __asm {
      // On entry: ECX=pos, EDX=dir, stack=[ret][texHash][trailTex][length][width][flags]
      push edx
      push ecx
      // texHash is now at [ESP+12] (2 pushes shifted the stack by 8)
      push dword ptr [esp+12]
      call resolve_anim_texture
      add  esp, 4
      mov  [esp+12], eax          // replace texHash with resolved value
      pop  ecx
      pop  edx
      jmp  [original_RenderLightsabre_regcall]
   }
}

// ---------------------------------------------------------------------------
// WeaponMeleeClass::SetProperty and blade lookup
// ---------------------------------------------------------------------------
// void __thiscall WeaponMeleeClass::SetProperty(this, uint propertyHash, char* value)
//
// The blade array pointer is at this+blade_array_offset (modtools 0x3DC,
// retail 0x2C8); each entry is 0x34 bytes with the base texture hash at +0x28.
// BladeLookup (__thiscall, RET 0x0C) resolves the blade index from globals the
// "Blade" property parse path stashes (model / name-hash / unknown).

using fn_SetProperty  = void(__thiscall*)(void*, uint32_t, const char*);
using fn_BladeLookup  = int(__thiscall*)(void*, int, uint32_t, uint32_t);

static fn_SetProperty  original_SetProperty = nullptr;

// Resolved at install time (runtime-cheap lookups):
static fn_BladeLookup s_bladeLookupFn   = nullptr;
static uint32_t*      s_bladeModelPtr   = nullptr;
static uint32_t*      s_bladeNameHashPtr = nullptr;
static uint32_t*      s_bladeUnkPtr     = nullptr;
static unsigned       s_bladeArrayOffset = 0;

static int blade_lookup(void* weaponMeleeClass)
{
   if (!s_bladeLookupFn) return -1;
   return s_bladeLookupFn(weaponMeleeClass, (int)*s_bladeModelPtr,
                          *s_bladeNameHashPtr, *s_bladeUnkPtr);
}

static void register_anim_texture(void* weaponMeleeClass, int frameSlot, const char* textureName)
{
   int bladeIdx = blade_lookup(weaponMeleeClass);
   if (bladeIdx < 0) return;

   uint32_t* bladeArrayPtr = *(uint32_t**)((char*)weaponMeleeClass + s_bladeArrayOffset);
   if (!bladeArrayPtr) return;

   // Each blade entry is 0x34 bytes; texture hash at +0x28 (dword index 10).
   uint32_t* bladeEntry = (uint32_t*)((char*)bladeArrayPtr + bladeIdx * 0x34);
   uint32_t baseTex = bladeEntry[10];

   if (!baseTex) {
      if (g_log)
         g_log("[AnimTex] WARNING: base texture hash is 0 for blade %d - AnimTexture%d ignored\n",
               bladeIdx, frameSlot);
      return;
   }

   uint32_t animHash = pbl_hash(textureName);

   auto& entry = s_animTextures[baseTex];
   entry.frames[0] = baseTex;
   entry.frames[frameSlot] = animHash;
}

static void __fastcall hooked_SetProperty(void* thisPtr, void* /*edx*/, uint32_t propHash, const char* value)
{
   // Let the original set the base Texture first, then read it back.
   original_SetProperty(thisPtr, propHash, value);

   switch (propHash) {
      case HASH_ANIM_TEXTURE_1: register_anim_texture(thisPtr, 1, value); break;
      case HASH_ANIM_TEXTURE_2: register_anim_texture(thisPtr, 2, value); break;
      case HASH_ANIM_TEXTURE_3: register_anim_texture(thisPtr, 3, value); break;
   }
}

// ---------------------------------------------------------------------------
// Per-build address set (selected from g_build, populated from game_addrs)
// ---------------------------------------------------------------------------
struct AnimTexAddrs {
   uintptr_t render_lightsabre;
   uintptr_t set_property;
   uintptr_t blade_lookup;
   uintptr_t blade_global_model;
   uintptr_t blade_global_namehash;
   uintptr_t blade_global_unk;
   unsigned  blade_array_offset; // WeaponMeleeClass -> blade array pointer offset
};

static constexpr AnimTexAddrs MODTOOLS_ADDRS = {
   game_addrs::modtools::lightsabre_render,
   game_addrs::modtools::weaponmelee_set_property,
   game_addrs::modtools::blade_lookup,
   game_addrs::modtools::blade_global_model,
   game_addrs::modtools::blade_global_namehash,
   game_addrs::modtools::blade_global_unk,
   0x3DC,
};

static constexpr AnimTexAddrs STEAM_ADDRS = {
   game_addrs::steam::lightsabre_render,
   game_addrs::steam::weaponmelee_set_property,
   game_addrs::steam::blade_lookup,
   game_addrs::steam::blade_global_model,
   game_addrs::steam::blade_global_namehash,
   game_addrs::steam::blade_global_unk,
   0x2C8,
};

static constexpr AnimTexAddrs GOG_ADDRS = {
   game_addrs::gog::lightsabre_render,
   game_addrs::gog::weaponmelee_set_property,
   game_addrs::gog::blade_lookup,
   game_addrs::gog::blade_global_model,
   game_addrs::gog::blade_global_namehash,
   game_addrs::gog::blade_global_unk,
   0x2C8,
};

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------
void anim_textures_install(uintptr_t exe_base)
{
   const AnimTexAddrs* addrs = nullptr;
   switch (g_build) {
      case GameBuild::Modtools: addrs = &MODTOOLS_ADDRS; break;
      case GameBuild::Steam:    addrs = &STEAM_ADDRS;    break;
      case GameBuild::GOG:      addrs = &GOG_ADDRS;      break;
      default:                  return; // unsupported build
   }

   g_log = get_gamelog(); // runtime-only - must not be called during install

   s_bladeLookupFn    = (fn_BladeLookup)resolve(exe_base, addrs->blade_lookup);
   s_bladeModelPtr    = (uint32_t*)resolve(exe_base, addrs->blade_global_model);
   s_bladeNameHashPtr = (uint32_t*)resolve(exe_base, addrs->blade_global_namehash);
   s_bladeUnkPtr      = (uint32_t*)resolve(exe_base, addrs->blade_global_unk);
   s_bladeArrayOffset = addrs->blade_array_offset;

   original_SetProperty = (fn_SetProperty)resolve(exe_base, addrs->set_property);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());

   if (g_build == GameBuild::Modtools) {
      original_RenderLightsabre_cdecl = (fn_RenderLightsabre_cdecl)resolve(exe_base, addrs->render_lightsabre);
      DetourAttach(&(PVOID&)original_RenderLightsabre_cdecl, hooked_RenderLightsabre_cdecl);
   } else {
      original_RenderLightsabre_regcall = (void*)resolve(exe_base, addrs->render_lightsabre);
      DetourAttach(&(PVOID&)original_RenderLightsabre_regcall, hooked_RenderLightsabre_regcall);
   }

   DetourAttach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   DetourTransactionCommit();
}

void anim_textures_uninstall()
{
   bool hasRenderHook = original_RenderLightsabre_cdecl || original_RenderLightsabre_regcall;
   if (!hasRenderHook && !original_SetProperty) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_RenderLightsabre_cdecl)
      DetourDetach(&(PVOID&)original_RenderLightsabre_cdecl, hooked_RenderLightsabre_cdecl);
   if (original_RenderLightsabre_regcall)
      DetourDetach(&(PVOID&)original_RenderLightsabre_regcall, hooked_RenderLightsabre_regcall);
   if (original_SetProperty)
      DetourDetach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   DetourTransactionCommit();

   s_animTextures.clear();
}
