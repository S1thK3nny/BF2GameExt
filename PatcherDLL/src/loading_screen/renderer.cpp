#include "pch.h"
#include "shared.hpp"

// =============================================================================
// RenderScreen hook
// =============================================================================
// Coordinates are normalized 0-1 screen space, confirmed from BF1 Ghidra analysis.
//
// TODO: no aspect correction anywhere in this file.  Every rect is taken
// literally in normalized space, so w and h scale against different pixel
// counts and a modder has to hand-compute h = w * screenAspect to keep a square
// texture square.  Anything tuned for 16:9 draws stretched at 4:3 or 16:10.
// This applies to all three consumers:
//   - AnimatedTextures (AnimEntry x/y/w/h)
//   - the PlanetLevel zoom sequence (PlanetEntry x/y/w/h -> zx0..zy1)
//   - the ZoomSelector tiles, where zoomTileHalfH defaults to zoomTileHalfW and
//     so is never actually square on screen
// Fixing it needs the backbuffer dimensions at render time plus an opt-in config
// flag, since existing configs are authored against the raw behaviour and would
// all shift if correction were applied unconditionally.

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

        // No aspect correction here — see the file-header TODO.
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
        g_prt(an.hashes[frame],
              ax, ay, ax + aw, ay + ah, g_color_ptr, 0,
              0,0,1,1,  1,1,0,0);
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
                    const float a = anim_ease_out((float)(ph - kOffC) / (float)kZIMs);
                    zx0 = tx0 * (1.0f - a);            zy0 = ty0 * (1.0f - a);
                    zx1 = tx1 + (1.0f - tx1) * a;     zy1 = ty1 + (1.0f - ty1) * a;
                    fgLevel = nxt;
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
            if (bg.texHash) {
                g_prt(bg.texHash, 0.0f, 0.0f, 1.0f, 1.0f,
                      g_color_ptr, 0, 0,0,1,1, 1,1,0,0);
            }
        }

        // Foreground — Phase C only.
        if (fgLevel >= 0) {
            const auto& fg = g_loadScreenCfg.planets[fgLevel];
            if (fg.texHash) {
                g_prt(fg.texHash, zx0, zy0, zx1, zy1,
                      g_color_ptr, 0, 0,0,1,1, 1,1,0,0);
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
                g_prt(h,x0_,y0_,x1_,y1_, g_color_ptr, 0, 0,0,1,1, 1,1,0,0);
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
    if (g_loadScreenCfg.scanLineTexHash)
        g_prt(g_loadScreenCfg.scanLineTexHash,
              0, 0, kW, kH, g_color_ptr, 0,
              0,0,1,1,  1,1,0,0);

    // Restore the heap.
    if (prevHeap >= 0 && g_set_current_heap)
        g_set_current_heap(prevHeap);
}
