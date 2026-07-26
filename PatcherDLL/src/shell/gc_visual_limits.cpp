#include "pch.h"
#include "gc_visual_limits.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"
#include "core/patch_table.hpp"

#include <detours.h>
#include <cstring>
#include <cstdio>

// =============================================================================
// Galactic Conquest visual limit extensions — see gc_visual_limits.hpp for the
// full story (buffer enlargement + RedParticleRenderer cache spill).
// =============================================================================

bool g_gcVisualLimitsEnabled = true;

// ---------------------------------------------------------------------------
// Logging.  Two channels with a hard rule:
//
//   install_log() — appends to BF2GameExt.log via the CRT.  The ONLY logger
//     that may run at install time: dllmain has all exe sections at
//     PAGE_READWRITE (non-executable) then, so calling any game code —
//     including the engine logger — is an EXEC access violation on builds
//     with DEP (crashes Steam; modtools merely tolerates it).
//
//   log() — the engine's GameLog, for runtime-only diagnostics (hooks/stats).
// ---------------------------------------------------------------------------
static GameLog_t g_log = nullptr;

static void log(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log("%s", buf);
}

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
// GC draw-buffer layout constants
// ---------------------------------------------------------------------------

static constexpr uint32_t kBeamEntrySize     = 0x2C;
static constexpr uint32_t kParticleEntrySize = 0x1C;

static constexpr uint32_t kVanillaBeamLimit     = 64;
static constexpr uint32_t kVanillaParticleLimit = 128;

static constexpr uint32_t kNewBeamLimit      = 256;  // modtools (Add is detoured)
static constexpr uint32_t kSteamBeamLimit    = 255;  // Steam limit is a CMP imm8 — 0xFF max
static constexpr uint32_t kNewParticleLimit  = 512;  // both builds

static constexpr uint32_t kArrayStart = 0x1C;

static constexpr uint32_t kVanillaBeamCountOff     = kArrayStart + kVanillaBeamLimit     * kBeamEntrySize;     // 0xB1C
static constexpr uint32_t kVanillaParticleCountOff = kArrayStart + kVanillaParticleLimit * kParticleEntrySize; // 0xE1C

static constexpr uint32_t kNewBeamCountOff     = kArrayStart + kNewBeamLimit     * kBeamEntrySize;     // 0x2C1C
static constexpr uint32_t kSteamBeamCountOff   = kArrayStart + kSteamBeamLimit   * kBeamEntrySize;     // 0x2BF0
static constexpr uint32_t kNewParticleCountOff = kArrayStart + kNewParticleLimit * kParticleEntrySize; // 0x381C

static constexpr uint32_t kNewBeamAllocSize     = kNewBeamCountOff     + 4;
static constexpr uint32_t kSteamBeamAllocSize   = kSteamBeamCountOff   + 4;
static constexpr uint32_t kNewParticleAllocSize = kNewParticleCountOff + 4;

// Active beam limit for the running build (set at install time).
static uint32_t g_beamLimit = kNewBeamLimit;

// ---------------------------------------------------------------------------
// RedParticleRenderer cache pool layout (identical in modtools and Steam)
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
// and needs this many free entries.
static constexpr uint32_t kSpillHeadroom = 4;

// ---------------------------------------------------------------------------
// Function pointer types
// ---------------------------------------------------------------------------

typedef bool (__thiscall* fn_BeamAdd_t)(
    void* ecx, const float* pos1, const float* pos2, const char* texName,
    float size, uint32_t color, float shift, float repetitions);

typedef bool (__thiscall* fn_ParticleAdd_t)(
    void* ecx, const float* pos, const char* texName,
    float size, uint32_t color, float rotation);

typedef uint32_t* (__fastcall* fn_PblHash_t)(uint32_t* out, void* edx, const char* str);

// RedParticleRenderer::SubmitParticle — __cdecl, 8 dword stack args.  Floats
// and pointers are forwarded opaquely.
typedef void (__cdecl* fn_SubmitParticle_t)(
    uint32_t type, uint32_t a2, uint32_t a3, uint32_t a4,
    uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8);

// ---------------------------------------------------------------------------
// Resolved pointers
// ---------------------------------------------------------------------------

static fn_BeamAdd_t        g_origBeamAdd        = nullptr;  // modtools only
static fn_ParticleAdd_t    g_origParticleAdd    = nullptr;  // modtools only
static fn_PblHash_t        g_pblHash            = nullptr;  // modtools only
static fn_SubmitParticle_t g_origSubmitParticle = nullptr;  // both builds

