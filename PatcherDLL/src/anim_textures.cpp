#include "pch.h"
#include "anim_textures.hpp"
#include "lua_hooks.hpp"

#include <detours.h>
#include <unordered_map>

// =============================================================================
// AnimTexture lightsaber system
// =============================================================================
// Xbox SWBF2 has AnimTexture1-3 ODF properties that give lightsaber blades a
// 4-frame animated texture cycle. PC uses a single static texture.
//
// Implementation:
//   1. Hook WeaponMeleeClass::SetProperty — intercept AnimTexture1/2/3 hashes,
//      read the blade's base texture hash, store anim textures in external map.
//   2. Hook _RenderLightsabre — look up incoming texture hash in map, cycle
//      through base + 3 anim textures based on a timer.
//
// The blade struct layout is NOT modified. All anim data lives in our map,
// keyed by the base texture hash (uint32_t).

// ---------------------------------------------------------------------------
// Property hashes (FNV-1a of "AnimTexture1", "AnimTexture2", "AnimTexture3")
// ---------------------------------------------------------------------------
static constexpr uint32_t HASH_ANIM_TEXTURE_1 = 0x2c47e9ee;
static constexpr uint32_t HASH_ANIM_TEXTURE_2 = 0x2b47e85b;
static constexpr uint32_t HASH_ANIM_TEXTURE_3 = 0x2a47e6c8;

// ---------------------------------------------------------------------------
// FNV-1a hash (PblHash) — matches the game's case-insensitive hash function
// ---------------------------------------------------------------------------
static uint32_t pbl_hash(const char* str)
{
   uint32_t h = 0x811c9dc5u;
   for (; *str; ++str)
      h = (h ^ ((uint8_t)*str | 0x20)) * 0x01000193u;
   return h;
}

// ---------------------------------------------------------------------------
// Animation data — keyed by base texture hash
// ---------------------------------------------------------------------------
struct AnimTextureSet {
   uint32_t frames[4]; // [0]=base, [1]=AnimTex1, [2]=AnimTex2, [3]=AnimTex3
};

static std::unordered_map<uint32_t, AnimTextureSet> s_animTextures;

// Animation timer — simple 4-frame cycle
static DWORD s_lastFrameTick  = 0;
static int   s_animFrameIndex = 0;
static constexpr DWORD ANIM_FRAME_MS = 66; // ~15 FPS texture cycle (4 frames @ ~60ms)

static void update_anim_timer()
{
   DWORD now = GetTickCount();
   if (now - s_lastFrameTick >= ANIM_FRAME_MS) {
      s_lastFrameTick = now;
      s_animFrameIndex = (s_animFrameIndex + 1) & 3; // cycle 0-3
   }
}

// ---------------------------------------------------------------------------
// Shared texture resolution — returns the animated texture hash for this frame,
// or the original hash if no anim textures are registered.
// ---------------------------------------------------------------------------
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
// Hook: _RenderLightsabre — MODTOOLS (pure __cdecl, all 7 params on stack)
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
// Hook: _RenderLightsabre — STEAM/GOG (LTCG hybrid: ECX=pos, EDX=dir,
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
// Hook: WeaponMeleeClass::SetProperty
// ---------------------------------------------------------------------------
// void __thiscall WeaponMeleeClass::SetProperty(this, uint propertyHash, char* value)
//
// On PC, blade array is at this+0x3DC (pointer to array of 0x34-byte entries).
// Blade texture hash is at blade_entry+0x28 (dword index 10).

using fn_SetProperty = void(__thiscall*)(void*, uint32_t, const char*);
static fn_SetProperty original_SetProperty = nullptr;

// Blade lookup: FUN_00635180 — __thiscall, RET 0x0C
// int __thiscall BladeLookup(this=WeaponMeleeClass*, int model, uint bladeNameHash, uint unknown)
// On PC, the SetProperty code uses globals DAT_00b92ad8 (model), DAT_00b92ad4 (bladeNameHash),
// DAT_00b92ad0 (unknown) which are set when a "Blade" property is parsed.
// We replicate by reading those globals directly.
static uintptr_t s_bladeLookupAddr  = 0;
static uintptr_t s_bladeGlobal_model    = 0; // DAT_00b92ad8 — RedModel*
static uintptr_t s_bladeGlobal_nameHash = 0; // DAT_00b92ad4 — blade name hash
static uintptr_t s_bladeGlobal_unk      = 0; // DAT_00b92ad0 — unknown param
static unsigned  s_bladeArrayOffset     = 0; // offset from class to blade array ptr (modtools: 0x3DC, Steam: 0x2C8)

// __thiscall: ECX=this, 3 stack params, callee cleans 12 bytes
using fn_BladeLookup = int(__thiscall*)(void*, int, uint32_t, uint32_t);

static int blade_lookup(void* weaponMeleeClass)
{
   if (!s_bladeLookupAddr) return -1;
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - 0x400000u + base; };

   auto fn = (fn_BladeLookup)res(s_bladeLookupAddr);
   int model    = *(int*)res(s_bladeGlobal_model);
   uint32_t nameHash = *(uint32_t*)res(s_bladeGlobal_nameHash);
   uint32_t unk      = *(uint32_t*)res(s_bladeGlobal_unk);

   return fn(weaponMeleeClass, model, nameHash, unk);
}

