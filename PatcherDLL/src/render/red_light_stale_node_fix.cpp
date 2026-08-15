#include "pch.h"
#include "red_light_stale_node_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>

#include <cstring>

// =============================================================================
// Freecam light stale-pointer fix, and a RedLight::Deactivate backstop.
// See the header for what each is for; the engine detail is below.
//
// THE LIST
//   A RedLight carries two intrusive list nodes, each {_pList, _pPrev, _pNext,
//   _pOwner}: one at light+0x30 for the single global light list, one at
//   light+0x40 for whichever visible list the renderer put it in.  Bit 0x400 in
//   the flags word at light+0x04 is the "I am in the global list" marker.
//
//   RedLight::Activate (modtools 0x0082F7C0) is the only thing that links node1,
//   and it writes _pOwner = the light itself:
//
//       lea edx,[ecx+0x30]
//       mov [edx+0xc],ecx        <- owner = this, the invariant we test
//       mov [edx],0xae3ae0       <- _pList = &s_GlobalList
//       ...
//       or  [ecx+4],0x400
//
//   The drain used by RedLight::InitSys / DeinitSys (modtools 0x0082F770, called
//   from 0x00830090 and 0x008301C8, which walk all four list heads) unlinks
//   every node the fast way:
//
//       mov [eax],edx            <- _pList = 0
//       mov [eax+0xc],edx        <- _pOwner = 0
//       mov [esi+8],edi          } splice out
//       mov [esi+4],eax          }
//
//   It never clears the light's own 0x400 flag and leaves _pPrev/_pNext
//   dangling.  RedLight::Deactivate then trusts the flag; its *second* node is
//   guarded on _pList != 0, its first has no guard at all:
//
//       test ah,4                <- flags & 0x400, the only check
//       je   ret
//       dec  [s_GlobalList._iCount]
//       mov  eax,[ecx+0x34]      <- _pPrev, stale after a drain
//       mov  [eax+8],esi         <- WILD WRITE
//       mov  [eax+4],esi         <- WILD WRITE
//
//   hooked_Deactivate below adds the missing test.  Activate is the only writer
//   of node1's owner and always sets it to the light; the drain is the only
//   thing that clears it.  So "flag set but owner is not me" is exact, not a
//   heuristic.  Clearing node2 alongside node1 is safe because the two call
//   sites that drain node1's list drain all four in the same call, so node2's
//   _pList is already zero and the counters have been reconciled by the drain.
//
//   Function is byte-identical on modtools, Steam and GOG (verified against all
//   three images): __thiscall, no stack arguments, bare RET, flags at +0x04,
//   node1 at +0x30, node2 at +0x40.
//
// WHY THAT IS NOT THE FREECAMLIGHT CRASH
//   It is not reached.  With a stale `s_pFreeCamLight` the disable path faults
//   on `call [eax+8]` at modtools 0x004ADA3C, before Deactivate is entered, on a
//   vtable pointer read out of a recycled pool block.  freecam_light_reset()
//   below is what actually fixes that; the guard is defence in depth for lights
//   that outlive a drain, which on retail means our own saber lights.
// =============================================================================

static constexpr int      kLight_Flags    = 0x04;
static constexpr unsigned kLightLinkedBit = 0x400;
static constexpr int      kLight_Node     = 0x30;
static constexpr int      kNode_Owner     = 0x0C;
static constexpr int      kBothNodesSize  = 0x20; // node1 + node2, 0x30..0x4F

using fn_light_deactivate_t = void(__fastcall*)(void* ecx, void* edx);

static fn_light_deactivate_t original_Deactivate = nullptr;
static bool                  g_installed         = false;

static void** g_pFreeCamLight = nullptr;

// One-shot log guards. A corrupt list tends to repeat, and this runs inside the
// renderer - a line per light per frame would be its own denial of service.
static bool g_loggedStale = false;

// Install runs from dllmain, long before the engine opens its logfile, so a line
// written there is thrown away. Defer the breadcrumb to the first time the hook
// is actually entered, which is also the only proof that matters: if this line
// is absent from the log after using a light, the detour is not live.
static bool g_loggedAlive = false;

// ---------------------------------------------------------------------------
// The actual freecamlight fix
// ---------------------------------------------------------------------------

void freecam_light_reset()
{
   // Nothing to free: the block went with the level. Dropping the pointer is the
   // whole fix - disable becomes a no-op and enable builds a fresh light.
   if (g_pFreeCamLight) *g_pFreeCamLight = nullptr;
}

// ---------------------------------------------------------------------------
// RedLight::Deactivate backstop
// ---------------------------------------------------------------------------

static void __fastcall hooked_Deactivate(void* ecx, void* edx)
{
   if (!g_loggedAlive) {
      g_loggedAlive = true;
      get_gamelog()("[RedLightStaleNodeFix] guard is live\n");
   }

   uint8_t* light = (uint8_t*)ecx;
   if (!light) return; // the engine would fault on the flags read

   unsigned& flags = *(unsigned*)(light + kLight_Flags);

   if (flags & kLightLinkedBit) {
      const void* owner = *(void* const*)(light + kLight_Node + kNode_Owner);
      if (owner != (const void*)light) {
         // The drain has already been here: links are stale, do not follow them.
         flags &= ~kLightLinkedBit;
         std::memset(light + kLight_Node, 0, kBothNodesSize);
         if (!g_loggedStale) {
            g_loggedStale = true;
            get_gamelog()("[RedLightStaleNodeFix] skipped unlink of a light the "
                          "engine had already drained (light %p)\n", (void*)light);
         }
         return;
      }
   }

   original_Deactivate(ecx, edx);
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------

void red_light_stale_node_fix_install(uintptr_t exe_base)
{
   // modtools-only: retail has no freecamlight command family and no allocator
   // for the light, so the pointer can never go stale there.
   if (g_addr->freecam_light_ptr != 0)
      g_pFreeCamLight = (void**)resolve(exe_base, g_addr->freecam_light_ptr);

   if (g_addr->red_light_deactivate == 0) return; // unknown build, stay out

   original_Deactivate =
      (fn_light_deactivate_t)resolve(exe_base, g_addr->red_light_deactivate);

   // Prologue check, so an image whose codegen we have not seen no-ops instead
   // of detouring something else. modtools reads the flags through ECX directly;
   // Steam and GOG copy ECX to EDX first. Both then TEST the 0x400 bit.
   const uint8_t* p = (const uint8_t*)original_Deactivate;
   static constexpr uint8_t kModtools[] = {0x8B, 0x41, 0x04, 0xF6, 0xC4, 0x04};
   static constexpr uint8_t kRetail[]   = {0x8B, 0xD1, 0xF7, 0x42, 0x04,
                                           0x00, 0x04, 0x00, 0x00};

   const bool ok = (g_build == GameBuild::Modtools)
                      ? std::memcmp(p, kModtools, sizeof(kModtools)) == 0
                      : std::memcmp(p, kRetail, sizeof(kRetail)) == 0;
   if (!ok) {
      original_Deactivate = nullptr;
      return; // nothing to log to yet; see the deferred breadcrumb above
   }

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_Deactivate, hooked_Deactivate);
   DetourTransactionCommit();

   g_installed = true;
}

void red_light_stale_node_fix_uninstall()
{
   if (!g_installed) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_Deactivate, hooked_Deactivate);
   DetourTransactionCommit();

   g_installed = false;
}
