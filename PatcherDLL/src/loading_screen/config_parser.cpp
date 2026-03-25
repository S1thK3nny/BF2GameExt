#include "pch.h"
#include "shared.hpp"

// =============================================================================
// Config file parsing — PblConfig helpers and LoadConfig hook
// =============================================================================

// Enter a sub-scope and drain all its DATA entries.
static void pbl_skip_next_scope(void* parent, uint8_t* tmp, uint32_t* scratch)
{
    memset(tmp, 0, 0x300);
    void* sr = g_pbl_read_scope(parent, nullptr, tmp);
    if (!sr) return;
    uint8_t child[0x300];
    memset(child, 0, sizeof(child));
    g_pbl_copy_ctor(child, nullptr, sr, 1);
    while (pbl_has_more(child))
        g_pbl_read_data(child, nullptr, scratch);
}

static void parse_bf1_entry(const uint32_t* data_buf);

// Parse a PlanetLevel DATA entry and append to g_bf1Ext.planets[].
static void parse_planet_level(const uint32_t* data_buf)
{
    const uint32_t argc = data_buf[1];
    if (argc < 2 || g_bf1Ext.planetCount >= Bf1LoadExt::kMaxPlanets) return;

    Bf1LoadExt::PlanetEntry& e = g_bf1Ext.planets[g_bf1Ext.planetCount++];
    e.levelIndex = pbl_get_int(data_buf, 0);
    const char* s = pbl_get_str(data_buf, 1);
    e.texHash = hash_name(s);
    if (!e.texHash) {
        auto fn_log = get_gamelog();
        fn_log("[BF1Ext] ERROR: PlanetLevel[%d] texture name '%s' could not be hashed\n",
               e.levelIndex, s ? s : "(null)");
    }
    e.x = (argc >= 3) ? pbl_get_float(data_buf, 2) : 0.0f;
    e.y = (argc >= 4) ? pbl_get_float(data_buf, 3) : 0.0f;
    e.w = (argc >= 5) ? pbl_get_float(data_buf, 4) : 0.0f;
    e.h = (argc >= 6) ? pbl_get_float(data_buf, 5) : 0.0f;
}

// Enter a sub-scope and parse its entries as BF1 params + PlanetLevel.
// Used for PC() inside LoadDisplay and for Map/World scopes.
static void pbl_parse_bf1_scope(void* parent, uint8_t* tmp, uint32_t* scratch)
{
    memset(tmp, 0, 0x300);
    void* sr = g_pbl_read_scope(parent, nullptr, tmp);
    if (!sr) return;
    uint8_t child[0x300];
    memset(child, 0, sizeof(child));
    g_pbl_copy_ctor(child, nullptr, sr, 1);

    while (pbl_has_more(child)) {
        memset(scratch, 0, 0x80 * sizeof(uint32_t));
        g_pbl_read_data(child, nullptr, scratch);

        if (scratch[0] == kHash_PlanetLevel)
            parse_planet_level(scratch);
        else
            parse_bf1_entry(scratch);
    }
}

