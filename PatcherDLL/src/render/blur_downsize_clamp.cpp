#include "pch.h"
#include "blur_downsize_clamp.hpp"
#include "core/resolve.hpp"

#include <detours.h>

// =============================================================================
// BlurEffect::Render downsize clamp — port of PrismaticFlower's upstream 1f8f618.
//
// The blur effect renders the scene into a render target downsized by a fixed
// factor (mDownsizeFactor @ +0x30).  The factor was tuned for ~2005 resolutions;
// at modern resolutions the downsized target is much larger than intended, so
// the blur is both weaker-looking and more expensive.  Before each Render we
// check the factor against the current viewport and rescale it so the
// downsized target never exceeds 512px on its long edge.
//
// Upstream patched the vtable slot to a MASM trampoline; we detour the Render
// body itself (all calls dispatch through it), matching this fork's hook style.
// =============================================================================

using fn_BlurRender_t = void(__fastcall*)(void* self, void* edx, uint32_t flags);
using fn_GetViewportExtents_t = void(__cdecl*)(float* minX, float* minY, float* maxX, float* maxY);

static fn_BlurRender_t          original_BlurRender    = nullptr;
static fn_GetViewportExtents_t  fn_getViewportExtents  = nullptr;

static constexpr size_t kDownsizeFactorOffset   = 0x30;
static constexpr float  kMaxDownsizeResolution  = 512.0f;

static void __fastcall hooked_BlurRender(void* self, void* edx, uint32_t flags)
{
    float vpMinX = 0.0f, vpMinY = 0.0f, vpMaxX = 0.0f, vpMaxY = 0.0f;

    fn_getViewportExtents(&vpMinX, &vpMinY, &vpMaxX, &vpMaxY);

    float* downsizeFactor = (float*)((char*)self + kDownsizeFactorOffset);

    const float width  = vpMaxX - vpMinX;
    const float height = vpMaxY - vpMinY;
    const float viewportMaxLength = width > height ? width : height;
    const float currentDownsizedResolution = *downsizeFactor * viewportMaxLength;

    if (currentDownsizedResolution > kMaxDownsizeResolution) {
        *downsizeFactor *= (kMaxDownsizeResolution / currentDownsizedResolution);
    }

    original_BlurRender(self, edx, flags);
}

bool g_blurDownsizeClampEnabled = true;

void blur_downsize_clamp_install(uintptr_t exe_base)
{
    if (!g_blurDownsizeClampEnabled) return;
    if (g_build == GameBuild::Unknown) return;
    if (g_addr->blur_effect_render == 0 ||
        g_addr->red_renderer_get_viewport_extents == 0)
        return;

    original_BlurRender   = (fn_BlurRender_t)resolve(exe_base, g_addr->blur_effect_render);
    fn_getViewportExtents = (fn_GetViewportExtents_t)resolve(exe_base, g_addr->red_renderer_get_viewport_extents);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)original_BlurRender, hooked_BlurRender);
    DetourTransactionCommit();
}

void blur_downsize_clamp_uninstall()
{
    if (!original_BlurRender) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)original_BlurRender, hooked_BlurRender);
    DetourTransactionCommit();
    original_BlurRender = nullptr;
}
