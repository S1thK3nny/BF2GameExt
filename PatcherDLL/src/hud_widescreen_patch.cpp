#include "pch.h"

#include "hud_widescreen_patch.hpp"
#include "lua_hooks.hpp"
#include <detours.h>

// ============================================================================
// HUD Widescreen Reticle Fix
//
// The vanilla letterbox transform in HUD::Manager::Update applies a Y-scale
// and Y-offset to ALL HUD elements on widescreen displays. This misaligns
// the reticle with the 3D aim point — error grows with distance from screen
// center (proportional to TiltValue camera parameter).
//
// Fix: pre-distort ONLY the reticle Y position in ReticuleDisplay::Update so
// that after the letterbox transform, it lands on the correct spot. All other
// HUD elements (minimap, ammo counter, etc.) are completely untouched.
//
// Correction formula:
//   corrected_ndc_y = (ndc_y - y_offset/480) / y_scale
//
// Modtools (x87): compiler emits (projY + 1.0) * 0.5 — we redirect the
//   float constant addresses in FADD/FMUL to DLL-controlled statics.
// Retail (SSE): compiler emits projY * 0.5 + 0.5 — we replace the
//   MULSS+ADDSS with a CALL to a naked correction function.
// ============================================================================

// ---------------------------------------------------------------------------
// Correction parameters — updated every frame from HUD::Manager::Update hook
// ---------------------------------------------------------------------------

// Retail SSE correction function uses these:
static float s_yScale      = 1.0f;  // letterbox Y scale (1.0 = no letterbox)
static float s_yOffsetNorm  = 0.0f;  // y_offset / screenH

// Modtools x87 constant redirect (replace 1.0 and 0.5 in the FADD/FMUL):
//   (projY + s_addConst) * s_mulConst = corrected NDC
static float s_addConst = 1.0f;  // default: original 1.0
static float s_mulConst = 0.5f;  // default: original 0.5

// Correction strength: -1 = auto (use 1-yscale, scales with aspect ratio),
// 0.0 = no correction, 1.0 = full letterbox undo.
// INI: [Patches] ReticleCorrection=-1
static float s_correctionStrength = -1.0f;

// ---------------------------------------------------------------------------
// Per-build addresses
// ---------------------------------------------------------------------------
struct HudWsAddrs {
    uintptr_t id_rva;
    uint64_t  id_expected;

    uintptr_t hud_manager_update;
    uintptr_t screen_width;
    uintptr_t screen_height;

    // Modtools: 4-byte address operands in FADD [1.0] / FMUL [0.5]
    uintptr_t fadd_addr_operand;
    uintptr_t fmul_addr_operand;

    // Retail: start of 24-byte patch region (MULSS...MOV...MOV...ADDSS)
    uintptr_t mulss_patch_start;

    bool is_modtools;
};

static const HudWsAddrs s_addrs_modtools = {
    .id_rva             = 0x62b59c,
    .id_expected        = 0x746163696c707041,
    .hud_manager_update = 0x006b7300,
    .screen_width       = 0x00e5b508,
    .screen_height      = 0x00e5b50c,
    .fadd_addr_operand  = 0x006834D9,  // FADD [0x00a2a290] → operand at +2
    .fmul_addr_operand  = 0x006834E5,  // FMUL [0x00a2a0cc] → operand at +2
    .mulss_patch_start  = 0,
    .is_modtools        = true,
};

static const HudWsAddrs s_addrs_steam = {
    .id_rva             = 0x39f834,
    .id_expected        = 0x746163696c707041,
    .hud_manager_update = 0x00565060,
    .screen_width       = 0x0093e4a4,
    .screen_height      = 0x0093e4a8,
    .fadd_addr_operand  = 0,
    .fmul_addr_operand  = 0,
    .mulss_patch_start  = 0x006308f6,
    .is_modtools        = false,
};

