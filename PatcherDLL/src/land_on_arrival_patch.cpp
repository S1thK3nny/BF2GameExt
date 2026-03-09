#include "pch.h"

#include "land_on_arrival_patch.hpp"
#include "cfile.hpp"
#include "lua_hooks.hpp"
#include <detours.h>

// ============================================================================
// LandOnArrival fix — two patches:
//
// 1. JG NOP: The path follower advance checks NodeProperties.mLandOnArrival
//    (+0x22) on node arrival and sets EntityPathFollower.mbLandNow, which
//    triggers Land() in UpdatePathFollower.  A JG instruction skips the check
//    when mIndex > 0.  Fix: NOP the JG (Lua-toggled via EnableLandOnArrival).
//
// 2. Land() hook: mbLandNow is never cleared after Land() is called, so the
//    flyer re-lands every frame.  Fix: Detours hook on EntityFlyer::Land()
//    clears mbLandNow after calling the original.
// ============================================================================

// ---------------------------------------------------------------------------
// Globals for the JG NOP (Lua toggle)
// ---------------------------------------------------------------------------
uint8_t* g_landOnArrivalPatch = nullptr;
uint8_t  g_landOnArrivalOrigJG = 0;

// ---------------------------------------------------------------------------
// Per-build addresses
// ---------------------------------------------------------------------------
struct land_on_arrival_addrs {
   uintptr_t  id_rva;
   uint64_t   id_expected;
   uintptr_t  patch_offset;       // JG instruction offset
   uint8_t    jg_operand;         // JG displacement byte
   uintptr_t  land_func_offset;   // EntityFlyer::Land() offset
   uintptr_t  mbLandNow_off;      // mbLandNow offset within EntityFlyer
   uintptr_t  end_path_offset;    // EndPathFollowing cleanup function offset
   uintptr_t  pathFollower_off;   // EntityPathFollower offset within EntityFlyer
};

static const land_on_arrival_addrs MODTOOLS = {
   .id_rva           = 0x62b59c,
   .id_expected      = 0x746163696c707041,  // "Applicat"
   .patch_offset     = 0x1ed340,            // VA 0x5ed340
   .jg_operand       = 0x25,
   .land_func_offset = 0x0f1380,            // VA 0x4f1380
   .mbLandNow_off    = 0x4a1,              // 0x3CC + 0xD5
   .end_path_offset  = 0x1e7290,            // VA 0x5e7290
   .pathFollower_off = 0x3cc,
};

static const land_on_arrival_addrs STEAM = {
   .id_rva           = 0x39f834,
   .id_expected      = 0x746163696c707041,
   .patch_offset     = 0x0d52fd,            // VA 0x4d52fd
   .jg_operand       = 0x42,
   .land_func_offset = 0x0b3d50,            // VA 0x4b3d50
   .mbLandNow_off    = 0x49d,
   .end_path_offset  = 0x0d2970,            // VA 0x4d2970
   .pathFollower_off = 0x3cc,
};

static const land_on_arrival_addrs GOG = {
   .id_rva           = 0x3a0698,
   .id_expected      = 0x746163696c707041,
   .patch_offset     = 0x0d52fd,            // VA 0x4d52fd (same as Steam)
   .jg_operand       = 0x42,
   .land_func_offset = 0x0b3d50,            // VA 0x4b3d50 (same as Steam)
   .mbLandNow_off    = 0x49d,
   .end_path_offset  = 0x0d2970,            // VA 0x4d2970 (same as Steam)
   .pathFollower_off = 0x3cc,
};

// ---------------------------------------------------------------------------
// Detours hook on EntityFlyer::Land()
// ---------------------------------------------------------------------------
typedef void (__thiscall* fn_Land)(void* thisFlyer);
typedef void (__thiscall* fn_EndPathFollow)(void* pathFollower, void* pathFollowerClass);

