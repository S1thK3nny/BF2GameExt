#include "pch.h"
#include "hud_weapon_icon_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

// =============================================================================
// See the header for the symptom and the shape of the fix. This file documents
// the layouts and calling conventions it relies on.
//
// HUD::Event -- 8 bytes, POD, empty destructor
//     +0x00  EventClass* mClass
//     +0x04  union Data  mData      { int, uint, float, wchar_t*, RedColor*, ... }
//   Confirmed by HUD::EventQueue::Update walking gEventList with a stride of 0xC
//   over DelayedEvent { Event e; float activationTime; }, and by Event::Send
//   doing `mov eax,[ebx]` then `mov esi,[eax+8]`.
//
// HUD::EventClass -- 32 bytes
//     +0x00  uint          mHashID
//     +0x04  Type          mType         1 Bool 2 Int 3 Uint 4 Float 5 Model
//                                        6 Texture 7 Color 8 String 9 Vector3
//     +0x08  PblListDouble mHandlerList
//   The +0x08 handler list is what Event::Send indexes, which pins +0x04.
//
// HUD::Transform (base) / HUD::TransformNameMesh
//     +0x1C  EventHandler  mEventInput
//     +0x30  EventClass*   mEventClassOutput
//     +0x38  NameMesh*     mMapping
//     +0x3C  uint          mNumMappings
//     +0x40  Node          mTransformNameMeshNode
//   All four are read directly by EventInput on modtools:
//     006cb101  mov eax,[ebx+0x30]      <- mEventClassOutput
//     006cb10c  mov eax,[ebx+0x3c]      <- mNumMappings
//     006cb116  call FindNameMesh       <- reads [ecx+0x3c] / [ecx+0x38]
//
// sTransformNameMeshList is a PblListSingle whose terminator IS the global, and
// whose node sits at item+0x40. From TransformNameMesh::FindByHashID:
//     006cb1b2  mov esi,[0xaf0ba0]      <- head->next
//     006cb1b8  cmp esi,0xaf0ba0        <- == &terminator means empty
//     006cb1c5  lea edi,[esi-0x40]      <- recover the item from the node
//     006cb1d3  mov esi,[esi]           <- node->next is at node+0
//
// TransformNameMesh::FindNameMesh is __thiscall NameMesh*(this, uint hash) with
// RET 4. It builds a 0x14-byte key on the stack, bsearches mMapping, and destroys
// the key; it mutates neither the transform nor any NameMesh, so calling it
// speculatively on a transform that is not the one being dispatched is safe.
// =============================================================================

// [Fixes] WeaponIconFix
bool g_hudWeaponIconFixEnabled = true;

// ---- layout constants -------------------------------------------------------

static constexpr int      kEvent_Data              = 0x04;
static constexpr int      kEventClass_Type         = 0x04;
static constexpr unsigned kType_Uint               = 3;
static constexpr int      kTransform_EventOutput   = 0x30;
static constexpr int      kTNM_NumMappings         = 0x3C;
static constexpr int      kTNM_Node                = 0x40;

// A corrupt or mid-construction list must not spin the render thread forever.
// The stock game builds a handful of these; a session with several mods loaded is
// still comfortably inside two digits.
static constexpr int kMaxListWalk = 256;

// ---- resolved engine entry points -------------------------------------------

using fn_tnm_event_input_t = void(__cdecl*)(void* ev, void* self);

// __thiscall(this, uint) reached as __fastcall: ECX = this, EDX unused, hash on
// the stack, callee cleans (RET 4).
using fn_tnm_find_name_mesh_t = void*(__fastcall*)(void* ecx, void* edx, unsigned hash);

static fn_tnm_event_input_t    original_EventInput = nullptr;
static fn_tnm_find_name_mesh_t g_findNameMesh      = nullptr;
static void**                  g_tnmList           = nullptr;
static bool                    g_installed         = false;

