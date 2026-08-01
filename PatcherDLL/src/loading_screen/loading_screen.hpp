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

    // ---- Team models ------------------------------------------------------
    // The two spinning 3D models on the loading screen. BF2 has the feature
    // fully built and switched off; the extension turns it back on.
    //
    // **The extension owns these keys outright - the engine's own TeamModel is
    // not used at all.** Two reasons, both fatal to the stock key:
    //
    //   1. It only exists in the engine's LoadDisplay() branch (the Map()/World()
    //      branch accepts seven keys and TeamModel is not among them), so models
    //      could never vary per map.
    //   2. It selects a slot by *faction*, and which two factions get shown is
    //      decided by single-letter team names that are LOCALIZED. A German
    //      build sends "k" for the CIS (KUS) against four hardcoded constants of
    //      "a"/"r"/"c"/"i", so the second model silently never appears.
    //
    // So slots are addressed as "1" and "2" - just the two on-screen positions,
    // no factions, nothing localized. The apply site binds the models itself and
    // writes m_team1Num/m_team2Num to the fixed slots 0 and 1, after which
    // PostLoad does the parenting, positioning and counter-rotation natively.
    //
    // Offsets are fractions of screen width/height, NOT pixels: the element
    // coordinate space is device pixels (RedInterfaceScreen::Render builds the
    // frustum so one world unit is one pixel of m_loadingScreen, whose
    // dimensions come from s_screenFull), so a pixel offset would drift across
    // resolutions. Measured from the TOP LEFT, +x right and +y down, matching
    // AnimatedTextures and PlanetLevel. The element space is anchored at the
    // bottom-right safe-zone corner, so the apply site rebases:
    // elem = (frac - anchorRel) * screenSize on both axes.
    static constexpr int kNumTeamModels = 2;
    struct TeamModelEntry {
        char  model[64];         // name as written; bound via SetModel
        float x, y;              // top-left screen fraction
        bool  hasModel;
        bool  hasOffset;         // false = leave PostLoad's own placement alone
    };
    TeamModelEntry teamModel[kNumTeamModels];

    // Absolute multiplier on m_scale, shared by both models - deliberately not
    // per-slot, since two models on one loading screen realistically want the
    // same size and a per-slot key would just be a duplicated line. NOT a screen
    // fraction: one world unit is one device pixel, so a fixed scale holds its
    // pixel size and shrinks relative to the screen as resolution rises.
    float teamModelScale;        // 0 = leave the engine's 1.0

    // Spin rate, written to m_teamModelOmega. PostLoad applies +omega to slot 0
    // and -omega to slot 1, so they counter-rotate. Ours rather than the
    // engine's key so that it works in Map() scope like everything else here.
    float teamModelOmega;
    bool  teamModelOmegaSet;     // false = leave Begin's default 0.6

    void reset() { memset(this, 0, sizeof(*this)); }
};

inline LoadScreenConfig g_loadScreenCfg = {};

// Call from lua_hooks_install() / lua_hooks_uninstall()
void loading_screen_install(uintptr_t exe_base);
void loading_screen_uninstall();
