#include "pch.h"
#include "particle_batch_spill.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"
#include "core/patch_table.hpp"

#include <detours.h>
#include <cstdio>

// See particle_batch_spill.hpp for what this is, and why it is no longer part
// of gc_visual_limits.cpp.

bool g_particleBatchSpillEnabled = true;

// ---------------------------------------------------------------------------
// Logging.  Install-time output must go through the CRT, not the engine
// logger: dllmain has every exe section at PAGE_READWRITE (non-executable)
// while installers run, so calling game code from here is an EXEC access
// violation on builds with DEP.
// ---------------------------------------------------------------------------
static void install_log(const char* fmt, ...)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// RedParticleRenderer cache pool layout (identical on all three builds)
// ---------------------------------------------------------------------------

static constexpr uint32_t kCacheStride          = 0x3558;
static constexpr uint32_t kVanillaCacheSlots    = 15;
static constexpr uint32_t kCacheParticleCap     = 200;    // entries of 0x44 bytes each
static constexpr uint32_t kCacheParticleIdxOff  = 0x3520;
static constexpr uint32_t kCacheTexHashOff      = 0x3524;
static constexpr uint32_t kCacheBlendOff        = 0x3528;
static constexpr uint32_t kCacheFlagsOff        = 0x352C;
static constexpr uint32_t kCacheNumVertsOff     = 0x3530;
static constexpr uint32_t kCacheNumIndicesOff   = 0x3534;

// A GC beam is one strip of at most 4 submits (types 1,2,2,3) that must stay
// in a single cache, so a spill is only allowed at a strip start (type 0/1)
// and needs this many free entries.  World particles arrive through
// FlushParticleCache as independent type-0 submits, so for them the headroom
// is just unused slack.
static constexpr uint32_t kSpillHeadroom = 4;

// RedParticleRenderer::SubmitParticle -- __cdecl, 8 dword stack args.  Floats
// and pointers are forwarded opaquely.
typedef void (__cdecl* fn_SubmitParticle_t)(
    uint32_t type, uint32_t a2, uint32_t a3, uint32_t a4,
    uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8);

static fn_SubmitParticle_t g_origSubmitParticle = nullptr;

static uint8_t** g_rprCurrentCache = nullptr;
static uint32_t* g_rprCacheIndex   = nullptr;
static uint8_t*  g_rprCaches       = nullptr;  // LIVE array base (DLL 120-slot buffer or exe s_caches[15])
static uint32_t  g_cacheSlots      = kVanillaCacheSlots;

static uint32_t g_cacheSpills     = 0;
static uint32_t g_cacheSpillFails = 0;

// SetCurrentCache's allocation clamp is an imm8 compared with a SIGNED branch
// (modtools 0x00824D3B: CMP EDX,0xF / JL), so a slot count above 127 would
// sign-extend negative, the "is there a free slot" test would never pass, and
// currentCache would be left NULL -- i.e. no particles at all, anywhere.
static_assert(RENDERER_CACHE_SLOTS <= 127,
              "RENDERER_CACHE_SLOTS is written into a signed imm8 clamp; >127 disables all particles");

uint32_t particle_batch_cache_slots()
{
    return g_cacheSlots;
}

void particle_batch_spill_stats(uint32_t& spills, uint32_t& fails)
{
    spills = g_cacheSpills;
    fails  = g_cacheSpillFails;
}

// ---------------------------------------------------------------------------
// The spill itself
// ---------------------------------------------------------------------------

static void spill_current_cache_if_full()
{
    uint8_t* cur = *g_rprCurrentCache;
    if (!cur) return;

    if (*(uint32_t*)(cur + kCacheParticleIdxOff) + kSpillHeadroom <= kCacheParticleCap)
        return;

    const uint32_t texHash = *(uint32_t*)(cur + kCacheTexHashOff);
    const uint32_t blend   = *(uint32_t*)(cur + kCacheBlendOff);
    const uint32_t flags   = *(uint32_t*)(cur + kCacheFlagsOff);

    // Prefer an earlier spill cache with the same key that still has room --
    // SetCurrentCache always finds the FIRST key match, so once that one is
    // full it re-selects it every call and we would otherwise burn a fresh
    // slot per particle.
    uint32_t used = *g_rprCacheIndex;
    if (used > g_cacheSlots) used = g_cacheSlots;

    for (uint32_t i = 0; i < used; ++i) {
        uint8_t* c = g_rprCaches + i * kCacheStride;
        if (c == cur) continue;
        if (*(uint32_t*)(c + kCacheTexHashOff) == texHash &&
            *(uint32_t*)(c + kCacheBlendOff)   == blend   &&
            *(uint32_t*)(c + kCacheFlagsOff)   == flags   &&
            *(uint32_t*)(c + kCacheParticleIdxOff) + kSpillHeadroom <= kCacheParticleCap) {
            *g_rprCurrentCache = c;
            return;
        }
    }

    if (*g_rprCacheIndex >= g_cacheSlots) {
        // Cache pool exhausted -- fall back to the vanilla silent drop.
        g_cacheSpillFails++;
        return;
    }

    uint8_t* fresh = g_rprCaches + (*g_rprCacheIndex) * kCacheStride;
    (*g_rprCacheIndex)++;

    *(uint32_t*)(fresh + kCacheTexHashOff)     = texHash;
    *(uint32_t*)(fresh + kCacheBlendOff)       = blend;
    *(uint32_t*)(fresh + kCacheFlagsOff)       = flags;
    *(uint32_t*)(fresh + kCacheParticleIdxOff) = 0;
    *(uint32_t*)(fresh + kCacheNumVertsOff)    = 0;
    *(uint32_t*)(fresh + kCacheNumIndicesOff)  = 0;

    *g_rprCurrentCache = fresh;
    g_cacheSpills++;
}