// Parse one DATA entry as a known BF1 param and update g_bf1Ext.
// Called from both the LoadDisplay and Map loops so BF1 params work in either scope.
// PlanetLevel is NOT handled here — it has position args and is Map-only.
static void parse_bf1_entry(const uint32_t* data_buf)
{
    const uint32_t hash = data_buf[0];
    const uint32_t argc = data_buf[1];

    if (hash == kHash_EnableBF1 && argc >= 1) {
        g_bf1Ext.bf1Enabled = (pbl_get_int(data_buf, 0) != 0);
    }
    else if (hash == kHash_ScanLineTexture && argc >= 1) {
        const char* s = pbl_get_str(data_buf, 0);
        g_bf1Ext.scanLineTexHash = hash_name(s);
        if (!g_bf1Ext.scanLineTexHash) {
            auto fn_log = get_gamelog();
            fn_log("[BF1Ext] ERROR: ScanLineTexture name '%s' could not be hashed\n", s ? s : "(null)");
        }
        if (argc >= 2) g_bf1Ext.scanLineParams[0] = pbl_get_float(data_buf, 1);
        if (argc >= 3) g_bf1Ext.scanLineParams[1] = pbl_get_float(data_buf, 2);
        if (argc >= 4) g_bf1Ext.scanLineParams[2] = pbl_get_float(data_buf, 3);
    }
    else if (hash == kHash_ZoomSelectorTextures && argc >= 1) {
        const int n = (int)argc < Bf1LoadExt::kMaxZoomSel
                    ? (int)argc : Bf1LoadExt::kMaxZoomSel;
        for (int i = 0; i < n; ++i) {
            const char* s = pbl_get_str(data_buf, i);
            g_bf1Ext.zoomSelHashes[i] = hash_name(s);
            if (!g_bf1Ext.zoomSelHashes[i]) {
                auto fn_log = get_gamelog();
                fn_log("[BF1Ext] ERROR: ZoomSelectorTextures[%d] name '%s' could not be hashed\n", i, s ? s : "(null)");
            }
        }
        g_bf1Ext.zoomSelCount = n;
    }
    else if (hash == kHash_AnimatedTextures && argc >= 2) {
        const char* base = pbl_get_str(data_buf, 0);
        int         cnt  = pbl_get_int(data_buf, 1);
        const float fps  = (argc >= 3) ? pbl_get_float(data_buf, 2) : 10.0f;
        const float ax = (argc >= 4) ? pbl_get_float(data_buf, 3) : 0.0f;
        const float ay = (argc >= 5) ? pbl_get_float(data_buf, 4) : 0.0f;
        const float aw = (argc >= 6) ? pbl_get_float(data_buf, 5) : 0.0f;
        const float ah = (argc >= 7) ? pbl_get_float(data_buf, 6) : 0.0f;
        if (base && cnt > 0) {
            if (cnt > Bf1LoadExt::kMaxAnimFrames) cnt = Bf1LoadExt::kMaxAnimFrames;
            for (int i = 0; i < cnt; ++i) {
                char nm[256];
                snprintf(nm, sizeof(nm), "%s%d", base, i);
                g_bf1Ext.animHashes[i] = hash_name(nm);
            }
            g_bf1Ext.animCount = cnt;
            g_bf1Ext.animFPS   = fps;
            g_bf1Ext.animX = ax; g_bf1Ext.animY = ay;
            g_bf1Ext.animW = aw; g_bf1Ext.animH = ah;
        } else if (!base) {
            auto fn_log = get_gamelog();
            fn_log("[BF1Ext] ERROR: AnimatedTextures base name is null\n");
        }
    }
    else if (hash == kHash_XTrackingSound && argc >= 1) {
        g_bf1Ext.xTrackSoundHash = hash_name(pbl_get_str(data_buf, 0));
    }
    else if (hash == kHash_YTrackingSound && argc >= 1) {
        g_bf1Ext.yTrackSoundHash = hash_name(pbl_get_str(data_buf, 0));
    }
    else if (hash == kHash_ZoomSound && argc >= 1) {
        g_bf1Ext.zoomSoundHash = hash_name(pbl_get_str(data_buf, 0));
    }
    else if (hash == kHash_TransitionSound && argc >= 1) {
        g_bf1Ext.transitionSoundHash = hash_name(pbl_get_str(data_buf, 0));
    }
    else if (hash == kHash_BarSound && argc >= 1) {
        g_bf1Ext.barSoundHash = hash_name(pbl_get_str(data_buf, 0));
    }
    else if (hash == kHash_BarSoundInterval && argc >= 1) {
        g_bf1Ext.barSoundInterval = pbl_get_int(data_buf, 0);
    }
    else if (kHash_LoadSoundLVL && hash == kHash_LoadSoundLVL && argc >= 1) {
        const char* s = pbl_get_str(data_buf, 0);
        if (s) {
            strncpy_s(g_bf1Ext.loadSoundLvl, sizeof(g_bf1Ext.loadSoundLvl), s,
                      sizeof(g_bf1Ext.loadSoundLvl) - 1);
        }
    }
    else if (kHash_ZoomSelectorTileSize && hash == kHash_ZoomSelectorTileSize && argc >= 1) {
        g_bf1Ext.zoomTileHalfW = pbl_get_float(data_buf, 0);
        g_bf1Ext.zoomTileHalfH = (argc >= 2) ? pbl_get_float(data_buf, 1)
                                              : g_bf1Ext.zoomTileHalfW;
    }
    // Known-but-unimplemented / BF2-native params — silently ignored.
    else if (hash == kHash_TeamModel
          || hash == kHash_TeamModelRotationSpeed
          || hash == kHash_ProgressBarTotalTime) {
        // no action
    }
}

