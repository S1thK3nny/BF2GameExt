#pragma once

#include <stdint.h>

// =============================================================================
// Snd::Engine::Open failure fixes (all three builds, always on).
//
// 1. Sound heap fallback.  Engine::Open mallocs one contiguous block for sample
//    RAM: 32 MB stock, 256 MB with the Sound Limit Extension patch set.  In a
//    2 GB, non-large-address-aware process a 256 MB contiguous block is not
//    always there.  The modtools exe alone is a 31 MB image, and which boots
//    have a big enough hole depends on where Windows put the system DLLs that
//    boot.  When the malloc returned NULL the engine gave up on sound entirely,
//    and then crashed (see 2).  Now the call goes to sound_heap_alloc(), which
//    steps down in 32 MB steps until one fits and tells SampleRAM::Init the size
//    it actually got, by rewriting that call's size immediate.
//
// 2. DSClose guard.  Every bail in Engine::Open calls Engine::DSClose before
//    EngineBase::Open has run, so Snd::EngineBase::smStreams is still NULL.
//    DSClose's stream loop derefs it anyway and faults at 0x0088B243 (modtools)
//    in SourceTransport::GetPlayState, and the EngineBase::Close it calls next
//    would fault on the NULL smVoices too.  With smStreams NULL there is nothing
//    either of them could close, so both are skipped.  A failed Open is then a
//    silent game instead of a crash.
//
// Every attempt is logged to BF2GameExt.log with the largest free address
// range at that moment, so a bad boot leaves a record of what happened.
// =============================================================================

void snd_engine_open_fix_install(uintptr_t exe_base);
void snd_engine_open_fix_uninstall();
