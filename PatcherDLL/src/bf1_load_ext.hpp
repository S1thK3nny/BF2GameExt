#pragma once

#include "pch.h"

// =============================================================================
// BF1 LoadDisplay extended parameters
// =============================================================================
// Populated by the LoadConfig hook (re-parses the same config file for
// BF1-only DATA chunks after BF2's original parse completes).
// Consumed by the RenderScreen hook and any Lua accessors.

struct Bf1LoadExt {
    // EnableBF1(1/0) — master switch; if false, BF1 sequence is skipped entirely.
    // Default false so omitting the param keeps vanilla BF2 behaviour.
    bool bf1Enabled;

    // ScanLineTexture: texName, f1, f2, f3
    uint32_t scanLineTexHash;
    float    scanLineParams[3];

    // ZoomSelectorTextures: up to 3 texture name args.
    // [0] = horizontal strips, [1] = vertical strips, [2] = corner pieces.
    // Draws the BF1-accurate 16-quad crosshair frame around the planet rect
    // when at least one PlanetLevel entry is configured.
    static constexpr int kMaxZoomSel = 3;
    uint32_t zoomSelHashes[kMaxZoomSel];
    int      zoomSelCount;
    // ZoomSelectorTileSize(half_w[, half_h]): half-dimensions of each strip tile
    // in normalized 0-1 screen space.  Default 0.02 when not set.
    float zoomTileHalfW, zoomTileHalfH;

    // AnimatedTextures: baseName, count, fps[, x, y, w, h]
    // Frames are named <baseName>0 .. <baseName>(count-1).
    // Screen rect in normalized 0-1; w==0 → full screen (0,0,1,1).
    static constexpr int kMaxAnimFrames = 64;
    uint32_t animHashes[kMaxAnimFrames];
    int      animCount;
    float    animFPS;
    float    animX, animY, animW, animH; // optional rect; w==0 means full-screen

    // PlanetBackdrops: up to 2 texture names
    static constexpr int kMaxBackdrops = 2;
    uint32_t backdropHashes[kMaxBackdrops];
    int      backdropCount;

    // Sound hashes (play on specific loading screen events)
    uint32_t xTrackSoundHash;
    uint32_t yTrackSoundHash;
    uint32_t zoomSoundHash;
    uint32_t transitionSoundHash;
    uint32_t barSoundHash;
    int      barSoundInterval;   // ms between BarSound plays

    // PlanetLevel: per-level planet texture with a direct 0-1 screen rect.
    // Syntax: PlanetLevel(levelIndex, texName, x, y, w, h)
    // x/y/w/h are in normalized 0-1 screen space — no image-size conversion needed.
    // The first entry also drives the ZoomSelector crosshair centre.
    static constexpr int kMaxPlanets = 32;
    struct PlanetEntry {
        int      levelIndex;
        uint32_t texHash;
        float    x, y, w, h;  // normalized 0-1 screen rect
    };
    PlanetEntry planets[kMaxPlanets];
    int         planetCount;

    void reset() { memset(this, 0, sizeof(*this)); }
};

extern Bf1LoadExt g_bf1Ext;

// Call from lua_hooks_install() / lua_hooks_uninstall()
void bf1_load_ext_install(uintptr_t exe_base);
void bf1_load_ext_uninstall();
