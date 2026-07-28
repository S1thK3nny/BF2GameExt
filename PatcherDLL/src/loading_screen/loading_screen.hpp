#pragma once

#include "pch.h"

// =============================================================================
// BF1 LoadDisplay extended parameters
// =============================================================================
// Populated by the LoadConfig hook (re-parses the same config file for
// BF1-only DATA chunks after BF2's original parse completes).
// Consumed by the RenderScreen hook and any Lua accessors.

struct LoadScreenConfig {
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
    // Screen rect in normalized 0-1; a rect needs BOTH w and h > 0, otherwise the
    // overlay falls back to full screen (0,0,1,1).
    //
    // Repeating the param adds another independent overlay rather than replacing
    // the previous one, so a config can run several animations at once with their
    // own frame set, rate and rect.  They draw in config order, so a later entry
    // layers on top of an earlier one.
    static constexpr int kMaxAnimFrames = 64;
    static constexpr int kMaxAnims      = 8;
    struct AnimEntry {
        uint32_t hashes[kMaxAnimFrames];
        int      count;            // frames in this overlay
        float    fps;
        float    x, y, w, h;       // normalized 0-1 screen rect
    };
    AnimEntry anims[kMaxAnims];
    int       animSlotCount;       // overlays configured, NOT a frame count

    // Sound hashes (play on specific loading screen events)
    uint32_t xTrackSoundHash;
    uint32_t yTrackSoundHash;
    uint32_t zoomSoundHash;
    uint32_t transitionSoundHash;
    uint32_t barSoundHash;
    int      barSoundInterval;   // seconds between BarSound replays (0 = play once)

    // LoadSoundLVL: lvl path containing the snd_ chunks for the above sounds, with
    // an optional ";group" selector. Stored raw; hooked_load_data_file splits the
    // selector off and hands the rest to lvl_resolve_data_path, so it accepts the
    // same forms as SetLoadDisplayLevel ("dc:" prefix, optional ".lvl" extension).
    // Loaded once on the first hooked_load_data_file call so that Properties are in
    // the game's sound hash table before the first render fires sounds.
    char     loadSoundLvl[260];

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

    // UI suppression flags — each hides exactly one element, independent of
    // EnableBF1.  See docs/user/LOADING_SCREEN.md.
    bool removeToolTips;     // m_groupLoadingTips (tips box + text)
    bool removeLoadingBar;   // m_progressBar only
    bool removeLoadingText;  // m_textLoading — the blinking "Loading" caption
    bool removeMissionName;  // m_textMissionName — top-left map name
    bool removeModeName;     // m_textModeName — top-right game mode name

    void reset() { memset(this, 0, sizeof(*this)); }
};

inline LoadScreenConfig g_loadScreenCfg = {};

// Call from lua_hooks_install() / lua_hooks_uninstall()
void loading_screen_install(uintptr_t exe_base);
void loading_screen_uninstall();
