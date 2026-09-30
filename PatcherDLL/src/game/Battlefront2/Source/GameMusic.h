#pragma once

#include "core/build_field.hpp"

// Members differ between the layouts and are not modelled yet. Size from
// CommandPostClass: mMusicCapture[8] ends 0x60 later on debug, 0x40 on release.
class GameMusic;

namespace layout::GameMusic {
inline constexpr BuildSize kSize{0xC, 0x8};
}
