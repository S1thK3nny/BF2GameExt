#pragma once

#include "core/build_field.hpp"

// Derives from GameSound and adds nothing, so it has GameSound's size. The
// CommandPost constructor builds its voice-over streams in 0x14 steps on debug
// (modtools 0x0064A4EA) and inside 0x140-byte VoiceOvers of 40 on release.
class GameSoundStream;

namespace layout::GameSoundStream {
inline constexpr BuildSize kSize{0x14, 0x8};
}
