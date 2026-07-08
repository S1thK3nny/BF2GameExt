#include "pch.h"
#include "hud_widescreen.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <detours.h>

// =============================================================================
// HUD widescreen reticle fix
// =============================================================================
// The vanilla letterbox transform in HUD::Manager::Update applies a Y-scale and
// Y-offset to ALL HUD elements on widescreen displays, misaligning the reticle
// with the 3D aim point (error grows with distance from screen center).
//
// Fix: pre-distort ONLY the reticle Y in ReticuleDisplay::Update so that, after
// the letterbox transform, it lands on the correct spot.  Everything else
// (minimap, ammo counter, etc.) is untouched.
//
//   corrected_ndc_y = (ndc_y - y_offset/480) / y_scale
//
// Modtools (x87): the compiler emits (projY + 1.0) * 0.5 — redirect the FADD /
//   FMUL constant-address operands to DLL-controlled statics.
// Retail (SSE): the compiler emits projY * 0.5 + 0.5 — replace the MULSS+ADDSS
//   region with a CALL to a naked correction thunk.
// =============================================================================

// [Fixes] ReticleCorrection: -1 auto, 0 disable, or manual strength.
float g_reticleCorrection = -1.0f;

// Runtime-only logger (never called during install; see the install-window
// no-game-code rule).  Resolved in hud_widescreen_install.
static GameLog_t g_log = nullptr;

// ---------------------------------------------------------------------------
// Correction parameters — refreshed each frame from HUD::Manager::Update hook.
// ---------------------------------------------------------------------------

// Retail SSE correction thunk reads these:
static float s_yScale      = 1.0f;  // effective letterbox Y scale (1.0 = none)
static float s_yOffsetNorm = 0.0f;  // effective y_offset / screenH

// Modtools x87 constant redirect: (projY + s_addConst) * s_mulConst = NDC.
static float s_addConst = 1.0f;
static float s_mulConst = 0.5f;

static float s_correctionStrength = -1.0f;

// Resolved screen-dimension globals.
static const uint32_t* s_screenWidth  = nullptr;
static const uint32_t* s_screenHeight = nullptr;

static int s_logOnce = 0;

// ---------------------------------------------------------------------------
// Retail SSE correction thunk (naked — called from the patched code).
//   Entry: XMM0 = projY, XMM1 = 0.5.  Exit: XMM0 = corrected NDC Y.
// ---------------------------------------------------------------------------
__declspec(naked) static void __cdecl reticle_y_correction()
{
   __asm {
      mulss xmm0, xmm1                       // projY * 0.5
      addss xmm0, xmm1                       // + 0.5 -> ndc_y
      subss xmm0, dword ptr [s_yOffsetNorm]  // - (1-y_scale)/2
      divss xmm0, dword ptr [s_yScale]       // / y_scale
      ret
   }
}

// ---------------------------------------------------------------------------
// Recompute correction params from the live screen dimensions.
// ---------------------------------------------------------------------------
static void update_correction_params()
{
   if (!s_screenWidth || !s_screenHeight) return;
   uint32_t w = *s_screenWidth;
   uint32_t h = *s_screenHeight;
   if (w == 0 || h == 0) return;

   float fw = (float)w;
   float fh = (float)h;
   float delta = fw * 0.75f - fh;

   if (delta >= 2.0f) {
      // Widescreen. Auto mode (strength<0) uses (1-yscale), which = 1/3 at 16:9.
      float yscale = 1.0f - delta / fh;
      float str    = s_correctionStrength < 0.0f ? (1.0f - yscale) : s_correctionStrength;
      float yscale_eff = 1.0f - str * (1.0f - yscale);
      float yoff_eff   = str * (1.0f - yscale) * 0.5f;

      // Retail SSE: corrected = (ndc_y - yoff_eff) / yscale_eff
      s_yScale      = yscale_eff;
      s_yOffsetNorm = yoff_eff;

      // Modtools x87: corrected = (projY + A) * B, B = 0.5/yscale_eff, A = 1 - 2*yoff_eff
      s_mulConst    = 0.5f / yscale_eff;
      s_addConst    = 1.0f - 2.0f * yoff_eff;
   } else {
      // 4:3 or taller — identity (no correction).
      s_yScale      = 1.0f;
      s_yOffsetNorm = 0.0f;
      s_mulConst    = 0.5f;
      s_addConst    = 1.0f;
   }

   if (s_logOnce < 3) {
      s_logOnce++;
      if (g_log)
         g_log("[HUD_WS] screen=%ux%u delta=%.1f str=%.2f yscale=%.4f yoff=%.4f",
               w, h, delta, s_correctionStrength, s_yScale, s_yOffsetNorm);
   }
}