static void register_anim_texture(void* weaponMeleeClass, int frameSlot, const char* textureName)
{
   int bladeIdx = blade_lookup(weaponMeleeClass);
   if (bladeIdx < 0) return;

   // Read blade array pointer and get base texture hash
   uint32_t* bladeArrayPtr = *(uint32_t**)((char*)weaponMeleeClass + s_bladeArrayOffset);
   if (!bladeArrayPtr) return;

   // Each blade entry is 0x34 bytes = 13 dwords. Texture hash at dword index 10 (offset 0x28).
   uint32_t* bladeEntry = (uint32_t*)((char*)bladeArrayPtr + bladeIdx * 0x34);
   uint32_t baseTex = bladeEntry[10]; // blade+0x28

   if (!baseTex) {
      dbg_log("[AnimTex] WARNING: base texture hash is 0 for blade %d — AnimTexture%d ignored\n",
              bladeIdx, frameSlot);
      return;
   }

   uint32_t animHash = pbl_hash(textureName);

   auto& entry = s_animTextures[baseTex];
   entry.frames[0] = baseTex;
   entry.frames[frameSlot] = animHash;

   dbg_log("[AnimTex] Registered AnimTexture%d: base=0x%08X anim=0x%08X (\"%s\")\n",
           frameSlot, baseTex, animHash, textureName);
}

static void __fastcall hooked_SetProperty(void* thisPtr, void* /*edx*/, uint32_t propHash, const char* value)
{
   // Let the original handle the property first (so base Texture is set before we read it)
   original_SetProperty(thisPtr, propHash, value);

   // Check for our AnimTexture properties
   switch (propHash) {
      case HASH_ANIM_TEXTURE_1:
         register_anim_texture(thisPtr, 1, value);
         break;
      case HASH_ANIM_TEXTURE_2:
         register_anim_texture(thisPtr, 2, value);
         break;
      case HASH_ANIM_TEXTURE_3:
         register_anim_texture(thisPtr, 3, value);
         break;
   }
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

// Per-build addresses
struct AnimTexAddrs {
   uintptr_t render_lightsabre;
   uintptr_t set_property;
   uintptr_t blade_lookup;
   uintptr_t blade_global_model;
   uintptr_t blade_global_namehash;
   uintptr_t blade_global_unk;
   unsigned  blade_array_offset;    // offset from WeaponMeleeClass* to blade array pointer
};

static constexpr AnimTexAddrs MODTOOLS_ADDRS = {
   .render_lightsabre    = 0x00633660,
   .set_property         = 0x0063ab40,
   .blade_lookup         = 0x00635180,
   .blade_global_model   = 0x00b92ad8,
   .blade_global_namehash = 0x00b92ad4,
   .blade_global_unk     = 0x00b92ad0,
   .blade_array_offset   = 0x3DC,
};

static constexpr AnimTexAddrs STEAM_ADDRS = {
   .render_lightsabre    = 0x0068f260,
   .set_property         = 0x0068d880,
   .blade_lookup         = 0x0068ed40,
   .blade_global_model   = 0x01fac3a0,
   .blade_global_namehash = 0x01fac39c,
   .blade_global_unk     = 0x01fac3a4,
   .blade_array_offset   = 0x2C8,
};

static constexpr AnimTexAddrs GOG_ADDRS = {
   .render_lightsabre    = 0x006902f0,
   .set_property         = 0x0068e910,
   .blade_lookup         = 0x0068fdd0,
   .blade_global_model   = 0x01fad850,
   .blade_global_namehash = 0x01fad84c,
   .blade_global_unk     = 0x01fad854,
   .blade_array_offset   = 0x2C8,
};

void anim_textures_install(uintptr_t exe_base)
{
   const AnimTexAddrs* addrs = nullptr;
   switch (g_exeType) {
      case ExeType::MODTOOLS: addrs = &MODTOOLS_ADDRS; break;
      case ExeType::STEAM:    addrs = &STEAM_ADDRS;    break;
      case ExeType::GOG:      addrs = &GOG_ADDRS;      break;
      default:
         dbg_log("[AnimTex] Skipping — unsupported build\n");
         return;
   }

   auto resolve = [=](uintptr_t addr) -> uintptr_t {
      return addr - 0x400000u + exe_base;
   };

   s_bladeLookupAddr       = addrs->blade_lookup;
   s_bladeGlobal_model     = addrs->blade_global_model;
   s_bladeGlobal_nameHash  = addrs->blade_global_namehash;
   s_bladeGlobal_unk       = addrs->blade_global_unk;
   s_bladeArrayOffset      = addrs->blade_array_offset;

   original_SetProperty = (fn_SetProperty)resolve(addrs->set_property);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());

   // _RenderLightsabre calling convention differs between builds:
   //   Modtools: __cdecl (all 7 params on stack)
   //   Steam/GOG: LTCG hybrid (ECX=pos, EDX=dir, 5 stack args, caller-clean)
   if (g_exeType == ExeType::MODTOOLS) {
      original_RenderLightsabre_cdecl = (fn_RenderLightsabre_cdecl)resolve(addrs->render_lightsabre);
      DetourAttach(&(PVOID&)original_RenderLightsabre_cdecl, hooked_RenderLightsabre_cdecl);
   } else {
      original_RenderLightsabre_regcall = (void*)resolve(addrs->render_lightsabre);
      DetourAttach(&(PVOID&)original_RenderLightsabre_regcall, hooked_RenderLightsabre_regcall);
   }

   DetourAttach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   LONG result = DetourTransactionCommit();

   if (result == NO_ERROR) {
      dbg_log("[AnimTex] Hooks installed successfully\n");
   } else {
      dbg_log("[AnimTex] ERROR: Detours commit failed (%ld)\n", result);
   }
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
   dbg_log("[AnimTex] Hooks removed\n");
}
