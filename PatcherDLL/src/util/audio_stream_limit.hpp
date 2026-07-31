#pragma once

#include <stdint.h>

// Audio Stream Limit Increase — runtime half.
//
// The binary patches in patch_table.cpp do all the real work (relocating
// Snd::EngineBase::smStreams and the Snd::SoundStream per-slot arrays, and
// widening every loop bound).  What they cannot do is widen
// Snd::SoundStream::Init, which unrolls its per-slot initialization six times;
// this detour re-runs that initialization for the slots beyond the stock six.
//
// No-op unless the patch set actually applied.
void audio_stream_limit_install(uintptr_t exe_base);
void audio_stream_limit_uninstall();