static void __cdecl hooked_submit_particle(
    uint32_t type, uint32_t a2, uint32_t a3, uint32_t a4,
    uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8)
{
    // Type 0 = standalone quad, type 1 = strip start (beam); types 2/3 are
    // strip continuations that must stay in the cache their strip started in.
    if (type <= 1)
        spill_current_cache_if_full();

    g_origSubmitParticle(type, a2, a3, a4, a5, a6, a7, a8);
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------

void particle_batch_spill_install(uintptr_t exe_base)
{
    if (!g_particleBatchSpillEnabled) return;
    if (g_build == GameBuild::Unknown) return;
    if (g_addr->rpr_submit_particle == 0 || g_addr->rpr_setcache_base_operand == 0 ||
        g_addr->rpr_setcache_limit_imm8_op == 0 || g_addr->rpr_current_cache == 0 ||
        g_addr->rpr_cache_index == 0 || g_addr->s_caches == 0)
        return;

    // The cache array base is read from the LIVE SetCurrentCache operand: the
    // "Particle Cache Increase" patch set (applied earlier from the patch
    // table, INI-toggleable) redirects it from the exe's s_caches[15] to the
    // DLL's g_sCaches_storage[120].  Reading the operand rather than assuming
    // is what lets this module work with the redirect on or off.
    const uint32_t liveBase = *reinterpret_cast<uint32_t*>(resolve(exe_base, g_addr->rpr_setcache_base_operand));
    const uint32_t exeBase_ = (uint32_t)(uintptr_t)resolve(exe_base, g_addr->s_caches);
    const uint32_t dllBase  = (uint32_t)(uintptr_t)&g_sCaches_storage[0];

    if (liveBase != dllBase && liveBase != exeBase_) {
        install_log("[PARTICLE_SPILL] Unrecognized s_caches base 0x%08x -- spill hook NOT installed", liveBase);
        return;
    }

    g_rprCaches  = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(liveBase));
    g_cacheSlots = (liveBase == dllBase) ? RENDERER_CACHE_SLOTS : kVanillaCacheSlots;

    // With the 120-slot buffer active, also raise SetCurrentCache's allocation
    // clamp (CMP imm8 15) -- the base redirect alone leaves it at 15, so the
    // extra slots would never be handed out.  If the clamp is not where we
    // expect it, stay within the stock 15 rather than spilling into slots the
    // engine will never allocate.
    if (g_cacheSlots > kVanillaCacheSlots) {
        auto imm = reinterpret_cast<uint8_t*>(resolve(exe_base, g_addr->rpr_setcache_limit_imm8_op));
        if (*imm == kVanillaCacheSlots) {
            *imm = (uint8_t)RENDERER_CACHE_SLOTS;
        } else {
            install_log("[PARTICLE_SPILL] SetCurrentCache clamp imm mismatch (0x%02x) -- leaving 15-slot clamp", *imm);
            g_cacheSlots = kVanillaCacheSlots;
        }
    }

    g_rprCurrentCache    = reinterpret_cast<uint8_t**>(resolve(exe_base, g_addr->rpr_current_cache));
    g_rprCacheIndex      = reinterpret_cast<uint32_t*>(resolve(exe_base, g_addr->rpr_cache_index));
    g_origSubmitParticle = reinterpret_cast<fn_SubmitParticle_t>(resolve(exe_base, g_addr->rpr_submit_particle));

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG rs = DetourAttach(reinterpret_cast<PVOID*>(&g_origSubmitParticle), hooked_submit_particle);
    const LONG rc = DetourTransactionCommit();

    if (rs != NO_ERROR || rc != NO_ERROR) {
        install_log("[PARTICLE_SPILL] Detour failed (attach=%ld commit=%ld) -- spill inactive", rs, rc);
        g_origSubmitParticle = nullptr;
        return;
    }

    install_log("Applying patch set: Particle Batch Spill (%u cache slots, %s cache array)",
                g_cacheSlots, (liveBase == dllBase) ? "relocated" : "stock");
}

void particle_batch_spill_uninstall()
{
    install_log("[PARTICLE_SPILL] Final stats: spills=%u spill_fails=%u", g_cacheSpills, g_cacheSpillFails);

    if (!g_origSubmitParticle) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&g_origSubmitParticle), hooked_submit_particle);
    DetourTransactionCommit();
    g_origSubmitParticle = nullptr;
}