// ---------------------------------------------------------------------------
// ReticuleDisplay::Update hook — refreshes correction params right before the
// reticle transform runs, every frame the reticle is drawn (with valid screen
// dims).  __thiscall bool(this, float) on every build.  This is the function
// that OWNS the FADD/FMUL (modtools) and MULSS/ADDSS (retail) that we patch, so
// hooking it keeps the constants current no matter the resolution.
// ---------------------------------------------------------------------------
using fn_ReticuleUpdate = bool(__fastcall*)(void* self, void* edx, float p1);
static fn_ReticuleUpdate s_origReticuleUpdate = nullptr;

static bool __fastcall hooked_ReticuleUpdate(void* self, void* edx, float p1)
{
   update_correction_params();
   return s_origReticuleUpdate(self, edx, p1);
}

// ---------------------------------------------------------------------------
// Per-build address set (selected from g_build, populated from game_addrs)
// ---------------------------------------------------------------------------
struct HudWsAddrs {
   uintptr_t reticle_display_update; // ReticuleDisplay::Update — detoured to refresh params
   uintptr_t screen_width;
   uintptr_t screen_height;
   uintptr_t fadd_operand;   // modtools only: FADD [1.0] operand
   uintptr_t fmul_operand;   // modtools only: FMUL [0.5] operand
   uintptr_t mulss_patch;    // retail only: start of 24-byte MULSS..ADDSS region
   bool      is_modtools;
   bool      guard_update_prologue; // verify the update entry before detouring (derived GOG addr)
};

static constexpr HudWsAddrs MODTOOLS_ADDRS = {
   game_addrs::modtools::reticle_display_update,
   game_addrs::modtools::hud_screen_width,
   game_addrs::modtools::hud_screen_height,
   game_addrs::modtools::hud_reticle_fadd_operand,
   game_addrs::modtools::hud_reticle_fmul_operand,
   0,
   true,
   false,
};

static constexpr HudWsAddrs STEAM_ADDRS = {
   game_addrs::steam::reticle_display_update,
   game_addrs::steam::hud_screen_width,
   game_addrs::steam::hud_screen_height,
   0, 0,
   game_addrs::steam::hud_reticle_mulss_patch,
   false,
   false,
};

static constexpr HudWsAddrs GOG_ADDRS = {
   game_addrs::gog::reticle_display_update,
   game_addrs::gog::hud_screen_width,
   game_addrs::gog::hud_screen_height,
   0, 0,
   game_addrs::gog::hud_reticle_mulss_patch,
   false,
   true, // GOG update address is derived — verify the prologue first
};

static const HudWsAddrs* s_addrs = nullptr;

