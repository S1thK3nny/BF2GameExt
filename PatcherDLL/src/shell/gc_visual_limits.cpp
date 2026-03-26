#include "pch.h"
#include "gc_visual_limits.hpp"

#include <detours.h>
#include <cstring>
#include <cstdio>

// =============================================================================
// Galactic Conquest visual limit extensions
//
// Raises the per-frame DrawAllBeamBetween and DrawAllParticleAt buffer limits
// from 64/128 to 256/512.
// =============================================================================

static constexpr uintptr_t kUnrelocatedBase = 0x400000u;

static inline void* resolve(uintptr_t exe_base, uintptr_t unrelocated_addr)
{
    return (void*)((unrelocated_addr - kUnrelocatedBase) + exe_base);
}

// ---------------------------------------------------------------------------
// GameLog for diagnostics
// ---------------------------------------------------------------------------
typedef void (__cdecl* GameLog_t)(const char*, ...);
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

// ---------------------------------------------------------------------------
// Struct layout constants
// ---------------------------------------------------------------------------

static constexpr uint32_t kBeamEntrySize     = 0x2C;
static constexpr uint32_t kParticleEntrySize = 0x1C;

static constexpr uint32_t kVanillaBeamLimit     = 64;
static constexpr uint32_t kVanillaParticleLimit = 128;

static constexpr uint32_t kNewBeamLimit     = 256;
static constexpr uint32_t kNewParticleLimit = 512;

static constexpr uint32_t kArrayStart = 0x1C;

static constexpr uint32_t kVanillaBeamCountOff     = kArrayStart + kVanillaBeamLimit     * kBeamEntrySize;
static constexpr uint32_t kVanillaParticleCountOff = kArrayStart + kVanillaParticleLimit * kParticleEntrySize;

static constexpr uint32_t kNewBeamCountOff     = kArrayStart + kNewBeamLimit     * kBeamEntrySize;
static constexpr uint32_t kNewParticleCountOff = kArrayStart + kNewParticleLimit * kParticleEntrySize;

static constexpr uint32_t kNewBeamAllocSize     = kNewBeamCountOff     + 4;
static constexpr uint32_t kNewParticleAllocSize = kNewParticleCountOff + 4;

// ---------------------------------------------------------------------------
// Addresses (unrelocated, BF2_modtools.exe)
// ---------------------------------------------------------------------------

static constexpr uintptr_t kBeamAdd     = 0x0045A920;
static constexpr uintptr_t kParticleAdd = 0x0045A9E0;
static constexpr uintptr_t kPblHashCtor = 0x007E1BD0;
static constexpr uintptr_t kGameLog     = 0x007E3D50;

// Beam count displacement patch points
static constexpr uintptr_t kBeamCountDisps[] = {
    0x0045A922, 0x0045A938,                         // Add
    0x0045ADC8, 0x0045B28D, 0x0045B2A8, 0x0045B2BC, // Render
    0x0045B924,                                      // PostLoadHack
};

// Particle count displacement patch points
static constexpr uintptr_t kParticleCountDisps[] = {
    0x0045A9E2, 0x0045A9FE,                         // Add
    0x0045B629, 0x0045B6C0, 0x0045B6D7, 0x0045B6E9, // Render
    0x0045B8E5,                                      // PostLoadHack
};

static constexpr uintptr_t kParticleAllocSizeOp = 0x0045B8BD;
static constexpr uintptr_t kBeamAllocSizeOp     = 0x0045B8FD;

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

// ---------------------------------------------------------------------------
// Resolved pointers
// ---------------------------------------------------------------------------

static fn_BeamAdd_t     g_origBeamAdd     = nullptr;
static fn_ParticleAdd_t g_origParticleAdd = nullptr;
static fn_PblHash_t     g_pblHash         = nullptr;

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

// ---------------------------------------------------------------------------
// Hooked Add functions
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
            log("[GC_VIS] STATS: beams=%u/%u particles=%u/%u (vanilla limits: %u/%u) dropped: b=%u p=%u",
                g_beamHighWater, kNewBeamLimit, g_particleHighWater, kNewParticleLimit,
                kVanillaBeamLimit, kVanillaParticleLimit, g_beamDropped, g_particleDropped);
            g_loggedOnce = true;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Runtime patching helpers
// ---------------------------------------------------------------------------

static int g_patchOk = 0;
static int g_patchFail = 0;

static void patch_u32(uintptr_t exe_base, uintptr_t unrelocated_addr, uint32_t expected, uint32_t replacement)
{
    auto ptr = reinterpret_cast<uint32_t*>(resolve(exe_base, unrelocated_addr));
    if (*ptr == expected) {
        *ptr = replacement;
        g_patchOk++;
    } else {
        log("[GC_VIS] PATCH FAIL at 0x%08x: expected 0x%08x, found 0x%08x",
            unrelocated_addr, expected, *ptr);
        g_patchFail++;
    }
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void gc_visual_limits_install(uintptr_t exe_base)
{
    g_log = reinterpret_cast<GameLog_t>(resolve(exe_base, kGameLog));
    g_pblHash = reinterpret_cast<fn_PblHash_t>(resolve(exe_base, kPblHashCtor));

    // Append to the install log alongside the other patch sets
    FILE* f = nullptr;
    if (fopen_s(&f, "BF2GameExt.log", "a") == 0 && f) {
        fprintf(f, "Applying patch set: GC Visual Limit Extensions\n");
        fclose(f);
    }

    // --- Patch all beam count displacement values ---
    for (uintptr_t addr : kBeamCountDisps)
        patch_u32(exe_base, addr, kVanillaBeamCountOff, kNewBeamCountOff);

    // --- Patch all particle count displacement values ---
    for (uintptr_t addr : kParticleCountDisps)
        patch_u32(exe_base, addr, kVanillaParticleCountOff, kNewParticleCountOff);

    // --- Patch allocation sizes in PostLoadHack ---
    patch_u32(exe_base, kBeamAllocSizeOp,     0x00000B20, kNewBeamAllocSize);
    patch_u32(exe_base, kParticleAllocSizeOp, 0x00000E20, kNewParticleAllocSize);

    log("[GC_VIS] Patches applied: %d ok, %d failed", g_patchOk, g_patchFail);

    // --- Detour the Add functions ---
    g_origBeamAdd     = reinterpret_cast<fn_BeamAdd_t>(resolve(exe_base, kBeamAdd));
    g_origParticleAdd = reinterpret_cast<fn_ParticleAdd_t>(resolve(exe_base, kParticleAdd));

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG r1 = DetourAttach(reinterpret_cast<PVOID*>(&g_origBeamAdd),     hooked_beam_add);
    LONG r2 = DetourAttach(reinterpret_cast<PVOID*>(&g_origParticleAdd), hooked_particle_add);
    LONG rc = DetourTransactionCommit();

    log("[GC_VIS] Detours: beam=%ld particle=%ld commit=%ld", r1, r2, rc);
}

void gc_visual_limits_uninstall()
{
    // Log final stats
    if (g_log) {
        log("[GC_VIS] Final stats: beam_add=%u (dropped=%u) particle_add=%u (dropped=%u)",
            g_beamAddCalls, g_beamDropped, g_particleAddCalls, g_particleDropped);
    }

    if (g_origBeamAdd) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(reinterpret_cast<PVOID*>(&g_origBeamAdd),     hooked_beam_add);
        DetourDetach(reinterpret_cast<PVOID*>(&g_origParticleAdd), hooked_particle_add);
        DetourTransactionCommit();
    }
}
