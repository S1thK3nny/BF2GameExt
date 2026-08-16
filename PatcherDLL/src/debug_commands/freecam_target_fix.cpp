#include "pch.h"
#include "freecam_target_fix.hpp"

// =============================================================================
// FreecamTargetFix
//
// debugmenu.SetFreecamTarget fires a 500-unit ray and stores the hit in
// followThisObj (0x00B76C9C) as a raw pointer.  FreeCamera::Update then
// virtual-calls that object every frame, and nothing clears the pointer when
// the entity is destroyed — so spectating a soldier that dies faults on the
// next frame:
//
//   004ae332: MOV EDX,[0x00B76C9C]   ; followThisObj, still the dead entity
//   004ae345: TEST EDX,EDX           ; non-null, so the guard passes
//   004ae356: MOV EAX,[EDX]          ; "vtable" of a freed block -> 0xDDDDDDDD
//   004ae35f: CALL [EAX+4]           ; read AV at 0xDDDDDDE1
//
// The plain follow branch at 0x004ae3c0/0x004ae3d8 makes the identical call, as
// does the tether-capture block at the top of Update, so the pointer itself is
// what has to be validated rather than any one call site.
//
// Engine vtables live in the exe's .rdata (modtools 0x00A2A000..0x00AC3000;
// every vtable in game_addrs.hpp falls inside it).  Freed or recycled heap
// cannot, because the loader reserves the image range.  So a vtable pointer
// outside .rdata means the object is gone: drop the lock and let the camera
// fall back to manual control.
//
// The one-shot request flags (gSetTargetObj 0x00B76C36, gSetTetherPosition
// 0x00B76C38) are deliberately left alone, so re-locking onto something else in
// the same frame still works.
//
// Modtools only — SetFreecamTarget and the tether command do not exist on
// Steam/GOG (see docs/RE/FreeCameraSystem.md), and DebugCommandRegistry is
// already gated to GameBuild::Modtools.
// =============================================================================

static uint8_t* s_isFollowing  = nullptr;   // gIsFollowingObj
static uint8_t* s_tethered     = nullptr;   // gFollowingTethered
static void**   s_followTarget = nullptr;   // followThisObj

// .rdata bounds of the exe image, the only place a live vtable can point.
static uintptr_t s_rdataLo = 0;
static uintptr_t s_rdataHi = 0;

// ---------------------------------------------------------------------------

static void cache_rdata_bounds(uintptr_t exe_base)
{
   __try {
      IMAGE_DOS_HEADER*   dos = (IMAGE_DOS_HEADER*)exe_base;
      IMAGE_NT_HEADERS32* nt  = (IMAGE_NT_HEADERS32*)(exe_base + dos->e_lfanew);
      IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);

      for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
         if (memcmp(sec[i].Name, ".rdata", 6) == 0) {
            s_rdataLo = exe_base + sec[i].VirtualAddress;
            s_rdataHi = s_rdataLo + sec[i].Misc.VirtualSize;
            return;
         }
      }

      // No .rdata under that name: widen to the whole image.  Still sound —
      // the loader reserves the image range, so no heap block lives inside it.
      s_rdataLo = exe_base;
      s_rdataHi = exe_base + nt->OptionalHeader.SizeOfImage;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_rdataLo = 0;
      s_rdataHi = 0;   // leaves the guard disabled rather than guessing
   }
}

void FreecamTargetFix::install(uintptr_t exe_base)
{
   using namespace game_addrs::modtools;

   s_isFollowing  = (uint8_t*)resolve(exe_base, freecam_is_following_obj);
   s_tethered     = (uint8_t*)resolve(exe_base, freecam_following_tethered);
   s_followTarget = (void**)  resolve(exe_base, freecam_follow_this_obj);

   cache_rdata_bounds(exe_base);
}

void FreecamTargetFix::preFreeCamUpdate()
{
   if (!s_followTarget || s_rdataHi == 0) return;

   void* target = *s_followTarget;
   if (!target) return;

   bool stale = true;
   __try {
      uintptr_t vtable = *(uintptr_t*)target;
      stale = (vtable < s_rdataLo || vtable >= s_rdataHi);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      stale = true;   // block decommitted outright
   }
   if (!stale) return;

   *s_followTarget = nullptr;
   if (s_isFollowing) *s_isFollowing = 0;
   if (s_tethered)    *s_tethered    = 0;

   auto fn_log = get_gamelog();
   fn_log("[FreeCam] follow target %p was destroyed - lock released\n", target);
}
