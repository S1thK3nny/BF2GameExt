#pragma once

#include <stdint.h>

// CameraManager::sInstance (game_addrs camera_manager_instance) -> mRedCamera[0],
// the camera the HUD draws for, as target_bar_latch.cpp reads it on every build.
namespace layout::CameraManager {

constexpr uint32_t kRedCamera0 = 0x24;

} // namespace layout::CameraManager