static uint8_t** g_rprCurrentCache = nullptr;
static uint32_t* g_rprCacheIndex   = nullptr;
static uint8_t*  g_rprCaches       = nullptr;  // LIVE array base (DLL 120-slot buffer or exe s_caches[15])
static uint32_t  g_cacheSlots      = kVanillaCacheSlots;

// ---------------------------------------------------------------------------
// Debug stats — per-frame high-water marks + periodic logging
// ---------------------------------------------------------------------------
static uint32_t g_beamAddCalls      = 0;
static uint32_t g_beamDropped       = 0;
static uint32_t g_particleAddCalls  = 0;
static uint32_t g_particleDropped   = 0;
static uint32_t g_beamHighWater     = 0;
static uint32_t g_particleHighWater = 0;
static uint32_t g_frameCount        = 0;
static bool     g_loggedOnce        = false;
static uint32_t g_cacheSpills       = 0;
static uint32_t g_cacheSpillFails   = 0;

// ---------------------------------------------------------------------------
// RedParticleRenderer cache spill (both builds) — the actual beam fix
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

    // Prefer an earlier spill cache with the same key that still has room —
    // SetCurrentCache always finds the FIRST key match, so once that one is
    // full it re-selects it every call and we'd otherwise burn a fresh slot
    // per particle.
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
        // Cache pool exhausted — fall back to the vanilla silent drop.
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
// Hooked Add functions (modtools only — Steam Adds are byte-patched instead)
// ---------------------------------------------------------------------------

static bool __fastcall hooked_beam_add(
    void* ecx, void* /*edx*/, const float* pos1, const float* pos2, const char* texName,
    float size, uint32_t color, float shift, float repetitions)
{
    auto base = reinterpret_cast<uint8_t*>(ecx);
    auto count = reinterpret_cast<uint32_t*>(base + kNewBeamCountOff);

    g_beamAddCalls++;

    if (*count >= kNewBeamLimit) {
        g_beamDropped++;
        return false;
    }

    uint32_t idx = *count;
    *count = idx + 1;

    // Track per-frame high-water mark
    if (idx + 1 > g_beamHighWater)
        g_beamHighWater = idx + 1;

    auto entry = reinterpret_cast<uint32_t*>(base + kArrayStart + idx * kBeamEntrySize);

    memcpy(entry, pos1, 12);
    memcpy(entry + 3, pos2, 12);

    uint32_t hash;
    g_pblHash(&hash, nullptr, texName);
    entry[6] = hash;

    memcpy(entry + 7, &size, 4);
    entry[8] = color;
    memcpy(entry + 9, &shift, 4);
    memcpy(entry + 10, &repetitions, 4);

    return true;
}

