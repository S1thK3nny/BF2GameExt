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

// Resolve a TeamModel* key's slot argument.  Returns -1 and complains on
// anything that is not the string "1" or "2".
//
// Deliberately strict and deliberately loud.  Everything that can go wrong here
// goes wrong *quietly* otherwise: pbl_get_str returns null for a numeric
// argument (an unquoted 1, or the superseded faction-name signatures), and the
// old faction names all/rep/cis/imp look perfectly reasonable to a modder who
// has read any other BF2 documentation. Each of those gets its own message,
// because "TeamModel ignored" with no reason is the worst possible outcome for
// something whose only symptom is a model that does not appear.
static int parse_team_slot(const char* s, const char* key)
{
    auto fn_log = get_gamelog();

    if (!s) {
        fn_log("[BF1Ext] ERROR: %s's first argument must be the QUOTED string "
               "\"1\" or \"2\". A bare number is not a string to the config "
               "parser and cannot be read as one. Entry ignored.\n", key);
        return -1;
    }
    if ((s[0] == '1' || s[0] == '2') && s[1] == '\0')
        return s[0] - '1';

    // Name the faction case specifically - it is the single most likely typo,
    // and it used to work, so silence would look like a regression.
    static const char* const kOldNames[] = { "all", "rep", "cis", "imp" };
    for (const char* old : kOldNames) {
        int i = 0;
        for (; old[i] && (s[i] | 0x20) == old[i]; ++i) {}
        if (!old[i] && !s[i]) {
            fn_log("[BF1Ext] ERROR: %s takes a slot, not a faction: \"%s\" is no "
                   "longer accepted. Use \"1\" or \"2\" - they are just the two "
                   "on-screen positions, which is what makes this work on "
                   "non-English builds. Entry ignored.\n", key, s);
            return -1;
        }
    }

    fn_log("[BF1Ext] ERROR: %s's first argument must be \"1\" or \"2\", got "
           "\"%s\". Entry ignored.\n", key, s);
    return -1;
}

// Parse a PlanetLevel DATA entry and append to g_loadScreenCfg.planets[].
static void parse_planet_level(const uint32_t* data_buf)
{
    const uint32_t argc = data_buf[1];
    if (argc < 2 || g_loadScreenCfg.planetCount >= LoadScreenConfig::kMaxPlanets) return;

    LoadScreenConfig::PlanetEntry& e = g_loadScreenCfg.planets[g_loadScreenCfg.planetCount++];
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
        memset(scratch, 0, kPblDataDwords * sizeof(uint32_t));
        g_pbl_read_data(child, nullptr, scratch);

        if (scratch[0] == kHash_PlanetLevel)
            parse_planet_level(scratch);
        else
            parse_bf1_entry(scratch);
    }
}

