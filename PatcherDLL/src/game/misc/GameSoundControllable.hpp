#pragma once

#include <cstdint>

namespace game {

// -------------------------------------------------------------------------
// GameSoundControllable (from PDB)
// -------------------------------------------------------------------------

struct GameSoundControllable {
    uint16_t mVoiceVirtualHandle;  // +0x00
    uint8_t  mFlags;               // +0x02
    char     _pad03;               // +0x03
};
static_assert(sizeof(GameSoundControllable) == 0x04);

} // namespace game
