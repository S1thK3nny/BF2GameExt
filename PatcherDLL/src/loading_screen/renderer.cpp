#include "pch.h"
#include "shared.hpp"

// =============================================================================
// RenderScreen hook
// =============================================================================
// Coordinates are normalized 0-1 screen space, confirmed from BF1 Ghidra analysis.

void __fastcall hooked_render_screen(void* ecx, void* edx)
{
    // Suppress BF2's RandomBackdrop draw when BF1 mode is active.
    // LoadDisplay::RenderScreen (0x0067a1b0) ONLY draws the RandomBackdrop hash
    // stored at ecx+0x14c0.  PlatformRenderTexture skips when hash==0, so we
    // temporarily zero it and restore it after the call.
    uint32_t savedBackdropHash = 0;
    if (g_bf1Ext.bf1Enabled && ecx) {
        uint32_t* pHash = (uint32_t*)((uint8_t*)ecx + 0x14c0);
        savedBackdropHash = *pHash;
        *pHash = 0;
    }

    g_orig_render_screen(ecx, edx);

    if (g_bf1Ext.bf1Enabled && ecx)
        *(uint32_t*)((uint8_t*)ecx + 0x14c0) = savedBackdropHash;

    if (!g_bf1Ext.bf1Enabled || !g_prt || !g_color_ptr)
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
        for (int i = 0; i < g_bf1Ext.backdropCount; ++i)
            check("PlanetBackdrops", g_bf1Ext.backdropHashes[i]);
        check("ScanLineTexture", g_bf1Ext.scanLineTexHash);
        for (int i = 0; i < g_bf1Ext.zoomSelCount; ++i)
            check("ZoomSelectorTextures", g_bf1Ext.zoomSelHashes[i]);
        for (int i = 0; i < g_bf1Ext.animCount; ++i)
            check("AnimatedTextures", g_bf1Ext.animHashes[i]);
        for (int pi = 0; pi < g_bf1Ext.planetCount; ++pi)
            check("PlanetLevel", g_bf1Ext.planets[pi].texHash);
    }

    static constexpr float kW = 1.0f, kH = 1.0f;

    // Switch to RunTimeHeap so any SortHeap array growth goes to persistent heap.
    int prevHeap = -1;
    if (g_set_current_heap && g_runtime_heap_idx)
        prevHeap = g_set_current_heap(*g_runtime_heap_idx);

    // --- PlanetBackdrops: drawn first, full-screen, opaque ---
    if (g_bf1Ext.backdropCount > 0) {
        const int bi = (g_bf1Ext.backdropCount > 1)
                     ? (int)((GetTickCount() / 5000u) % (DWORD)g_bf1Ext.backdropCount)
                     : 0;
        const uint32_t bHash = g_bf1Ext.backdropHashes[bi];
        if (bHash && g_pbl_find && g_pbl_find(g_tex_table, 0x2000, bHash))
            g_prt(bHash, 0, 0, kW, kH, g_color_ptr, 0,
                  0,0,1,1,  1,1,0,0);
    }

    // --- AnimatedTextures: cycle frames at animFPS ---
    if (g_bf1Ext.animCount > 0 && g_bf1Ext.animFPS > 0.0f) {
        const DWORD ms_per_frame = (DWORD)(1000.0f / g_bf1Ext.animFPS);
        const int frame = (ms_per_frame > 0)
                        ? (int)(GetTickCount() / ms_per_frame) % g_bf1Ext.animCount
                        : 0;
        if (g_bf1Ext.animHashes[frame]) {
            const float ax = g_bf1Ext.animW > 0.0f ? g_bf1Ext.animX : 0.0f;
            const float ay = g_bf1Ext.animW > 0.0f ? g_bf1Ext.animY : 0.0f;
            const float aw = g_bf1Ext.animW > 0.0f ? g_bf1Ext.animW : 1.0f;
            const float ah = g_bf1Ext.animH > 0.0f ? g_bf1Ext.animH : 1.0f;
            g_prt(g_bf1Ext.animHashes[frame],
                  ax, ay, ax + aw, ay + ah, g_color_ptr, 0,
                  0,0,1,1,  1,1,0,0);
        }
    }

    // --- BF1 zoom-sequence animation ---
    if (g_bf1Ext.planetCount > 0) {
        const int nLev = g_bf1Ext.planetCount;

        int nTrans = 0;
        for (int i = 0; i < nLev; ++i) {
            const auto& e = g_bf1Ext.planets[i];
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
                const auto& cur = g_bf1Ext.planets[ti];
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
                        bf1_play_sound(g_bf1Ext.transitionSoundHash);

                    switch (curPhase) {
                    case 0: tracking_sound_start(g_bf1Ext.yTrackSoundHash); break;
                    case 1: tracking_sound_stop(); break;
                    case 2: tracking_sound_start(g_bf1Ext.xTrackSoundHash); break;
                    case 3: tracking_sound_stop(); break;
                    case 4:
                        tracking_sound_stop();
                        bf1_play_sound(g_bf1Ext.zoomSoundHash);
                        break;
                    }

                    s_lastAnimPhase = curPhase;
                    s_lastAnimCycle = ti;
                }
            }
        }

        // Non-planet mode: play BarSound periodically.
        if (nTrans == 0 && g_bf1Ext.barSoundHash) {
            const DWORD now = GetTickCount();
            if (s_nextBarSoundMs == 0 || now >= s_nextBarSoundMs) {
                const DWORD intervalMs = (g_bf1Ext.barSoundInterval > 0)
                                       ? (DWORD)g_bf1Ext.barSoundInterval * 1000u
                                       : 5000u;
                s_nextBarSoundMs = now + intervalMs;
                bf1_play_sound(g_bf1Ext.barSoundHash);
            }
        }

        // Background — full-screen, opaque.
        {
            const auto& bg = g_bf1Ext.planets[bgLevel];
            if (bg.texHash) {
                g_prt(bg.texHash, 0.0f, 0.0f, 1.0f, 1.0f,
                      g_color_ptr, 0, 0,0,1,1, 1,1,0,0);
            }
        }

        // Foreground — Phase C only.
        if (fgLevel >= 0) {
            const auto& fg = g_bf1Ext.planets[fgLevel];
            if (fg.texHash) {
                g_prt(fg.texHash, zx0, zy0, zx1, zy1,
                      g_color_ptr, 0, 0,0,1,1, 1,1,0,0);
            }
        }

        // ZoomSelector 16-quad crosshair
        if (g_bf1Ext.zoomSelCount > 0 && !animDone) {
            const float hw = g_bf1Ext.zoomTileHalfW > 0.0f ? g_bf1Ext.zoomTileHalfW : 0.02f;
            const float hh = g_bf1Ext.zoomTileHalfH > 0.0f ? g_bf1Ext.zoomTileHalfH : hw;

            const uint32_t zH = g_bf1Ext.zoomSelHashes[0];
            const uint32_t zV = g_bf1Ext.zoomSelCount > 1 ? g_bf1Ext.zoomSelHashes[1] : zH;
            const uint32_t zC = g_bf1Ext.zoomSelCount > 2 ? g_bf1Ext.zoomSelHashes[2] : zH;

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
    if (g_bf1Ext.scanLineTexHash)
        g_prt(g_bf1Ext.scanLineTexHash,
              0, 0, kW, kH, g_color_ptr, 0,
              0,0,1,1,  1,1,0,0);

    // Restore the heap.
    if (prevHeap >= 0 && g_set_current_heap)
        g_set_current_heap(prevHeap);
}