// Parse one DATA entry as a known BF1 param and update g_loadScreenCfg.
// Called from both the LoadDisplay and Map loops so BF1 params work in either scope.
// PlanetLevel is NOT handled here — it has position args and is Map-only.
static void parse_bf1_entry(const uint32_t* data_buf)
{
    const uint32_t hash = data_buf[0];
    const uint32_t argc = data_buf[1];

    if (hash == kHash_EnableBF1 && argc >= 1) {
        g_loadScreenCfg.bf1Enabled = (pbl_get_int(data_buf, 0) != 0);
    }
    else if (hash == kHash_ScanLineTexture && argc >= 1) {
        const char* s = pbl_get_str(data_buf, 0);
        g_loadScreenCfg.scanLineTexHash = hash_name(s);
        if (!g_loadScreenCfg.scanLineTexHash) {
            auto fn_log = get_gamelog();
            fn_log("[BF1Ext] ERROR: ScanLineTexture name '%s' could not be hashed\n", s ? s : "(null)");
        }
        if (argc >= 2) g_loadScreenCfg.scanLineParams[0] = pbl_get_float(data_buf, 1);
        if (argc >= 3) g_loadScreenCfg.scanLineParams[1] = pbl_get_float(data_buf, 2);
        if (argc >= 4) g_loadScreenCfg.scanLineParams[2] = pbl_get_float(data_buf, 3);
    }
    else if (hash == kHash_ZoomSelectorTextures && argc >= 1) {
        const int n = (int)argc < LoadScreenConfig::kMaxZoomSel
                    ? (int)argc : LoadScreenConfig::kMaxZoomSel;
        for (int i = 0; i < n; ++i) {
            const char* s = pbl_get_str(data_buf, i);
            g_loadScreenCfg.zoomSelHashes[i] = hash_name(s);
            if (!g_loadScreenCfg.zoomSelHashes[i]) {
                auto fn_log = get_gamelog();
                fn_log("[BF1Ext] ERROR: ZoomSelectorTextures[%d] name '%s' could not be hashed\n", i, s ? s : "(null)");
            }
        }
        g_loadScreenCfg.zoomSelCount = n;
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
            // Each AnimatedTextures adds an overlay; it does NOT replace the
            // previous one.  Overlays draw in config order.
            if (g_loadScreenCfg.animSlotCount >= LoadScreenConfig::kMaxAnims) {
                auto fn_log = get_gamelog();
                fn_log("[BF1Ext] WARNING: AnimatedTextures '%s' ignored - already at the "
                       "%d overlay limit\n", base, LoadScreenConfig::kMaxAnims);
                return;
            }
            if (cnt > LoadScreenConfig::kMaxAnimFrames) {
                auto fn_log = get_gamelog();
                fn_log("[BF1Ext] WARNING: AnimatedTextures count %d exceeds the %d frame limit - "
                       "only %s0..%s%d will be used\n",
                       cnt, LoadScreenConfig::kMaxAnimFrames,
                       base, base, LoadScreenConfig::kMaxAnimFrames - 1);
                cnt = LoadScreenConfig::kMaxAnimFrames;
            }
            if (fps <= 0.0f) {
                auto fn_log = get_gamelog();
                fn_log("[BF1Ext] WARNING: AnimatedTextures '%s' fps %f is not positive - "
                       "the overlay will hold on frame 0\n", base, fps);
            }
            // x/y/w/h only place the overlay as a complete rect.  Giving one of
            // w/h without the other used to honour half the placement and draw
            // the rest off-screen, so it now falls back to full screen.
            if ((aw > 0.0f) != (ah > 0.0f)) {
                auto fn_log = get_gamelog();
                fn_log("[BF1Ext] WARNING: AnimatedTextures '%s' needs both w and h to place the "
                       "overlay (got w=%f h=%f) - drawing full screen instead\n", base, aw, ah);
            }

            LoadScreenConfig::AnimEntry& a =
                g_loadScreenCfg.anims[g_loadScreenCfg.animSlotCount++];
            for (int i = 0; i < cnt; ++i) {
                char nm[256];
                snprintf(nm, sizeof(nm), "%s%d", base, i);
                a.hashes[i] = hash_name(nm);
            }
            a.count = cnt;
            a.fps   = fps;
            a.x = ax; a.y = ay;
            a.w = aw; a.h = ah;
        } else if (!base) {
            auto fn_log = get_gamelog();
            fn_log("[BF1Ext] ERROR: AnimatedTextures base name is null\n");
        }
    }
    else if (hash == kHash_XTrackingSound && argc >= 1) {
        g_loadScreenCfg.xTrackSoundHash = hash_name(pbl_get_str(data_buf, 0));
    }
    else if (hash == kHash_YTrackingSound && argc >= 1) {
        g_loadScreenCfg.yTrackSoundHash = hash_name(pbl_get_str(data_buf, 0));
    }
    else if (hash == kHash_ZoomSound && argc >= 1) {
        g_loadScreenCfg.zoomSoundHash = hash_name(pbl_get_str(data_buf, 0));
    }
    else if (hash == kHash_TransitionSound && argc >= 1) {
        g_loadScreenCfg.transitionSoundHash = hash_name(pbl_get_str(data_buf, 0));
    }
    else if (hash == kHash_BarSound && argc >= 1) {
        g_loadScreenCfg.barSoundHash = hash_name(pbl_get_str(data_buf, 0));
    }
    else if (hash == kHash_BarSoundInterval && argc >= 1) {
        g_loadScreenCfg.barSoundInterval = pbl_get_int(data_buf, 0);
    }
    else if (kHash_LoadSoundLVL && hash == kHash_LoadSoundLVL && argc >= 1) {
        const char* s = pbl_get_str(data_buf, 0);
        if (s) {
            strncpy_s(g_loadScreenCfg.loadSoundLvl, sizeof(g_loadScreenCfg.loadSoundLvl), s,
                      sizeof(g_loadScreenCfg.loadSoundLvl) - 1);
        }
    }
    else if (kHash_ZoomSelectorTileSize && hash == kHash_ZoomSelectorTileSize && argc >= 1) {
        g_loadScreenCfg.zoomTileHalfW = pbl_get_float(data_buf, 0);
        g_loadScreenCfg.zoomTileHalfH = (argc >= 2) ? pbl_get_float(data_buf, 1)
                                              : g_loadScreenCfg.zoomTileHalfW;
    }
    else if (kHash_RemoveToolTips && hash == kHash_RemoveToolTips) {
        g_loadScreenCfg.removeToolTips = (argc >= 1) ? (pbl_get_int(data_buf, 0) != 0) : true;
    }
    else if (kHash_RemoveLoadingBar && hash == kHash_RemoveLoadingBar) {
        g_loadScreenCfg.removeLoadingBar = (argc >= 1) ? (pbl_get_int(data_buf, 0) != 0) : true;
    }
    else if (kHash_RemoveLoadingText && hash == kHash_RemoveLoadingText) {
        g_loadScreenCfg.removeLoadingText = (argc >= 1) ? (pbl_get_int(data_buf, 0) != 0) : true;
    }
    else if (kHash_RemoveMissionName && hash == kHash_RemoveMissionName) {
        g_loadScreenCfg.removeMissionName = (argc >= 1) ? (pbl_get_int(data_buf, 0) != 0) : true;
    }
    else if (kHash_RemoveModeName && hash == kHash_RemoveModeName) {
        g_loadScreenCfg.removeModeName = (argc >= 1) ? (pbl_get_int(data_buf, 0) != 0) : true;
    }
    // ---- Team models ------------------------------------------------------
    // All four keys are handled here rather than by the engine, so that they
    // work in Map() scope and address slots instead of localized factions.
    // See the LoadScreenConfig comment for why the stock key cannot be used.
    //
    // TeamModel("1"|"2", "modelName")
    else if (hash == kHash_TeamModel) {
        if (argc < 2) {
            auto fn_log = get_gamelog();
            fn_log("[BF1Ext] ERROR: TeamModel needs two arguments, "
                   "TeamModel(\"1\"|\"2\", \"modelName\") - got %u. Entry ignored.\n", argc);
        }
        else {
            const int slot = parse_team_slot(pbl_get_str(data_buf, 0), "TeamModel");
            if (slot >= 0) {
                const char* model = pbl_get_str(data_buf, 1);
                if (!model || !*model) {
                    auto fn_log = get_gamelog();
                    fn_log("[BF1Ext] ERROR: TeamModel(\"%d\", ...) second argument must be a "
                           "non-empty quoted model name. Entry ignored.\n", slot + 1);
                }
                else {
                    auto& e = g_loadScreenCfg.teamModel[slot];
                    strncpy_s(e.model, sizeof(e.model), model, sizeof(e.model) - 1);
                    e.hasModel = true;
                }
            }
        }
    }
    // TeamModelOffset("1"|"2", x, y) - top-left screen fractions.
    else if (kHash_TeamModelOffset && hash == kHash_TeamModelOffset) {
        if (argc < 3) {
            auto fn_log = get_gamelog();
            fn_log("[BF1Ext] ERROR: TeamModelOffset needs three arguments, "
                   "TeamModelOffset(\"1\"|\"2\", x, y) - got %u. Entry ignored.\n", argc);
        }
        else {
            const int slot = parse_team_slot(pbl_get_str(data_buf, 0), "TeamModelOffset");
            if (slot >= 0) {
                auto& e = g_loadScreenCfg.teamModel[slot];
                e.x         = pbl_get_float(data_buf, 1);
                e.y         = pbl_get_float(data_buf, 2);
                e.hasOffset = true;
            }
        }
    }
    // TeamModelScale(f) - shared by both models, absolute multiplier.
    else if (kHash_TeamModelScale && hash == kHash_TeamModelScale && argc >= 1) {
        g_loadScreenCfg.teamModelScale = pbl_get_float(data_buf, 0);
    }
    // TeamModelRotationSpeed(f) - ours, so it works per map.
    else if (hash == kHash_TeamModelRotationSpeed && argc >= 1) {
        g_loadScreenCfg.teamModelOmega    = pbl_get_float(data_buf, 0);
        g_loadScreenCfg.teamModelOmegaSet = true;
    }
    // Known-but-unimplemented / BF2-native params — silently ignored.
    else if (hash == kHash_ProgressBarTotalTime) {
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
    // First point at which calling into the engine is safe; the installer runs
    // too early to hash anything.
    loading_screen_init_hashes();

    // Snapshot the 5-uint FileHandle before the original call advances fh[3] (position).
    uint32_t fh_saved[5];
    if (fh) memcpy(fh_saved, fh, sizeof(fh_saved));

    g_orig_load_config(ecx, edx, fh);

    if (g_guardUnresolved) {
        g_guardUnresolved = false;
        warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
               "[LoadDisplay] LoadDataChunk bounds guard not installed - one of "
               "its callees is missing from this build's address table. Loading "
               "screens still work, but a load lvl with more than %d models, %d "
               "textures or %d skeletons will corrupt the LoadDisplay object.\n",
               load_display::kMaxModels, load_display::kMaxTextures,
               load_display::kMaxSkeletons);
    }

    if (!g_pbl_ctor || !g_pbl_read_data || !g_pbl_read_scope || !g_pbl_copy_ctor || !fh) {
        auto fn_log = get_gamelog();
        fn_log("[BF1Ext] ERROR: LoadConfig — missing fn ptrs (ctor=%p rd=%p rs=%p cc=%p fh=%p)\n",
               (void*)g_pbl_ctor, (void*)g_pbl_read_data,
               (void*)g_pbl_read_scope, (void*)g_pbl_copy_ctor, (void*)fh);
        return;
    }

    // Restore so our second PblConfig ctor starts reading from the beginning.
    memcpy(fh, fh_saved, sizeof(fh_saved));

    g_loadScreenCfg.reset();
    g_animStartMs    = GetTickCount(); // restart animation from PlanetLevel 0 on each new match
    g_lastRenderMs   = 0;              // reset so first injected render fires immediately
    g_lastSndUpdateMs = GetTickCount(); // reset audio-tick timer so first deltaTime is ~0
    g_endProcessed   = false;          // re-arm End() hook for new loading screen
    s_sndLvlLoaded   = false;          // re-arm sound LVL loading for new loading screen
    s_lastAnimPhase  = -1;             // reset phase tracking for sound triggers
    s_lastAnimCycle  = -1;
    s_nextBarSoundMs = 0;
    s_teamModelsLogged = false;        // re-arm the team icon diagnostic for new loading screen
    loading_screen_stop_all_sounds();  // clear anything still playing from a previous load

    // Current level hashes stored in the LoadDisplay object (ecx+4 = world hash, ecx+8 = map hash).
    const uint32_t lvlHash1 = *(const uint32_t*)((const uint8_t*)ecx + 4);
    const uint32_t lvlHash2 = *(const uint32_t*)((const uint8_t*)ecx + 8);

    // PblConfig objects — 0x300 bytes each; the object itself only reaches +0x2c.
    uint8_t  outer_buf[0x300];
    uint8_t  scope_buf[0x300];
    uint8_t  map_buf  [0x300];
    uint8_t  temp_buf [0x300];
    // ReadNextData output. Must be kPblDataDwords — see the note in shared.hpp;
    // these are NOT free to shrink, and an overrun here lands on the PblConfig
    // objects above.
    uint32_t root_data[kPblDataDwords];
    uint32_t data_buf [kPblDataDwords];

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
                        g_loadScreenCfg.planetCount = 0;
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

    // ---- Team models: bind and claim the two slots -------------------------
    // Must happen here rather than in the Update hook.  LoadDisplay::Begin runs
    // SetLoadState -> LoadData (which is what called us) -> PostLoad, and
    // PostLoad is what parents the icons to m_groupBottomRight, sets their spin
    // and positions them - reading m_team1Num/m_team2Num to decide *which* slots
    // to do that to.  Writing them now means PostLoad does all of that work for
    // us natively; a slot left at -1 is not merely hidden, it is never added to
    // the screen group at all and nothing later can bring it back.
    //
    // Both scopes have been parsed by this point, so a Map() override has
    // already replaced the LoadDisplay() default.
    {
        int teamNum[LoadScreenConfig::kNumTeamModels] = { -1, -1 };
        bool any = false;

        for (int i = 0; i < LoadScreenConfig::kNumTeamModels; ++i) {
            const auto& e = g_loadScreenCfg.teamModel[i];
            if (!e.hasModel) continue;
            any = true;

            // Slot i of m_modelTeamIcon is just an array index once we stop
            // treating the array as faction-indexed.
            if (g_set_model) {
                g_set_model((uint8_t*)ecx + load_display::team_icon(i), nullptr, e.model);
                teamNum[i] = i;
            }
        }

        if (any && !g_set_model) {
            auto fn_log = get_gamelog();
            fn_log("[BF1Ext] ERROR: TeamModel configured but Red3DModelElementLite::SetModel "
                   "is not mapped for this build - the models cannot be bound.\n");
        }

        if (any) {
            *load_display::at<int>(ecx, load_display::kTeam1Num) = teamNum[0];
            *load_display::at<int>(ecx, load_display::kTeam2Num) = teamNum[1];

            if (g_loadScreenCfg.teamModelOmegaSet)
                *load_display::at<float>(ecx, load_display::kTeamModelOmega) =
                    g_loadScreenCfg.teamModelOmega;
        }
    }

    // Post-parse gate: clear BF1-only params when not enabled.
    if (!g_loadScreenCfg.bf1Enabled) {
        g_loadScreenCfg.xTrackSoundHash     = 0;
        g_loadScreenCfg.yTrackSoundHash     = 0;
        g_loadScreenCfg.zoomSoundHash       = 0;
        g_loadScreenCfg.transitionSoundHash = 0;
        g_loadScreenCfg.barSoundHash        = 0;
        g_loadScreenCfg.barSoundInterval    = 0;
        g_loadScreenCfg.zoomSelCount        = 0;
        g_loadScreenCfg.planetCount         = 0;
    }
}