// Saved bytes for uninstall.
static uint8_t   s_savedPatchBytes[24];
static uintptr_t s_patchLoc = 0;
static uint32_t  s_savedFaddAddr = 0;
static uintptr_t s_faddPatchLoc = 0;
static uint32_t  s_savedFmulAddr = 0;
static uintptr_t s_fmulPatchLoc = 0;

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
void hud_widescreen_install(uintptr_t exe_base)
{
   if (g_reticleCorrection == 0.0f) return; // 0 = disabled

   switch (g_build) {
      case GameBuild::Modtools: s_addrs = &MODTOOLS_ADDRS; break;
      case GameBuild::Steam:    s_addrs = &STEAM_ADDRS;    break;
      case GameBuild::GOG:      s_addrs = &GOG_ADDRS;      break;
      default:                  return; // unsupported build
   }

   // For builds whose update address is derived (GOG), verify the function
   // prologue before touching anything — a wrong guess would otherwise detour a
   // random function.  Signature: PUSH EBP; MOV EBP,ESP; AND ESP,-8; SUB ESP,imm8.
   if (s_addrs->guard_update_prologue) {
      const uint8_t* p = (const uint8_t*)resolve(exe_base, s_addrs->reticle_display_update);
      static const uint8_t kPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC };
      if (memcmp(p, kPrologue, sizeof(kPrologue)) != 0) {
         s_addrs = nullptr; // disable — leave the exe untouched
         return;
      }
   }

   g_log = get_gamelog(); // runtime-only — must not be called during install
   s_correctionStrength = g_reticleCorrection;
   s_screenWidth  = (const uint32_t*)resolve(exe_base, s_addrs->screen_width);
   s_screenHeight = (const uint32_t*)resolve(exe_base, s_addrs->screen_height);

   if (s_addrs->is_modtools) {
      // Redirect the FADD [1.0] / FMUL [0.5] constant-address operands to our statics.
      s_faddPatchLoc = (uintptr_t)resolve(exe_base, s_addrs->fadd_operand);
      memcpy(&s_savedFaddAddr, (void*)s_faddPatchLoc, 4);
      uint32_t newAddr = (uint32_t)(uintptr_t)&s_addConst;
      protected_write((void*)s_faddPatchLoc, &newAddr, 4);

      s_fmulPatchLoc = (uintptr_t)resolve(exe_base, s_addrs->fmul_operand);
      memcpy(&s_savedFmulAddr, (void*)s_fmulPatchLoc, 4);
      newAddr = (uint32_t)(uintptr_t)&s_mulConst;
      protected_write((void*)s_fmulPatchLoc, &newAddr, 4);
   } else {
      // Retail (SSE): patch the 24-byte MULSS...ADDSS region with a CALL.
      //   [0..3]   MULSS XMM0, XMM1           (F3 0F 59 C1)
      //   [4..11]  MOV [ESP+0x1c], 0x3f000000 (C7 44 24 1C ...)
      //   [12..19] MOV [ESP+0x24], 0x0        (C7 44 24 24 ...)
      //   [20..23] ADDSS XMM0, XMM1           (F3 0F 58 C1)
      // Patched:
      //   [0..4]   CALL reticle_y_correction  (E8 rel32)
      //   [5..20]  preserved two MOV instructions
      //   [21..23] NOP NOP NOP
      s_patchLoc = (uintptr_t)resolve(exe_base, s_addrs->mulss_patch);
      memcpy(s_savedPatchBytes, (void*)s_patchLoc, 24);

      uint8_t patch[24];
      patch[0] = 0xE8;
      uint32_t rel32 = (uint32_t)(uintptr_t)&reticle_y_correction - (uint32_t)(s_patchLoc + 5);
      memcpy(&patch[1], &rel32, 4);
      memcpy(&patch[5], &s_savedPatchBytes[4], 16); // preserve the two MOVs
      patch[21] = patch[22] = patch[23] = 0x90;

      protected_write((void*)s_patchLoc, patch, 24);
   }

   // Detour ReticuleDisplay::Update to keep the correction params current.
   s_origReticuleUpdate = (fn_ReticuleUpdate)resolve(exe_base, s_addrs->reticle_display_update);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)s_origReticuleUpdate, hooked_ReticuleUpdate);
   DetourTransactionCommit();

   update_correction_params(); // initial params (screen dims usually available)
}

// ---------------------------------------------------------------------------
// Uninstall
// ---------------------------------------------------------------------------
void hud_widescreen_uninstall()
{
   if (!s_addrs) return;

   if (s_addrs->is_modtools) {
      if (s_faddPatchLoc) protected_write((void*)s_faddPatchLoc, &s_savedFaddAddr, 4);
      if (s_fmulPatchLoc) protected_write((void*)s_fmulPatchLoc, &s_savedFmulAddr, 4);
   } else if (s_patchLoc) {
      protected_write((void*)s_patchLoc, s_savedPatchBytes, 24);
   }

   if (s_origReticuleUpdate) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(&(PVOID&)s_origReticuleUpdate, hooked_ReticuleUpdate);
      DetourTransactionCommit();
   }

   s_addrs = nullptr;
}