static const HudWsAddrs s_addrs_gog = {
    .id_rva             = 0x3a0698,
    .id_expected        = 0x746163696c707041,
    .hud_manager_update = 0x00565de0,
    .screen_width       = 0x0093f944,
    .screen_height      = 0x0093f948,
    .fadd_addr_operand  = 0,
    .fmul_addr_operand  = 0,
    .mulss_patch_start  = 0x00631996,
    .is_modtools        = false,
};

static const HudWsAddrs* s_addrs = nullptr;
static uintptr_t s_baseOffset = 0;

// Saved bytes for uninstall
static uint8_t s_savedPatchBytes[24];
static uintptr_t s_patchLoc = 0;

static uint32_t s_savedFaddAddr = 0;
static uintptr_t s_faddPatchLoc = 0;
static uint32_t s_savedFmulAddr = 0;
static uintptr_t s_fmulPatchLoc = 0;

// ---------------------------------------------------------------------------
// Retail SSE correction function (naked — called from patched code)
//
// Entry: XMM0 = projY, XMM1 = 0.5
// Exit:  XMM0 = corrected NDC Y,  XMM1 preserved
// ---------------------------------------------------------------------------
__declspec(naked) static void __cdecl reticle_y_correction()
{
    __asm {
        mulss xmm0, xmm1                       // projY * 0.5
        addss xmm0, xmm1                       // + 0.5 → ndc_y
        subss xmm0, dword ptr [s_yOffsetNorm]  // - (1-y_scale)/2
        divss xmm0, dword ptr [s_yScale]       // / y_scale
        ret
    }
}

// ---------------------------------------------------------------------------
// HUD::Manager::Update hook — runs original then updates correction params
// ---------------------------------------------------------------------------
using fn_HudUpdate_fc = void(__fastcall*)(void*);
using fn_HudUpdate_cd = void(__cdecl*)(int);
static fn_HudUpdate_fc s_origUpdate_fc = nullptr;
static fn_HudUpdate_cd s_origUpdate_cd = nullptr;

static int s_logOnce = 0;

static void update_correction_params()
{
    auto w = *(const uint32_t*)(s_addrs->screen_width + s_baseOffset);
    auto h = *(const uint32_t*)(s_addrs->screen_height + s_baseOffset);
    if (w == 0 || h == 0) return;

    float fw = (float)w;
    float fh = (float)h;
    float delta = fw * 0.75f - fh;

    if (delta >= 2.0f) {
        // Widescreen — apply correction scaled by strength.
        // Auto mode (strength<0): use (1-yscale) which = 1/3 at 16:9.
        // Empirically verified: 0.333 is correct at 16:9, matches 1-yscale.
        float yscale = 1.0f - delta / fh;
        float str    = s_correctionStrength < 0.0f
                     ? (1.0f - yscale)       // auto: scales with aspect ratio
                     : s_correctionStrength;  // manual override from INI
        float yscale_eff = 1.0f - str * (1.0f - yscale);
        float yoff_eff   = str * (1.0f - yscale) * 0.5f;

        // Retail SSE params: corrected = (ndc_y - yoff_eff) / yscale_eff
        s_yScale      = yscale_eff;
        s_yOffsetNorm = yoff_eff;

        // Modtools x87 params: corrected = (projY + A) * B
        //   B = 0.5 / yscale_eff
        //   A = 1.0 - 2*yoff_eff
        s_mulConst    = 0.5f / yscale_eff;
        s_addConst    = 1.0f - 2.0f * yoff_eff;
    } else {
        // 4:3 or taller — identity (no correction)
        s_yScale      = 1.0f;
        s_yOffsetNorm = 0.0f;
        s_mulConst    = 0.5f;
        s_addConst    = 1.0f;
    }

    if (s_logOnce < 3) {
        s_logOnce++;
        dbg_log("HUD WS: screen=%ux%u delta=%.1f str=%.2f yscale=%.4f yoff=%.4f mulC=%.4f addC=%.4f\n",
                w, h, delta, s_correctionStrength, s_yScale, s_yOffsetNorm, s_mulConst, s_addConst);
    }
}

