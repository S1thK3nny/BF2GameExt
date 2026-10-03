#pragma once

#include <stdint.h>

#include "core/build_field.hpp"

// Members differ between the layouts and are not modelled yet. Sizes from the
// GameSound arrays in CommandPostClass: mSoundAmbience[8] ends 0xA0 later on
// debug, 0x40 later on release (see CommandPost.h).
class GameSound;

namespace layout::GameSound {
inline constexpr BuildSize kSize{0x14, 0x8};
}

// Same on every build: CommandPost::mSoundAmbience to mSoundAmbienceProps is 4
// bytes on both.
struct GameSoundControllable {
   uint16_t mVoiceVirtualHandle;
   uint8_t  mFlags;
};
static_assert(sizeof(GameSoundControllable) == 0x4, "GameSoundControllable");