// =============================================================================
// LoadConfig hook
// =============================================================================
// Strategy: call original first (BF2 parses its own params into a private copy
// of the FileHandle). Because PblConfig copies fh internally, the fh pointer
// passed to us is NOT modified by the original. We can re-construct a fresh
// PblConfig from the same fh to parse the BF1-only DATA chunks.

void __fastcall hooked_load_config(void* ecx, void* edx, uint32_t* fh)
{
    // Snapshot the 5-uint FileHandle before the original call advances fh[3] (position).
    uint32_t fh_saved[5];
    if (fh) memcpy(fh_saved, fh, sizeof(fh_saved));

    g_orig_load_config(ecx, edx, fh);

    if (!g_pbl_ctor || !g_pbl_read_data || !g_pbl_read_scope || !g_pbl_copy_ctor || !fh) {
        auto fn_log = get_gamelog();
        fn_log("[BF1Ext] ERROR: LoadConfig — missing fn ptrs (ctor=%p rd=%p rs=%p cc=%p fh=%p)\n",
               (void*)g_pbl_ctor, (void*)g_pbl_read_data,
               (void*)g_pbl_read_scope, (void*)g_pbl_copy_ctor, (void*)fh);
        return;
    }

    // Restore so our second PblConfig ctor starts reading from the beginning.
    memcpy(fh, fh_saved, sizeof(fh_saved));

    g_bf1Ext.reset();
    g_animStartMs    = GetTickCount(); // restart animation from PlanetLevel 0 on each new match
    g_lastRenderMs   = 0;              // reset so first injected render fires immediately
    g_lastSndUpdateMs = GetTickCount(); // reset audio-tick timer so first deltaTime is ~0
    g_endProcessed   = false;          // re-arm End() hook for new loading screen
    s_sndLvlLoaded   = false;          // re-arm sound LVL loading for new loading screen
    s_lastAnimPhase  = -1;             // reset phase tracking for sound triggers
    s_lastAnimCycle  = -1;
    s_nextBarSoundMs = 0;
    tracking_sound_stop();             // stop any stale tracking sound from previous load

    // Current level hashes stored in the LoadDisplay object (ecx+4 = world hash, ecx+8 = map hash).
    const uint32_t lvlHash1 = *(const uint32_t*)((const uint8_t*)ecx + 4);
    const uint32_t lvlHash2 = *(const uint32_t*)((const uint8_t*)ecx + 8);

    // PblConfig stack buffers — 0x300 bytes each, matches the game's auStack_768 allocation.
    uint8_t  outer_buf[0x300];
    uint8_t  scope_buf[0x300];
    uint8_t  map_buf  [0x300];
    uint8_t  temp_buf [0x300];
    uint32_t root_data[0x80];
    uint32_t data_buf [0x80];

    memset(outer_buf, 0, sizeof(outer_buf));
    memset(scope_buf, 0, sizeof(scope_buf));
    memset(map_buf,   0, sizeof(map_buf));

    g_pbl_ctor(outer_buf, nullptr, fh);

    while (pbl_has_more(outer_buf)) {
        memset(root_data, 0, sizeof(root_data));
        g_pbl_read_data(outer_buf, nullptr, root_data);

        const uint32_t root_hash = root_data[0];
        const uint32_t root_argc = root_data[1];

        // LoadDisplay scope — BF1 texture / sound params (defaults for all maps)
        if (root_hash == kHash_LoadDisplay) {
            memset(temp_buf, 0, sizeof(temp_buf));
            void* sr = g_pbl_read_scope(outer_buf, nullptr, temp_buf);
            g_pbl_copy_ctor(scope_buf, nullptr, sr, 1);

            while (pbl_has_more(scope_buf)) {
                memset(data_buf, 0, sizeof(data_buf));
                g_pbl_read_data(scope_buf, nullptr, data_buf);

                const uint32_t hash = data_buf[0];

                // PC() sub-scope — enter and parse BF1 params + PlanetLevel
                if (kHash_PC && hash == kHash_PC) {
                    pbl_parse_bf1_scope(scope_buf, temp_buf, data_buf);
                    continue;
                }

                // Other platform / known sub-scopes — drain to keep reader in sync
                if (hash == kHash_LoadingTextColorPallete
                    || (kHash_PS2  && hash == kHash_PS2)
                    || (kHash_XBOX && hash == kHash_XBOX)) {
                    pbl_skip_next_scope(scope_buf, temp_buf, data_buf);
                    continue;
                }

                // PlanetLevel at LoadDisplay top level (outside any platform scope)
                if (hash == kHash_PlanetLevel) {
                    parse_planet_level(data_buf);
                    continue;
                }

                parse_bf1_entry(data_buf);
            }
        }

        // Map / World scope — per-map overrides (replaces LoadDisplay defaults where specified)
        else if (root_hash == kHash_Map || root_hash == kHash_World) {
            const char*    lvlName = (root_argc >= 1) ? pbl_get_str(root_data, 0) : nullptr;
            const uint32_t mHash   = lvlName ? hash_name(lvlName) : 0;
            const bool     match   = mHash && (mHash == lvlHash1 || mHash == lvlHash2);

            memset(temp_buf, 0, sizeof(temp_buf));
            void* mr = g_pbl_read_scope(outer_buf, nullptr, temp_buf);
            g_pbl_copy_ctor(map_buf, nullptr, mr, 1);

            // If this Map block has its own PlanetLevel entries, they replace
            // LoadDisplay's defaults entirely (not append).
            bool mapClearedPlanets = false;

            while (pbl_has_more(map_buf)) {
                memset(data_buf, 0, sizeof(data_buf));
                g_pbl_read_data(map_buf, nullptr, data_buf);

                if (!match) continue;

                if (data_buf[0] == kHash_PlanetLevel) {
                    if (!mapClearedPlanets) {
                        g_bf1Ext.planetCount = 0;
                        mapClearedPlanets = true;
                    }
                    parse_planet_level(data_buf);
                }
                else {
                    parse_bf1_entry(data_buf);
                }
            }
        }
        else {
            // Unknown root-level scope — no scope consumed.
        }
    }

    // Post-parse gate: clear BF1-only params when not enabled.
    if (!g_bf1Ext.bf1Enabled) {
        g_bf1Ext.xTrackSoundHash     = 0;
        g_bf1Ext.yTrackSoundHash     = 0;
        g_bf1Ext.zoomSoundHash       = 0;
        g_bf1Ext.transitionSoundHash = 0;
        g_bf1Ext.barSoundHash        = 0;
        g_bf1Ext.barSoundInterval    = 0;
        g_bf1Ext.zoomSelCount        = 0;
        g_bf1Ext.planetCount         = 0;
    }
}
