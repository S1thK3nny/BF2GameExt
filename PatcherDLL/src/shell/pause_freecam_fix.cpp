#include "pch.h"
#include "pause_freecam_fix.hpp"

#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

// =============================================================================
// Pause-menu freecam state across missions
//
// PauseMenu::_SetFreeCamera flips mFreecamSetting (+0x20) and only acts when it
// differs from mFreecamView (+0x19), the state actually applied.  PauseMenu::_Init
// clears mFreecamSetting every mission but leaves mFreecamView alone; only the
// static constructor zeroes it.  Leave a match in freecam and the next one
// starts with Setting 0 / View 1, so:
//
//   press 1: Setting -> 1 == View   nothing happens, Lua label flips anyway
//   press 2: Setting -> 0 != View   "exits" the freecam that never started
//   press 3: works
//
// GameLoop::sFreeMode is never reset either, and GameLoop::SetFreeMode(true)
// early-outs on it, so clearing the view flag alone would close the menu
// without moving the camera.  Both are raw-cleared: the CameraManager is
// brand new at this point, so there is no camera to switch back.
//
// Same fields, stride and _Init on modtools, Steam and GOG (addresses in
// game_addrs.hpp).  Stock retail hides the button; the v1.3 patch shows it.
// =============================================================================

static constexpr int kPauseMenuStride       = 0x64;
static constexpr int kPauseMenu_FreecamView = 0x19;
static constexpr uint32_t kMaxViewports     = 4;

static uint8_t** s_pauseMenus = nullptr; // -> PauseMenu[]
static uint32_t* s_numCameras = nullptr; // GameLoop::mNumCameras
static uint8_t*  s_freeMode   = nullptr; // GameLoop::sFreeMode

void pause_freecam_fix_install(uintptr_t exe_base)
{
   if (!g_addr->pause_menu_instances_ptr || !g_addr->game_loop_num_cameras ||
       !g_addr->game_loop_free_mode)
      return;

   s_pauseMenus = (uint8_t**)resolve(exe_base, g_addr->pause_menu_instances_ptr);
   s_numCameras = (uint32_t*)resolve(exe_base, g_addr->game_loop_num_cameras);
   s_freeMode   = (uint8_t*) resolve(exe_base, g_addr->game_loop_free_mode);
}

void pause_freecam_fix_reset()
{
   if (!s_pauseMenus) return;

   // Same loop bound PauseMenu::Init uses.
   uint8_t* menus = *s_pauseMenus;
   if (menus) {
      for (uint32_t i = 0; i < *s_numCameras && i < kMaxViewports; ++i)
         menus[i * kPauseMenuStride + kPauseMenu_FreecamView] = 0;
   }
   *s_freeMode = 0;
}