static fn_Land          original_Land    = nullptr;
static fn_EndPathFollow s_endPathFollow  = nullptr;
static uintptr_t        s_mbLandNow_off  = 0;
static uintptr_t        s_pathFollower_off = 0;

static void __fastcall hooked_Land(void* thisFlyer, void* /*edx*/)
{
   // Check mbLandNow BEFORE calling original — if set, this is a
   // LandOnArrival-triggered landing and we should detach the path.
   uint8_t* pLandNow = (uint8_t*)((uintptr_t)thisFlyer + s_mbLandNow_off);
   bool wasLandOnArrival = (*pLandNow != 0);

   original_Land(thisFlyer);

   // Clear mbLandNow so the flyer doesn't re-land every frame
   *pLandNow = 0;

   // If triggered by LandOnArrival, detach the path follower so the
   // vehicle is released from path control and AI can re-enter it.
   if (wasLandOnArrival && s_endPathFollow) {
      void* pathFollower = (void*)((uintptr_t)thisFlyer + s_pathFollower_off);
      s_endPathFollow(pathFollower, nullptr);
   }
}

// ---------------------------------------------------------------------------
// Init / install / uninstall
// ---------------------------------------------------------------------------
void identify_land_on_arrival(uintptr_t exe_base)
{
   cfile log("BF2GameExt.log", "a");

   auto check_id = [&](const land_on_arrival_addrs& a) -> bool {
      uint64_t val = *(uint64_t*)(exe_base + a.id_rva);
      return val == a.id_expected;
   };

   const land_on_arrival_addrs* addrs = nullptr;
   const char* build_name = nullptr;

   if (check_id(MODTOOLS)) {
      addrs = &MODTOOLS;
      build_name = "modtools";
   } else if (check_id(STEAM)) {
      addrs = &STEAM;
      build_name = "Steam";
   } else if (check_id(GOG)) {
      addrs = &GOG;
      build_name = "GOG";
   } else {
      log.printf("[LandOnArrival] Build not recognized, skipping\n");
      return;
   }

   log.printf("[LandOnArrival] Identified %s build\n", build_name);

   // Locate JG patch site
   uint8_t* patch_addr = (uint8_t*)(exe_base + addrs->patch_offset);
   if (patch_addr[0] != 0x7F || patch_addr[1] != addrs->jg_operand) {
      log.printf("[LandOnArrival] WARNING: JG byte mismatch at %p (got %02X %02X, expected 7F %02X)\n",
                 patch_addr, patch_addr[0], patch_addr[1], addrs->jg_operand);
      return;
   }
   g_landOnArrivalPatch  = patch_addr;
   g_landOnArrivalOrigJG = addrs->jg_operand;
   log.printf("[LandOnArrival] JG patch site at %p\n", patch_addr);

   // Resolve Land() and EndPathFollow for Detours hook
   original_Land      = (fn_Land)(exe_base + addrs->land_func_offset);
   s_endPathFollow    = (fn_EndPathFollow)(exe_base + addrs->end_path_offset);
   s_mbLandNow_off    = addrs->mbLandNow_off;
   s_pathFollower_off = addrs->pathFollower_off;
   log.printf("[LandOnArrival] Land() at %p, EndPathFollow at %p, mbLandNow 0x%x, pathFollower 0x%x\n",
              (void*)original_Land, (void*)s_endPathFollow,
              (unsigned)s_mbLandNow_off, (unsigned)s_pathFollower_off);
}

void land_on_arrival_install()
{
   if (!original_Land) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_Land, hooked_Land);
   LONG result = DetourTransactionCommit();

   cfile log("BF2GameExt.log", "a");
   log.printf("[LandOnArrival] Land() hook %s (result=%ld)\n",
              result == NO_ERROR ? "installed" : "FAILED", result);
}

void land_on_arrival_uninstall()
{
   if (!original_Land) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_Land, hooked_Land);
   DetourTransactionCommit();
}
