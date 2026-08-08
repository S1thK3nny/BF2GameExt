#include "pch.h"
#include "shared.hpp"
#include "util/shader_patch_detect.hpp"

#include <math.h>   // expf — the zoom cross-fade curve

// =============================================================================
// RenderScreen hook
// =============================================================================
// Coordinates are normalized 0-1 screen space, confirmed from BF1 Ghidra analysis.
//
// There is no aspect correction anywhere in this file, and measurement says it
// does not need any.  Recorded here because the absence reads like an oversight:
//
//   - PlanetLevel rects are aspect-INDEPENDENT and correct as-is.  The level
//     image is drawn full-screen with UV 0..1, so image fraction and screen
//     fraction are the same number at every resolution, and the rect tracks the
//     same subject whatever the screen does.  This is the same identity the zoom
//     phase relies on when it reuses tx0..ty1 as both screen rect and UV crop.
//     Correcting anything here would break that.
//
//   - The ZoomSelector tiles are drawn non-square: zoomTileHalfH defaults to
//     zoomTileHalfW (config_parser.cpp, and again at the fallback below), so a
//     tile spans 2*hw of screen width by 2*hh of screen height.  The stock strip
//     textures are immune by construction - load_edge_horz is uniform along x
//     (one distinct column) and load_edge_vert uniform along y, so the stretch
//     has nothing to act on.  What is left is line thickness, (2/32) * 0.04 * the
//     screen dimension: 3.6 px horizontal against 6.4 px vertical at 2560x1440,
//     3.6 against 4.8 at 1920x1440.  Play-tested at both; indistinguishable on a
//     hairline sweeping over a moving backdrop.  The non-square corner tile is
//     also load-bearing, its arms match the thicknesses of the two bands it joins
//     precisely because it scales with each axis separately.
//     An edge texture carrying real 2D detail (dashes, ticks, a bracket) would
//     show the stretch.  That is the case worth revisiting, and only that one.
//
//   - AnimatedTextures rects distort a panel's own aspect the same way, but
//     there is no correct answer for an arbitrary overlay rect and modders tune
//     these by eye.  Preserving texture aspect would need an opt-in key and would
//     move every overlay already authored against the current behaviour.

// One textured quad through PlatformRenderTexture.
//
// `alpha` is the per-draw tweak-colour alpha, 255 = opaque.  On modtools it
// rides PlatformRenderTexture's real RedColor* argument; on retail that
// argument was constant-folded away and it arrives via the operand patch in
// lifecycle.cpp.  The tint is restored to opaque white after every call so
// nothing else in the frame inherits a ramp — safe because pcRenderPrimitive
// copies the colour into RenderItem::tweakColor rather than keeping the
// pointer, confirmed by disassembly on both build families.
//
// alphaBlend is derived from the alpha rather than passed in.  Stock
// LoadDisplay::RenderScreen passes true unconditionally (modtools 0x0067a1b0,
// `&RedColor::WHITE, true`) and the retail builds hardwired it on, so passing
// true everywhere would be the more faithful thing — but every one of these
// overlays was authored and play-tested against the flag being off on
// modtools, and flipping it for opaque quads changes their look for no gain.
// An opaque quad keeps the historical path; only a quad that actually needs to
// ramp asks for blending.
static void prt_quad(uint32_t hash,
                     float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1,
                     uint8_t alpha = 255)
{
    if (!hash) return;
    const int blend = (alpha != 255) ? 1 : 0;
    g_prtTint = 0x00FFFFFFu | ((uint32_t)alpha << 24);
    g_prt(hash, x0, y0, x1, y1, &g_prtTint, blend, u0, v0, u1, v1, 1, 1, 0, 0);
    g_prtTint = 0xFFFFFFFFu;
}