static bool __fastcall hooked_particle_add(
    void* ecx, void* /*edx*/, const float* pos, const char* texName,
    float size, uint32_t color, float rotation)
{
    auto base = reinterpret_cast<uint8_t*>(ecx);
    auto count = reinterpret_cast<uint32_t*>(base + kNewParticleCountOff);

    g_particleAddCalls++;

    if (*count >= kNewParticleLimit) {
        g_particleDropped++;
        return false;
    }

    uint32_t idx = *count;
    *count = idx + 1;

    // Track per-frame high-water mark
    if (idx + 1 > g_particleHighWater)
        g_particleHighWater = idx + 1;

    auto entry = reinterpret_cast<uint32_t*>(base + (idx + 1) * kParticleEntrySize);

    memcpy(entry, pos, 12);

    uint32_t hash;
    g_pblHash(&hash, nullptr, texName);
    entry[3] = hash;

    memcpy(entry + 4, &size, 4);
    entry[5] = color;
    memcpy(entry + 6, &rotation, 4);

    // Log high-water marks periodically (every ~120 frames)
    if (idx == 0) {
        g_frameCount++;
        if (!g_loggedOnce || (g_frameCount % 120 == 0)) {
            log("[GC_VIS] STATS: beams=%u/%u particles=%u/%u (vanilla limits: %u/%u) dropped: b=%u p=%u spills=%u spill_fails=%u",
                g_beamHighWater, g_beamLimit, g_particleHighWater, kNewParticleLimit,
                kVanillaBeamLimit, kVanillaParticleLimit, g_beamDropped, g_particleDropped,
                g_cacheSpills, g_cacheSpillFails);
            g_loggedOnce = true;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Runtime patching helpers — the count offsets, limits and alloc sizes only
// make sense together, so the whole set is verified before anything is
// written (a half-applied set would corrupt memory).
// ---------------------------------------------------------------------------

struct gc_patch {
    uintptr_t addr;        // unrelocated operand address
    uint32_t  expected;
    uint32_t  replacement;
    bool      is_u8;
};

static bool verify_and_apply(uintptr_t exe_base, const gc_patch* list, int count)
{
    for (int i = 0; i < count; ++i) {
        const gc_patch& p = list[i];
        const uint32_t found = p.is_u8 ? *reinterpret_cast<uint8_t*>(resolve(exe_base, p.addr))
                                       : *reinterpret_cast<uint32_t*>(resolve(exe_base, p.addr));
        if (found != p.expected) {
            install_log("[GC_VIS] PATCH VERIFY FAIL at 0x%08x: expected 0x%08x, found 0x%08x — skipping patch set",
                        p.addr, p.expected, found);
            return false;
        }
    }

    for (int i = 0; i < count; ++i) {
        const gc_patch& p = list[i];
        if (p.is_u8)
            *reinterpret_cast<uint8_t*>(resolve(exe_base, p.addr)) = (uint8_t)p.replacement;
        else
            *reinterpret_cast<uint32_t*>(resolve(exe_base, p.addr)) = p.replacement;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void gc_visual_limits_install(uintptr_t exe_base)
{
    if (!g_gcVisualLimitsEnabled) return;
    if (g_build == GameBuild::Unknown) return;

    g_log = get_gamelog(); // runtime-only — must not be called during install

    // Append to the install log alongside the other patch sets
    install_log("Applying patch set: GC Visual Limit Extensions");

    // --- Build the byte-patch set for the active build ---
    gc_patch patches[20];
    int n = 0;

    if (g_build == GameBuild::Modtools) {
        using namespace game_addrs::modtools;
        g_beamLimit = kNewBeamLimit;

        for (uintptr_t addr : gc_beam_count_patches)
            patches[n++] = { addr, kVanillaBeamCountOff, kNewBeamCountOff, false };
        for (uintptr_t addr : gc_particle_count_patches)
            patches[n++] = { addr, kVanillaParticleCountOff, kNewParticleCountOff, false };
        patches[n++] = { gc_beam_alloc_size_op,     0x00000B20, kNewBeamAllocSize,     false };
        patches[n++] = { gc_particle_alloc_size_op, 0x00000E20, kNewParticleAllocSize, false };
    } else if (g_build == GameBuild::Steam) {
        using namespace game_addrs::steam;
        g_beamLimit = kSteamBeamLimit;

        // Pure byte patches: the LTCG Add functions keep their code, just
        // with bigger limit immediates and count offsets.
        for (uintptr_t addr : gc_beam_count_patches)
            patches[n++] = { addr, kVanillaBeamCountOff, kSteamBeamCountOff, false };
        for (uintptr_t addr : gc_particle_count_patches)
            patches[n++] = { addr, kVanillaParticleCountOff, kNewParticleCountOff, false };
        patches[n++] = { gc_beam_limit_imm8_op,      0x40,       kSteamBeamLimit,       true  };
        patches[n++] = { gc_particle_limit_imm32_op, 0x00000080, kNewParticleLimit,     false };
        patches[n++] = { gc_beam_alloc_size_op,      0x00000B20, kSteamBeamAllocSize,   false };
        patches[n++] = { gc_particle_alloc_size_op,  0x00000E20, kNewParticleAllocSize, false };
    } else {
        // GOG: identical code shape to Steam (same imm8 beam CMP, same vanilla
        // 0xB1C/0xE1C count offsets and 0xB20/0xE20 allocation sizes), only the
        // addresses move.  verify_and_apply() still checks every site.
        using namespace game_addrs::gog;
        g_beamLimit = kSteamBeamLimit;

        for (uintptr_t addr : gc_beam_count_patches)
            patches[n++] = { addr, kVanillaBeamCountOff, kSteamBeamCountOff, false };
        for (uintptr_t addr : gc_particle_count_patches)
            patches[n++] = { addr, kVanillaParticleCountOff, kNewParticleCountOff, false };
        patches[n++] = { gc_beam_limit_imm8_op,      0x40,       kSteamBeamLimit,       true  };
        patches[n++] = { gc_particle_limit_imm32_op, 0x00000080, kNewParticleLimit,     false };
        patches[n++] = { gc_beam_alloc_size_op,      0x00000B20, kSteamBeamAllocSize,   false };
        patches[n++] = { gc_particle_alloc_size_op,  0x00000E20, kNewParticleAllocSize, false };
    }

    // Verify every site before writing anything — a half-applied set (count
    // offset moved but allocation still small, or vice versa) corrupts memory.
    if (!verify_and_apply(exe_base, patches, n))
        return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    LONG rb = 0, rp = 0;
    if (g_build == GameBuild::Modtools) {
        using namespace game_addrs::modtools;

        // Detour the Add functions (raise the entry limits).
        g_pblHash         = reinterpret_cast<fn_PblHash_t>(resolve(exe_base, hash_string_thiscall));
        g_origBeamAdd     = reinterpret_cast<fn_BeamAdd_t>(resolve(exe_base, gc_beam_add));
        g_origParticleAdd = reinterpret_cast<fn_ParticleAdd_t>(resolve(exe_base, gc_particle_add));
        rb = DetourAttach(reinterpret_cast<PVOID*>(&g_origBeamAdd),     hooked_beam_add);
        rp = DetourAttach(reinterpret_cast<PVOID*>(&g_origParticleAdd), hooked_particle_add);
    }

    // Detour SubmitParticle for the cache spill (both builds).  The cache
    // array base must be read from the LIVE SetCurrentCache operand: the
    // "Particle Cache Increase" patch set (applied earlier from the patch
    // table, INI-toggleable) redirects it from the exe's s_caches[15] to the
    // DLL's g_sCaches_storage[120].
    const uint32_t liveBase = *reinterpret_cast<uint32_t*>(resolve(exe_base, g_addr->rpr_setcache_base_operand));
    const uint32_t exeBase_ = (uint32_t)(uintptr_t)resolve(exe_base, g_addr->s_caches);
    const uint32_t dllBase  = (uint32_t)(uintptr_t)&g_sCaches_storage[0];

    LONG rs = -1, rc;
    if (liveBase == dllBase || liveBase == exeBase_) {
        g_rprCaches = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(liveBase));
        g_cacheSlots = (liveBase == dllBase) ? RENDERER_CACHE_SLOTS : kVanillaCacheSlots;

        // With the 120-slot buffer active, also raise SetCurrentCache's
        // allocation clamp (CMP imm8 15) — the base redirect alone leaves it
        // at 15, which would let our spills starve other textures of slots.
        if (g_cacheSlots > kVanillaCacheSlots) {
            auto imm = reinterpret_cast<uint8_t*>(resolve(exe_base, g_addr->rpr_setcache_limit_imm8_op));
            if (*imm == kVanillaCacheSlots)
                *imm = (uint8_t)RENDERER_CACHE_SLOTS;
            else
                install_log("[GC_VIS] SetCurrentCache clamp imm mismatch (0x%02x) — leaving 15-slot clamp", *imm);
        }

        g_rprCurrentCache = reinterpret_cast<uint8_t**>(resolve(exe_base, g_addr->rpr_current_cache));
        g_rprCacheIndex   = reinterpret_cast<uint32_t*>(resolve(exe_base, g_addr->rpr_cache_index));
        g_origSubmitParticle = reinterpret_cast<fn_SubmitParticle_t>(resolve(exe_base, g_addr->rpr_submit_particle));
        rs = DetourAttach(reinterpret_cast<PVOID*>(&g_origSubmitParticle), hooked_submit_particle);
    } else {
        install_log("[GC_VIS] Unrecognized s_caches base 0x%08x — cache spill hook NOT installed", liveBase);
        g_origSubmitParticle = nullptr;
    }

    rc = DetourTransactionCommit();

    install_log("[GC_VIS] %d patches applied (beam limit %u, particle limit %u, cache slots %u)",
                n, g_beamLimit, kNewParticleLimit, g_cacheSlots);
    install_log("[GC_VIS] Detours: beam=%ld particle=%ld submit_particle=%ld commit=%ld", rb, rp, rs, rc);
}

void gc_visual_limits_uninstall()
{
    // Final stats — CRT file logger only: at DLL detach the engine (and its
    // logger) may already be torn down.
    install_log("[GC_VIS] Final stats: beam_add=%u (dropped=%u) particle_add=%u (dropped=%u) spills=%u spill_fails=%u",
                g_beamAddCalls, g_beamDropped, g_particleAddCalls, g_particleDropped,
                g_cacheSpills, g_cacheSpillFails);

    if (!g_origBeamAdd && !g_origSubmitParticle) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (g_origBeamAdd) {
        DetourDetach(reinterpret_cast<PVOID*>(&g_origBeamAdd),     hooked_beam_add);
        DetourDetach(reinterpret_cast<PVOID*>(&g_origParticleAdd), hooked_particle_add);
    }
    if (g_origSubmitParticle)
        DetourDetach(reinterpret_cast<PVOID*>(&g_origSubmitParticle), hooked_submit_particle);
    DetourTransactionCommit();
}