static void __fastcall hooked_HudUpdate_fc(void* self)
{
    s_origUpdate_fc(self);
    update_correction_params();

    // Log actual matrix values for debugging (first few frames only)
    if (s_logOnce <= 3) {
        float mat_yscale = *(float*)((uintptr_t)self + 0x44);
        float mat_yoff   = *(float*)((uintptr_t)self + 0x64);
        dbg_log("HUD WS matrix: yscale=%.4f yoff=%.1f (this=%08X)\n",
                mat_yscale, mat_yoff, (uint32_t)self);
    }
}

static void __cdecl hooked_HudUpdate_cd(int ptr)
{
    s_origUpdate_cd(ptr);
    update_correction_params();

    if (s_logOnce <= 3) {
        float mat_yscale = *(float*)((uintptr_t)ptr + 0x44);
        float mat_yoff   = *(float*)((uintptr_t)ptr + 0x64);
        dbg_log("HUD WS matrix: yscale=%.4f yoff=%.1f (this=%08X)\n",
                mat_yscale, mat_yoff, (uint32_t)ptr);
    }
}

// ---------------------------------------------------------------------------
// Build identification
// ---------------------------------------------------------------------------
static const HudWsAddrs* identify_build(uintptr_t exe_base)
{
    auto check = [&](const HudWsAddrs& a) -> bool {
        return *(const uint64_t*)(exe_base + a.id_rva) == a.id_expected;
    };
    if (check(s_addrs_modtools)) return &s_addrs_modtools;
    if (check(s_addrs_steam))    return &s_addrs_steam;
    if (check(s_addrs_gog))      return &s_addrs_gog;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
void patch_hud_widescreen(uintptr_t exe_base, float correction_strength)
{
    s_addrs = identify_build(exe_base);
    if (!s_addrs) return;

    s_correctionStrength = correction_strength;
    s_baseOffset = exe_base - 0x400000;

    if (s_addrs->is_modtools) {
        // --- Modtools (x87): redirect FADD/FMUL constant addresses ---
        // FADD [1.0] → FADD [s_addConst]
        s_faddPatchLoc = s_addrs->fadd_addr_operand + s_baseOffset;
        memcpy(&s_savedFaddAddr, (void*)s_faddPatchLoc, 4);
        uint32_t newAddr = (uint32_t)&s_addConst;
        DWORD oldProt;
        VirtualProtect((void*)s_faddPatchLoc, 4, PAGE_READWRITE, &oldProt);
        memcpy((void*)s_faddPatchLoc, &newAddr, 4);
        VirtualProtect((void*)s_faddPatchLoc, 4, oldProt, &oldProt);

        // FMUL [0.5] → FMUL [s_mulConst]
        s_fmulPatchLoc = s_addrs->fmul_addr_operand + s_baseOffset;
        memcpy(&s_savedFmulAddr, (void*)s_fmulPatchLoc, 4);
        newAddr = (uint32_t)&s_mulConst;
        VirtualProtect((void*)s_fmulPatchLoc, 4, PAGE_READWRITE, &oldProt);
        memcpy((void*)s_fmulPatchLoc, &newAddr, 4);
        VirtualProtect((void*)s_fmulPatchLoc, 4, oldProt, &oldProt);

        dbg_log("HUD widescreen: modtools FADD at %08X, FMUL at %08X redirected\n",
                (uint32_t)s_faddPatchLoc, (uint32_t)s_fmulPatchLoc);
    } else {
        // --- Retail (SSE): patch 24-byte MULSS...ADDSS region with CALL ---
        //
        // Original 24 bytes:
        //   [0..3]   MULSS XMM0, XMM1           (F3 0F 59 C1)
        //   [4..11]  MOV [ESP+0x1c], 0x3f000000  (C7 44 24 1C ...)
        //   [12..19] MOV [ESP+0x24], 0x0         (C7 44 24 24 ...)
        //   [20..23] ADDSS XMM0, XMM1            (F3 0F 58 C1)
        //
        // Patched:
        //   [0..4]   CALL reticle_y_correction   (E8 rel32)
        //   [5..12]  MOV [ESP+0x1c], 0x3f000000  (preserved)
        //   [13..20] MOV [ESP+0x24], 0x0         (preserved)
        //   [21..23] NOP NOP NOP

        s_patchLoc = s_addrs->mulss_patch_start + s_baseOffset;
        memcpy(s_savedPatchBytes, (void*)s_patchLoc, 24);

        uint8_t patch[24];

        // CALL rel32
        patch[0] = 0xE8;
        uint32_t rel32 = (uint32_t)&reticle_y_correction - (uint32_t)(s_patchLoc + 5);
        memcpy(&patch[1], &rel32, 4);

        // Preserve the two MOV instructions from original bytes [4..19]
        memcpy(&patch[5], &s_savedPatchBytes[4], 16);

        // NOP padding
        patch[21] = 0x90;
        patch[22] = 0x90;
        patch[23] = 0x90;

        DWORD oldProt;
        VirtualProtect((void*)s_patchLoc, 24, PAGE_READWRITE, &oldProt);
        memcpy((void*)s_patchLoc, patch, 24);
        VirtualProtect((void*)s_patchLoc, 24, oldProt, &oldProt);

        dbg_log("HUD widescreen: retail patch at %08X, CALL target %08X, rel32=%08X\n",
                (uint32_t)s_patchLoc, (uint32_t)&reticle_y_correction, rel32);
        dbg_log("HUD widescreen: saved bytes: %02X %02X %02X %02X | %02X %02X %02X %02X\n",
                s_savedPatchBytes[0], s_savedPatchBytes[1], s_savedPatchBytes[2], s_savedPatchBytes[3],
                s_savedPatchBytes[20], s_savedPatchBytes[21], s_savedPatchBytes[22], s_savedPatchBytes[23]);
    }

    // --- Hook HUD::Manager::Update to keep correction params current ---
    auto* target = (void*)(s_addrs->hud_manager_update + s_baseOffset);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (s_addrs->is_modtools) {
        s_origUpdate_cd = (fn_HudUpdate_cd)target;
        DetourAttach(&(PVOID&)s_origUpdate_cd, hooked_HudUpdate_cd);
    } else {
        s_origUpdate_fc = (fn_HudUpdate_fc)target;
        DetourAttach(&(PVOID&)s_origUpdate_fc, hooked_HudUpdate_fc);
    }

    DetourTransactionCommit();

    // Initial correction params (screen dims typically available by now)
    update_correction_params();

    dbg_log("HUD widescreen reticle fix installed (%s, yscale=%.3f)\n",
            s_addrs->is_modtools ? "modtools" : "retail", s_yScale);
}

// ---------------------------------------------------------------------------
// Uninstall
// ---------------------------------------------------------------------------
void unpatch_hud_widescreen()
{
    if (!s_addrs) return;

    auto restore = [](uintptr_t loc, const void* data, size_t size) {
        if (!loc) return;
        DWORD oldProt;
        VirtualProtect((void*)loc, size, PAGE_READWRITE, &oldProt);
        memcpy((void*)loc, data, size);
        VirtualProtect((void*)loc, size, oldProt, &oldProt);
    };

    if (s_addrs->is_modtools) {
        restore(s_faddPatchLoc, &s_savedFaddAddr, 4);
        restore(s_fmulPatchLoc, &s_savedFmulAddr, 4);
    } else {
        restore(s_patchLoc, s_savedPatchBytes, 24);
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (s_addrs->is_modtools && s_origUpdate_cd)
        DetourDetach(&(PVOID&)s_origUpdate_cd, hooked_HudUpdate_cd);
    else if (s_origUpdate_fc)
        DetourDetach(&(PVOID&)s_origUpdate_fc, hooked_HudUpdate_fc);

    DetourTransactionCommit();
    s_addrs = nullptr;
}
