#pragma once

#include <stdint.h>

// Hook RedParticleRenderer::SubmitParticle to spread overflow particles across
// multiple renderer cache entries when a single entry fills up (200 particles).
// Must be called while all executable sections are writable.
bool patch_particle_renderer(uintptr_t exe_base);

// Detach Detours hooks. Call from BF2GameExt_Shutdown().
void unpatch_particle_renderer();
