#pragma once

#include <stdint.h>

// =============================================================================
// Particle density — one dial over the three things that decide how many
// particles a world actually gets to show.
//
//   0  stock      the engine's own numbers, nothing touched
//   1  balanced   full density near and mid-range, stock thinning far out
//   2  maximum    no distance thinning at all
//
// Stock BF2 thins effects with distance in a way that is invisible in the
// config and very visible on screen: each particle belongs to a bucket
// (index & 3) and ParticleSystem::sLodMask decides, per LOD level, which
// buckets are allowed to spawn.  LOD1 drops one bucket in four, LOD2 two, LOD3
// three — so a distant emitter creates a quarter of its particles.  Alongside
// it, sLodFadeMask marks the bucket the NEXT LOD will cull so it can fade out
// first; the two tables have to stay consistent or particles spawn ghost-faded
// and then pop to full alpha at each boundary.  Rather than hand-maintain that
// pairing, both tables are generated here from one rule:
//
//     fade[L][b] = mask[L][b] && !mask[L+1][b]
//
// which is exactly what the stock data satisfies.
//
// Level 1 is the interesting one.  Instead of unmasking everything and paying
// for particles too far away to resolve, it softens the mask by one step AND
// pushes the LOD curve out so the thinning starts at twice the distance.  The
// far field — where the cost concentrates, because that is where the emitter
// count is highest — keeps decimating.
//
// The LOD curve is
//
//     lodF = (4.0 / (ref - 20.0)) * (dist - 20.0)      floored, capped at 3
//
// and that 4.0 lives in the shared literal pool (~90 xrefs across the engine:
// turn speeds, sound distances, RGBtoHSV).  Editing it would corrupt half the
// game, so instead the loading instruction's disp32 operand is repointed at a
// float this DLL owns — one call site changed, the shared constant untouched.
//
// Also folded in: ParticleEmitter::mMaxParticles, the per-emitter LIFETIME
// budget, clamped to 128 as it is read from the .fx.  Effects asking for more
// are silently cut short.  Raising it is safe — the spawn loop bounds its
// cursor against the end of sParticleCache every iteration, independently —
// but it is off at level 0 because existing maps were authored against 128.
//
// Note for content authors: MaxParticles = -1 in a .fx already means
// "unlimited" to the engine and skips the budget test entirely, with no patch
// needed at all.
// =============================================================================

// 0 = stock, 1 = balanced, 2 = maximum.  Read from [Particles] ParticleDensity.
extern int g_particleDensity;

void particle_density_install(uintptr_t exe_base);
void particle_density_uninstall();
