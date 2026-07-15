#pragma once

#include <stdint.h>

// Enables GameSoundEngine::gEnableSoundWarnings, the bool that gates the
// "GameSound::SetID - Unable to find sound property 0x%08x" RedWarning (and the
// sibling "Sound %s - %s(0x%08x) not loaded" warning).  The game only ever reads
// this flag; its image default is 0, so those missing-sound warnings never fire.
// When [Features] EnableSoundWarnings is set, we write 1 so they surface.
//
// Modtools-only: retail (Steam/GOG) compiled the warning code out entirely — the
// sole GameSound::SetID has no such read and the format strings are stripped — so
// there is nothing to enable there.  No-ops on retail and unidentified builds.

extern bool g_enableSoundWarnings;

void enable_sound_warnings_install(uintptr_t exe_base);
void enable_sound_warnings_uninstall();