void __fastcall hooked_render_screen(void* ecx, void* edx)
{
    // Suppress BF2's RandomBackdrop draw when BF1 mode is active.
    // LoadDisplay::RenderScreen (0x0067a1b0) ONLY draws the RandomBackdrop hash
    // stored in m_backDropHash.  PlatformRenderTexture skips when hash==0, so we
    // temporarily zero it and restore it after the call.
    using namespace load_display;

    uint32_t savedBackdropHash = 0;
    if (g_loadScreenCfg.bf1Enabled && ecx) {
        uint32_t* pHash = at<uint32_t>(ecx, kBackDropHash);
        savedBackdropHash = *pHash;
        *pHash = 0;
    }

    // Element suppression flags.  Each one hides exactly the element it names —
    // they are deliberately independent, because the engine's screen groups do
    // not line up with what a modder thinks of as one piece of UI.  In
    // particular m_groupBottomLeft holds BOTH the "Loading" caption and the
    // progress bar, so hiding the group could never mean "hide the bar".
    if (ecx) {
        if (g_loadScreenCfg.removeToolTips)
            set_element_visible(ecx, kGroupLoadingTips, false);
        if (g_loadScreenCfg.removeLoadingText)
            set_element_visible(ecx, kTextLoading, false);
        if (g_loadScreenCfg.removeMissionName)
            set_element_visible(ecx, kTextMissionName, false);
        if (g_loadScreenCfg.removeModeName)
            set_element_visible(ecx, kTextModeName, false);

        if (g_loadScreenCfg.removeLoadingBar) {
            set_element_visible(ecx, kProgressBar, false);
            // Belt and braces: also blank the LEDs.  m_pIntensity is the per-LED
            // intensity array (not alpha), and ProgressIndicator::Update rewrites
            // it every tick — this only sticks because RenderScreen runs after
            // that update inside the same 50 ms block.
            int    numLEDs   = *at<int>(ecx, bar(pi::kNumLEDs));
            float* intensity = *at<float*>(ecx, bar(pi::kPIntensity));
            if (intensity && numLEDs > 0 && numLEDs < 200) {
                for (int i = 0; i < numLEDs; ++i)
                    intensity[i] = 0.0f;
            }
        }
    }

    g_orig_render_screen(ecx, edx);

    if (g_loadScreenCfg.bf1Enabled && ecx)
        *at<uint32_t>(ecx, kBackDropHash) = savedBackdropHash;


    // AnimatedTextures and ScanLineTexture are plain overlays that depend on
    // nothing in the zoom sequence, so they draw with or without EnableBF1.  The
    // rest of this function is already self-disabling without it: the post-parse
    // gate in config_parser.cpp zeroes planetCount, zoomSelCount and every sound
    // hash, so the zoom block and all its sound triggers never run.
    if (!g_prt)
        return;
    if (!g_loadScreenCfg.bf1Enabled
        && g_loadScreenCfg.animSlotCount == 0
        && !g_loadScreenCfg.scanLineTexHash)
        return;

    // One-shot texture probe: log errors for any BF1 textures that aren't in
    // the global texture hash table.
    static bool s_texProbed = false;
    if (!s_texProbed && g_pbl_find && g_tex_table) {
        s_texProbed = true;
        auto fn_log = get_gamelog();
        auto check = [&](const char* label, uint32_t hash) {
            if (hash && !g_pbl_find(g_tex_table, 0x2000, hash))
                fn_log("[BF1Ext] ERROR: %s texture hash %08x not found in texture table\n", label, hash);
        };
        check("ScanLineTexture", g_loadScreenCfg.scanLineTexHash);
        for (int i = 0; i < g_loadScreenCfg.zoomSelCount; ++i)
            check("ZoomSelectorTextures", g_loadScreenCfg.zoomSelHashes[i]);
        for (int s = 0; s < g_loadScreenCfg.animSlotCount; ++s) {
            const auto& an = g_loadScreenCfg.anims[s];
            for (int i = 0; i < an.count; ++i)
                check("AnimatedTextures", an.hashes[i]);
        }
        for (int pi = 0; pi < g_loadScreenCfg.planetCount; ++pi)
            check("PlanetLevel", g_loadScreenCfg.planets[pi].texHash);
    }

    static constexpr float kW = 1.0f, kH = 1.0f;

    // Switch to RunTimeHeap so any SortHeap array growth goes to persistent heap.
    int prevHeap = -1;
    if (g_set_current_heap && g_runtime_heap_idx)
        prevHeap = g_set_current_heap(*g_runtime_heap_idx);

    // --- AnimatedTextures: cycle each overlay's frames at its own fps ---
    // Drawn in config order, so a later AnimatedTextures layers over an earlier
    // one.  All overlays share g_animStartMs, so equal-length sequences stay in
    // lockstep with each other.
    for (int s = 0; s < g_loadScreenCfg.animSlotCount; ++s) {
        const LoadScreenConfig::AnimEntry& an = g_loadScreenCfg.anims[s];
        if (an.count <= 0)
            continue;

        // Phase is measured from the start of THIS loading screen (g_animStartMs
        // is re-stamped in LoadConfig), so a sequence with a distinct first frame
        // actually begins on it instead of wherever system uptime happens to land.
        const DWORD elapsed = GetTickCount() - g_animStartMs;
        // fps <= 0 means "hold on frame 0", not "hide the overlay".  The index is
        // computed as elapsed*fps rather than elapsed/(1000/fps) so high rates do
        // not truncate the period to zero and freeze.
        const int frame = (an.fps > 0.0f)
                        ? (int)(((uint64_t)((double)elapsed * (double)an.fps / 1000.0))
                                % (uint64_t)an.count)
                        : 0;
        if (!an.hashes[frame])
            continue;

        // No aspect correction here — see the file header for why not.
        //
        // The rect is all-or-nothing: w and h are both needed to place the
        // overlay, so a partial rect falls back to full screen rather than
        // honouring half the placement and drawing off the edge.  The mismatch
        // is reported once at parse time.
        const bool  haveRect = an.w > 0.0f && an.h > 0.0f;
        const float ax = haveRect ? an.x : 0.0f;
        const float ay = haveRect ? an.y : 0.0f;
        const float aw = haveRect ? an.w : 1.0f;
        const float ah = haveRect ? an.h : 1.0f;
        prt_quad(an.hashes[frame], ax, ay, ax + aw, ay + ah, 0, 0, 1, 1);
    }

    // --- BF1 zoom-sequence animation ---
    if (g_loadScreenCfg.planetCount > 0) {
        const int nLev = g_loadScreenCfg.planetCount;

        int nTrans = 0;
        for (int i = 0; i < nLev; ++i) {
            const auto& e = g_loadScreenCfg.planets[i];
            if (e.w > 0.0f && e.h > 0.0f) nTrans++;
            else break;
        }

        static constexpr DWORD kTBMs   = 1200u;
        static constexpr DWORD kHoldMs =  400u;
        static constexpr DWORD kLRMs   = 1200u;
        static constexpr DWORD kZIMs   = 1500u;
        static constexpr DWORD kCyMs   = kAnimCycleMs;

        static constexpr DWORD kOffAh = kTBMs;
        static constexpr DWORD kOffB  = kTBMs + kHoldMs;
        static constexpr DWORD kOffBh = kTBMs + kHoldMs + kLRMs;
        static constexpr DWORD kOffC  = kTBMs + kHoldMs + kLRMs + kHoldMs;

        int   bgLevel = 0;
        int   fgLevel = -1;
        float zx0 = 0.0f, zy0 = 0.0f, zx1 = 1.0f, zy1 = 1.0f;
        bool  animDone = false;

        // Zoom phase only.  The sub-rect of the OUTGOING level's texture that the
        // zoom magnifies, in UV space.  Screen rect and UV rect are the same
        // numbers here — the planet entry is authored in normalized screen space
        // and the texture is drawn full-screen, so the region under the selector
        // is at the same fractional coordinates in both spaces.
        float cu0 = 0.0f, cv0 = 0.0f, cu1 = 1.0f, cv1 = 1.0f;
        // Linear zoom progress 0..1, NOT the eased rect curve — BF1 ramps the
        // hand-over on the raw phase fraction, so the timing is read off that.
        float zoomT = 0.0f;

        if (nTrans > 0) {
            const DWORD totalMs    = (DWORD)nTrans * kCyMs;
            const DWORD rawElapsed = GetTickCount() - g_animStartMs;

            if (rawElapsed >= totalMs) {
                animDone = true;
                bgLevel  = (nTrans < nLev) ? nTrans : nTrans - 1;
                if (s_lastAnimPhase != -2) {
                    tracking_sound_stop();
                    s_lastAnimPhase = -2;
                }
            } else {
                const DWORD elapsed = rawElapsed % totalMs;
                const int   ti      = (int)(elapsed / kCyMs);
                const DWORD ph      = elapsed % kCyMs;

                const int   nxt  = (ti + 1 < nLev) ? ti + 1 : ti;
                const auto& cur = g_loadScreenCfg.planets[ti];
                const float tx0  = cur.x,         ty0 = cur.y;
                const float tx1  = cur.x + cur.w, ty1 = cur.y + cur.h;
                bgLevel = ti;

                if (ph < kOffAh) {
                    const float t = anim_smoothstep((float)ph / (float)kTBMs);
                    zx0 = 0.0f;                        zx1 = 1.0f;
                    zy0 = ty0 * t;                     zy1 = 1.0f - (1.0f - ty1) * t;
                }
                else if (ph < kOffB) {
                    zx0 = 0.0f;  zx1 = 1.0f;
                    zy0 = ty0;   zy1 = ty1;
                }
                else if (ph < kOffBh) {
                    const float t = anim_smoothstep((float)(ph - kOffB) / (float)kLRMs);
                    zx0 = tx0 * t;                     zx1 = 1.0f - (1.0f - tx1) * t;
                    zy0 = ty0;                         zy1 = ty1;
                }
                else if (ph < kOffC) {
                    zx0 = tx0;  zx1 = tx1;
                    zy0 = ty0;  zy1 = ty1;
                }
                else {
                    zoomT = (float)(ph - kOffC) / (float)kZIMs;
                    const float a = anim_ease_out(zoomT);
                    zx0 = tx0 * (1.0f - a);            zy0 = ty0 * (1.0f - a);
                    zx1 = tx1 + (1.0f - tx1) * a;     zy1 = ty1 + (1.0f - ty1) * a;
                    fgLevel = nxt;
                    cu0 = tx0;  cv0 = ty0;  cu1 = tx1;  cv1 = ty1;
                }

                // Sound triggers
                const int curPhase = (ph < kOffAh) ? 0
                                   : (ph < kOffB)  ? 1
                                   : (ph < kOffBh) ? 2
                                   : (ph < kOffC)  ? 3
                                                   : 4;

                if (ti != s_lastAnimCycle || curPhase != s_lastAnimPhase) {
                    const bool newCycle = (ti != s_lastAnimCycle);

                    if (newCycle && s_lastAnimPhase == 4)
                        loading_screen_play_sound(g_loadScreenCfg.transitionSoundHash);

                    switch (curPhase) {
                    case 0: tracking_sound_start(g_loadScreenCfg.yTrackSoundHash); break;
                    case 1: tracking_sound_stop(); break;
                    case 2: tracking_sound_start(g_loadScreenCfg.xTrackSoundHash); break;
                    case 3: tracking_sound_stop(); break;
                    case 4:
                        tracking_sound_stop();
                        loading_screen_play_sound(g_loadScreenCfg.zoomSoundHash);
                        break;
                    }

                    s_lastAnimPhase = curPhase;
                    s_lastAnimCycle = ti;
                }
            }
        }

        // Non-planet mode: play BarSound periodically.
        if (nTrans == 0 && g_loadScreenCfg.barSoundHash) {
            const DWORD now = GetTickCount();
            if (s_nextBarSoundMs == 0 || now >= s_nextBarSoundMs) {
                const DWORD intervalMs = (g_loadScreenCfg.barSoundInterval > 0)
                                       ? (DWORD)g_loadScreenCfg.barSoundInterval * 1000u
                                       : 5000u;
                s_nextBarSoundMs = now + intervalMs;
                loading_screen_play_sound(g_loadScreenCfg.barSoundHash);
            }
        }

        // Background — full-screen, opaque.
        {
            const auto& bg = g_loadScreenCfg.planets[bgLevel];
            prt_quad(bg.texHash, 0.0f, 0.0f, 1.0f, 1.0f, 0, 0, 1, 1);
        }

        // Foreground — zoom phase only.  The OUTGOING level's texture, UV-cropped
        // to the rect the selector just finished framing and stretched over the
        // growing rect, so that region magnifies.  This is the half that was
        // missing: the zoom only ever grew the incoming image, so the outgoing
        // one sat motionless at full-screen size behind it and the shot never
        // appeared to push in.  At progress 0 the quad lands on exactly the
        // pixels the full-screen backdrop already shows there, so the
        // magnification starts seamlessly instead of popping, and the cut to the
        // incoming level happens at the cycle boundary where the next
        // background takes over full-screen.
        //
        // The INCOMING level's texture over the same rect, alpha ramping up
        // (SWBF1 0x001ba888).  BF1 fades the incoming image IN rather than
        // fading the crop OUT — the crop is drawn opaque and simply gets
        // covered — which keeps the pair opaque instead of letting the backdrop
        // show through a half-transparent sandwich.
        //
        // The curve reproduces BF1's, which is not a curve at all in the
        // original: UpdateZoom re-applies `fade *= (1 - progress)` every frame,
        // so the decay compounds and is frame-rate dependent.  Integrating that
        // over N frames gives roughly exp(-N*t^2/2); at 60 fps over a 1.5 s zoom
        // that is exp(-45*t^2), which is what is evaluated here — same shape,
        // but independent of frame rate.  It puts the crop at ~5% opacity a
        // quarter of the way in and invisible just after, matching how BF1
        // actually looks.
        //
        // Once the incoming image is fully opaque the crop underneath cannot
        // show, so it is dropped and the phase falls back to two opaque quads —
        // the same draw shape as the rest of the sequence.
        //
        // Shader Patch cannot take this at all, so under SP the whole thing —
        // magnifying crop included — stands down to the pre-BF1 behaviour of
        // simply growing the incoming image.  Not a cosmetic preference: the
        // crop only reads as a zoom because the fade hands over mid-flight, and
        // on its own it just looks wrong.
        //
        // The reason is SP's fixed-function state matcher
        // (direct3d/texture_stage_state_manager.cpp).  The engine puts
        // tweakColor into D3DRS_TEXTUREFACTOR, and SP identifies the draw by
        // pattern-matching the texture stage setup:
        //
        //     bool is_plain_texture_state(const DWORD texture_factor) ...
        //        if (texture_factor != D3DCOLOR_ARGB(0xff,0xff,0xff,0xff)) return false;
        //
        // Exactly opaque white or no match, and an unmatched fixed-function
        // state terminates.  So under SP the only usable tweak colour here is
        // 0xFFFFFFFF: no alpha ramp, and no RGB ramp either (a dip-to-black
        // fails the identical check).  See docs/LoadDisplaySystem.md.
        if (fgLevel >= 0) {
            const auto& fg = g_loadScreenCfg.planets[fgLevel];

            if (shader_patch_present()) {
                prt_quad(fg.texHash, zx0, zy0, zx1, zy1, 0, 0, 1, 1);
            }
            else {
                const auto&   outgoing    = g_loadScreenCfg.planets[bgLevel];
                const float   cropOpacity = expf(-45.0f * zoomT * zoomT);
                const uint8_t inAlpha     = (uint8_t)((1.0f - cropOpacity) * 255.0f + 0.5f);

                if (inAlpha < 255)
                    prt_quad(outgoing.texHash, zx0, zy0, zx1, zy1, cu0, cv0, cu1, cv1);

                prt_quad(fg.texHash, zx0, zy0, zx1, zy1, 0, 0, 1, 1, inAlpha);
            }
        }

        // ZoomSelector 16-quad crosshair
        if (g_loadScreenCfg.zoomSelCount > 0 && !animDone) {
            const float hw = g_loadScreenCfg.zoomTileHalfW > 0.0f ? g_loadScreenCfg.zoomTileHalfW : 0.02f;
            const float hh = g_loadScreenCfg.zoomTileHalfH > 0.0f ? g_loadScreenCfg.zoomTileHalfH : hw;

            const uint32_t zH = g_loadScreenCfg.zoomSelHashes[0];
            const uint32_t zV = g_loadScreenCfg.zoomSelCount > 1 ? g_loadScreenCfg.zoomSelHashes[1] : zH;
            const uint32_t zC = g_loadScreenCfg.zoomSelCount > 2 ? g_loadScreenCfg.zoomSelHashes[2] : zH;

            const float L0=zx0-hw, L1=zx0+hw, R0=zx1-hw, R1=zx1+hw;
            const float T0=zy0-hh, T1=zy0+hh, B0=zy1-hh, B1=zy1+hh;

            auto tile = [&](uint32_t h, float x0_,float y0_,float x1_,float y1_) {
                if (!h || x1_<=x0_ || y1_<=y0_) return;
                if (x0_>2.f||y0_>2.f||x1_>2.f||y1_>2.f) return;
                prt_quad(h, x0_, y0_, x1_, y1_, 0, 0, 1, 1);
            };

            tile(zH,0.0f,T0,L0,T1); tile(zH,L1,T0,R0,T1); tile(zH,R1,T0,1.f,T1);
            tile(zH,0.0f,B0,L0,B1); tile(zH,L1,B0,R0,B1); tile(zH,R1,B0,1.f,B1);
            tile(zV,L0,0.0f,L1,T0); tile(zV,L0,T1,L1,B0); tile(zV,L0,B1,L1,1.f);
            tile(zV,R0,0.0f,R1,T0); tile(zV,R0,T1,R1,B0); tile(zV,R0,B1,R1,1.f);
            tile(zC,L0,T0,L1,T1);   tile(zC,R0,T0,R1,T1);
            tile(zC,L0,B0,L1,B1);   tile(zC,R0,B0,R1,B1);
        }
    }

    // --- ScanLineTexture: full-screen overlay drawn last ---
    prt_quad(g_loadScreenCfg.scanLineTexHash, 0, 0, kW, kH, 0, 0, 1, 1);

    // Restore the heap.
    if (prevHeap >= 0 && g_set_current_heap)
        g_set_current_heap(prevHeap);
}