// install_log() is the ONLY logger that may run during install: dllmain holds the
// exe sections at PAGE_READWRITE (non-executable) then, so calling the engine's
// own logger there is an EXEC access violation on DEP builds. Runtime code uses
// get_gamelog() instead. Same split as gc_visual_limits.cpp.
static void install_log(const char* fmt, ...)
{
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

// ---------------------------------------------------------------------------
// Arbitration
// ---------------------------------------------------------------------------

// True when some other transform feeding the same output event has a real mapping
// for this mesh-name hash, and we do not. In that case our pass-through would
// overwrite its answer, so we stay quiet and let it win.
static bool another_transform_owns(const void* ev, const void* self)
{
   if (!g_findNameMesh || !g_tnmList || !ev || !self) return false;

   const uint8_t* e = (const uint8_t*)ev;

   const void* cls = *(void* const*)e;
   if (!cls) return false;

   // The engine only extracts a hash for type_Uint; anything else leaves hash 0
   // and takes an early return, so there is nothing to arbitrate.
   if (*(const unsigned*)((const uint8_t*)cls + kEventClass_Type) != kType_Uint) return false;

   const unsigned hash = *(const unsigned*)(e + kEvent_Data);
   if (hash == 0) return false;

   const uint8_t* me    = (const uint8_t*)self;
   const void*    myOut = *(void* const*)(me + kTransform_EventOutput);
   if (!myOut) return false; // engine returns early too

   // Do we have it ourselves? Then behave exactly as stock.
   if (*(const unsigned*)(me + kTNM_NumMappings) != 0 &&
       g_findNameMesh((void*)me, nullptr, hash) != nullptr)
      return false;

   const uint8_t* const end  = (const uint8_t*)g_tnmList; // terminator is the global itself
   const uint8_t*       node = (const uint8_t*)*g_tnmList;

   for (int guard = 0; node && node != end && guard < kMaxListWalk; ++guard) {
      const uint8_t* other = node - kTNM_Node;
      node                 = *(const uint8_t* const*)node; // node->next is at node+0

      if (other == me) continue;
      if (*(void* const*)(other + kTransform_EventOutput) != myOut) continue;
      if (*(const unsigned*)(other + kTNM_NumMappings) == 0) continue;

      if (g_findNameMesh((void*)other, nullptr, hash) != nullptr) return true;
   }
   return false;
}

// Runs inside HUD event dispatch on every weapon change. Deliberately silent: the
// install-time line in BF2GameExt.log is enough to confirm the hook is live, and a
// per-dispatch logger here is a flood risk for no diagnostic value once it works.
static void __cdecl hooked_TNM_EventInput(void* ev, void* self)
{
   if (another_transform_owns(ev, self)) return;
   original_EventInput(ev, self);
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------

void hud_weapon_icon_fix_install(uintptr_t exe_base)
{
   if (!g_hudWeaponIconFixEnabled) return;

   // 0 on any build the addresses have not been ported to (Steam, GOG). Not worth
   // a log line: it is the expected state there, not a failure.
   if (g_addr->hud_tnm_event_input == 0 || g_addr->hud_tnm_find_name_mesh == 0 ||
       g_addr->hud_tnm_list == 0)
      return;

   void* eventInput = resolve(exe_base, g_addr->hud_tnm_event_input);
   void* findMesh   = resolve(exe_base, g_addr->hud_tnm_find_name_mesh);

   // Byte guards. Both functions are called or detoured directly, so an image
   // whose codegen we have not seen must no-op rather than land in the middle of
   // something else. Nothing is logged here: this runs inside the install window,
   // where the game's own code is not callable.
   // Two codegens: the modtools debug build frames on ESP, the LTCG retail builds
   // frame on EBP. Steam and GOG are byte-identical across these ten bytes (only
   // call/data operands differ, and none of those land this early), so one retail
   // pair covers both. Each is a clean instruction boundary past five bytes, which
   // is what Detours needs to relocate.
   static constexpr uint8_t kEventInputModtools[] = {
      0x83, 0xEC, 0x08,             // sub  esp,8
      0x53, 0x56,                   // push ebx / push esi
      0x8B, 0x74, 0x24, 0x14,       // mov  esi,[esp+0x14]
      0x57                          // push edi
   };
   static constexpr uint8_t kFindNameMeshModtools[] = {
      0x83, 0xEC, 0x14,             // sub  esp,0x14
      0x56, 0x8B, 0xF1,             // push esi / mov esi,ecx
      0x8D, 0x4C, 0x24, 0x04        // lea  ecx,[esp+4]
   };
   static constexpr uint8_t kEventInputRetail[] = {
      0x55, 0x8B, 0xEC,             // push ebp / mov ebp,esp
      0x8B, 0x55, 0x08,             // mov  edx,[ebp+8]
      0x83, 0xEC, 0x08,             // sub  esp,8
      0x8B                          // mov  ecx,edx
   };
   static constexpr uint8_t kFindNameMeshRetail[] = {
      0x55, 0x8B, 0xEC,             // push ebp / mov ebp,esp
      0x83, 0xEC, 0x14,             // sub  esp,0x14
      0x56, 0x8B, 0xF1,             // push esi / mov esi,ecx
      0x8D                          // lea  ecx,[ebp-0x14]
   };
   static_assert(sizeof(kEventInputModtools) == sizeof(kEventInputRetail), "guard sizes");
   static_assert(sizeof(kFindNameMeshModtools) == sizeof(kFindNameMeshRetail), "guard sizes");

   const bool     isModtools           = (g_build == GameBuild::Modtools);
   const uint8_t* kEventInputPrologue   = isModtools ? kEventInputModtools : kEventInputRetail;
   const uint8_t* kFindNameMeshPrologue = isModtools ? kFindNameMeshModtools : kFindNameMeshRetail;

   // A mismatch here is almost always an address derived from the wrong image, and
   // a silent decline is expensive to diagnose from a play test. Say so, with the
   // bytes we actually found.
   constexpr size_t kGuardLen = sizeof(kEventInputModtools); // both pairs are this long

   if (std::memcmp(eventInput, kEventInputPrologue, kGuardLen) != 0 ||
       std::memcmp(findMesh, kFindNameMeshPrologue, kGuardLen) != 0) {
      const uint8_t* a = (const uint8_t*)eventInput;
      const uint8_t* b = (const uint8_t*)findMesh;
      install_log("[WeaponIconFix] NOT installed: prologue mismatch. "
                  "EventInput @0x%08X = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X; "
                  "FindNameMesh @0x%08X = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                  (unsigned)g_addr->hud_tnm_event_input,
                  a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9],
                  (unsigned)g_addr->hud_tnm_find_name_mesh,
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9]);
      return;
   }

   g_findNameMesh      = (fn_tnm_find_name_mesh_t)findMesh;
   g_tnmList           = (void**)resolve(exe_base, g_addr->hud_tnm_list);
   original_EventInput = (fn_tnm_event_input_t)eventInput;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)original_EventInput, hooked_TNM_EventInput);
   DetourTransactionCommit();

   g_installed = (r == NO_ERROR);
   install_log("[WeaponIconFix] %s (EventInput 0x%08X, FindNameMesh 0x%08X, list 0x%08X)",
               g_installed ? "installed" : "DetourAttach failed",
               (unsigned)g_addr->hud_tnm_event_input,
               (unsigned)g_addr->hud_tnm_find_name_mesh, (unsigned)g_addr->hud_tnm_list);
}

void hud_weapon_icon_fix_uninstall()
{
   if (!g_installed) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_EventInput, hooked_TNM_EventInput);
   DetourTransactionCommit();

   g_installed = false;
}
