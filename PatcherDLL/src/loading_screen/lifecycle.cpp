#include "pch.h"
#include "shared.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/lvl_read.hpp"
#include "lua/lua_hooks.hpp"   // g_loadDisplayPath

#include <detours.h>

// =============================================================================
// Tracking sound state (file-local — only accessed by sound functions below)
// =============================================================================

static GameSoundControllable g_trackCtrl   = {};
static bool                  g_trackActive = false;
static void*                 g_trackVoice  = nullptr;

// End-hook state (file-local)
static bool g_inRealEnd = false;

// Set once the detour transaction has actually committed, so uninstall does not
// try to detach hooks that install bailed out before attaching.
static bool g_installed = false;

// The imm32 operand of the instruction that hands LoadDataFile its path on the
// release builds; lua_hooks_install has repointed it at g_loadDisplayPath.
// Read-only here — see path_buffer_live.
static const uint32_t* g_pathOperand = nullptr;

// =============================================================================
// Sound helpers
// =============================================================================

// One-shot sounds are owned rather than fired and forgotten, so that
// loading_screen_stop_all_sounds() can silence them when the screen ends. Without
// this a ZoomSound or BarSound started late keeps playing into the match — most
// visibly when the player alt-tabs and the load stalls mid-sequence.
static constexpr int kMaxOneShots = 16;
struct OwnedSound {
    GameSoundControllable ctrl;
    void*                 vv;   // VoiceVirtual* as returned by Play
};
static OwnedSound g_oneShots[kMaxOneShots] = {};
static int        g_oneShotNext = 0;

// Retire one owned sound.
//
// The gentle path clears the VoiceVirtual's looping flag and then releases
// ownership without stopping anything. Snd::VoiceVirtual::Update notices the flag
// is clear once the play position passes the end of the sample and calls Stop
// itself, so the sound finishes the pass it is already playing instead of being
// cut off mid-waveform, and the FireForget callback installed by ReleaseVoice
// frees it afterwards. See the note on kVoiceVirtual_LoopByte in shared.hpp.
//
// hardStop instead runs GameSoundControllable::Stop(true) -> Snd::VoiceVirtual::Stop,
// which silences the voice on the spot. Only for cases where a sound genuinely
// must be quiet this instant.
static void snd_ctrl_retire(GameSoundControllable* ctrl, void* vv, bool hardStop = false)
{
    if (!ctrl) return;

    // Staleness guard: the pointer must still map back to the handle we hold,
    // otherwise the voice was recycled and vv now belongs to somebody else.
    if (!hardStop && vv && ctrl->mVoiceVirtualHandle != 0 && g_voice_to_handle
        && (uint16_t)g_voice_to_handle(vv) == ctrl->mVoiceVirtualHandle) {
        uint8_t* loop = (uint8_t*)vv + kVoiceVirtual_LoopByte;
        *loop &= (uint8_t)~kVoiceVirtual_LoopBit;
    }

    if (ctrl->mVoiceVirtualHandle != 0 && g_snd_ctrl_stop)
        g_snd_ctrl_stop(ctrl, nullptr, hardStop ? 1 : 0);

    memset(ctrl, 0, sizeof(*ctrl));
}

// Stop the current tracking sound, if any is playing.
void tracking_sound_stop()
{
    snd_ctrl_retire(&g_trackCtrl, g_trackVoice);
    g_trackVoice  = nullptr;
    g_trackActive = false;
}

// Release everything this module started. Called on the way out of the loading
// screen, on every path, so no loop can survive into the match.
void loading_screen_stop_all_sounds()
{
    tracking_sound_stop();

    for (int i = 0; i < kMaxOneShots; ++i) {
        snd_ctrl_retire(&g_oneShots[i].ctrl, g_oneShots[i].vv);
        g_oneShots[i].vv = nullptr;
    }
    g_oneShotNext = 0;
}

// Start a looping/controllable tracking sound.
// Stops any currently playing tracking sound first.
void tracking_sound_start(uint32_t hash)
{
    tracking_sound_stop();
    if (!g_snd_play_ex || !g_find_by_hash || !hash) return;
    void* props = g_find_by_hash(hash);
    if (!props) {
        warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
                     "[BF1Ext] tracking_sound_start - sound hash %08x not found\n",
                     hash);
        return;
    }

    memset(&g_trackCtrl, 0, sizeof(g_trackCtrl));

    const uint8_t loopBit      = (*(const uint8_t*)((const uint8_t*)props + g_propsLoopByte)) & 0x10;
    g_trackCtrl.mFlags         = (uint8_t)((loopBit | 8u) >> 3);

    // Reset nextAllowedTime cooldown so Play always proceeds.
    *(float*)((uint8_t*)props + g_propsNextAllowed) = 0.0f;

    void* voice = g_snd_play_ex(nullptr, props, g_snd_stolen_callback, &g_trackCtrl, 0);
    if (voice && g_voice_to_handle)
        g_trackCtrl.mVoiceVirtualHandle = (uint16_t)g_voice_to_handle(voice);

    g_trackVoice = voice;
    g_trackActive = true;
}

// Play a one-shot sound by its hash, keeping ownership so it can be stopped.
void loading_screen_play_sound(uint32_t sound_hash)
{
    if (!g_find_by_hash || !g_snd_play_ex || !sound_hash) return;
    void* props = g_find_by_hash(sound_hash);
    if (!props) {
        warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
                     "[BF1Ext] loading_screen_play_sound - sound hash %08x not found\n",
                     sound_hash);
        return;
    }

    // Recycle the oldest slot. Retiring first matters: reusing a slot whose handle
    // is still live would orphan that voice with no way to reach it. Retiring is
    // soft, so an older one-shot still playing is left to finish.
    OwnedSound* slot = &g_oneShots[g_oneShotNext];
    g_oneShotNext = (g_oneShotNext + 1) % kMaxOneShots;
    snd_ctrl_retire(&slot->ctrl, slot->vv);
    slot->vv = nullptr;

    // Reset nextAllowedTime cooldown so Play always proceeds.
    *(float*)((uint8_t*)props + g_propsNextAllowed) = 0.0f;

    void* voice = g_snd_play_ex(nullptr, props, g_snd_stolen_callback, &slot->ctrl, 0);
    if (voice && g_voice_to_handle)
        slot->ctrl.mVoiceVirtualHandle = (uint16_t)g_voice_to_handle(voice);
    slot->vv = voice;
}

// =============================================================================
// LoadDataFile hook
// =============================================================================

// Should the BF1-ext sound LVL be pulled in on this call?  Once per loading
// screen, and only when BF1 mode named one.
static bool want_sound_lvl()
{
    if (!g_loadScreenCfg.bf1Enabled) return false;
    if (s_sndLvlLoaded) return false;

    if (!g_loadScreenCfg.loadSoundLvl[0]) {
        // Legitimate when the sounds already live in the loading screen's own
        // lvl, so this is informational - but naming sounds with no LoadSoundLVL
        // is also exactly what a missing or misspelled key looks like, and that
        // is otherwise invisible until the sounds report "not found".
        const bool namedSounds = g_loadScreenCfg.barSoundHash
                              || g_loadScreenCfg.xTrackSoundHash
                              || g_loadScreenCfg.yTrackSoundHash
                              || g_loadScreenCfg.zoomSoundHash
                              || g_loadScreenCfg.transitionSoundHash;
        if (namedSounds) {
            s_sndLvlLoaded = true;   // report once per loading screen
            warn_gamelog(RED_SEVERITY_INFO, SRC_FILE, __LINE__,
                   "[BF1Ext] This LoadConfig names sounds but has no LoadSoundLVL "
                   "entry, so they must already be in the loading screen's own "
                   "lvl. If they are not, they will report \"not found\".\n");
        }
        return false;
    }

    s_sndLvlLoaded  = true;
    s_lastAnimPhase = -1;
    s_lastAnimCycle = -1;
    return true;
}

// Is the instruction that supplies the path reading our buffer?  lua_hooks
// repoints that imm32 at g_loadDisplayPath for SetLoadDisplayLevel; if the patch
// was never applied it still holds the engine's own literal, and swapping the
// buffer would silently load the same lvl twice.
static bool path_buffer_live()
{
    return g_pathOperand && *g_pathOperand == (uint32_t)(uintptr_t)g_loadDisplayPath;
}

// One shape for all three builds: __thiscall(const char*), RET 4.
//
// The argument is only *live* on modtools, where the body passes it to
// LoadUtil::MakeFullName. The release builds inlined the "Load\\load" literal
// into the body and never read the slot — but they still declare it and still
// pop it, and LoadDisplay::LoadData depends on that: it leaves the argument of
// its preceding __cdecl RedSetCurrentHeap call on the stack and lets this
// function's RET 4 clean it up (steam 0x005773a5/0x005773c9/0x005773d9).
//
// So the argument is dead to the callee and load-bearing to the caller. See the
// note in shared.hpp for what hooking it with the wrong arity did.
void __fastcall hooked_load_data_file(void* ecx, void* edx, const char* lvlPath)
{
    g_orig_load_data_file(ecx, edx, lvlPath);

    if (!want_sound_lvl()) return;

    // Everything past here is reported.  The engine's own failure mode is
    // silence - LoadDataFile gates the open on PblFile::Exists and returns
    // without a word - and the symptom only surfaces much later as
    // "sound hash not found" from a completely different function.
    const char* const want = g_loadScreenCfg.loadSoundLvl;

    // LoadSoundLVL may carry the engine's ";group" selector, the way script-side
    // sound loading always writes it: ReadDataFile("sound\\bes.lvl;bes1cw").
    // ReadDataFileOnHeap splits on the ';' itself - everything before it is the
    // file, the rest is stashed where the chunk handlers pick it up - and a BF2
    // sound lvl generally needs that group, or it opens and registers nothing.
    // So the selector has to be kept out of the filename and out of the on-disk
    // check, and the extension has to go *before* it.
    char lvlPart[300];
    const char* group = nullptr;
    {
        const char* semi = strchr(want, ';');
        size_t n = semi ? (size_t)(semi - want) : strlen(want);
        if (n > sizeof(lvlPart) - 1) n = sizeof(lvlPart) - 1;
        memcpy(lvlPart, want, n);
        lvlPart[n] = '\0';
        if (semi && semi[1]) group = semi + 1;
    }

    // Both engine calls below resolve a bare name under data\_lvl_pc\ - MakeFullName
    // with fileType 0 (steam 0x005790a0) and ReadDataFileOnHeap (steam 0x00579ac1)
    // each prepend it - so the on-disk check needs that prefix and neither call
    // wants it. This is also the exact name the engine will look for.
    char full[512];
    _snprintf_s(full, sizeof(full), _TRUNCATE, "data\\_lvl_pc\\%s.lvl", lvlPart);
    const DWORD attrs = GetFileAttributesA(full);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
               "[BF1Ext] LoadSoundLVL \"%s\": \"%s\" does not exist, so no BF1 "
               "sound is loaded and every LoadConfig sound name will report "
               "\"not found\" later. Path is relative to the game's working "
               "directory.\n", want, full);
        return;
    }

    // Two independent reads of the same lvl, and neither is allowed to skip the
    // other - they deliver different things.
    //
    // 1. LoadDisplay::LoadDataFile, which is what this feature has always done.
    //    It is the only path that appends models, textures and skeletons to the
    //    LoadDisplay arrays, so any art in that lvl comes in here.
    //
    //    Retail cannot be told which lvl to load through the argument - the body
    //    ignores it and reads the inlined literal - so it goes through the buffer
    //    that literal's imm32 was repointed at.
    //    It gets lvlPart, never the selector: MakeFullName would paste a ';' into
    //    the middle of the filename.
    if (g_build == GameBuild::Modtools) {
        g_orig_load_data_file(ecx, edx, lvlPart);
    } else if (path_buffer_live()) {
        char saved[sizeof(g_loadDisplayPath)];
        memcpy(saved, g_loadDisplayPath, sizeof(saved));
        strncpy_s(g_loadDisplayPath, sizeof(g_loadDisplayPath), lvlPart, _TRUNCATE);
        g_orig_load_data_file(ecx, edx, lvlPath);   // path comes from the buffer
        memcpy(g_loadDisplayPath, saved, sizeof(saved));
    } else {
        warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
               "[BF1Ext] LoadSoundLVL \"%s\": this build takes the lvl path from "
               "an inlined literal and that operand was not repointed at our "
               "buffer (reads %08x, expected %08x), so any art in that lvl is "
               "skipped.\n", want,
               g_pathOperand ? *g_pathOperand : 0u,
               (uint32_t)(uintptr_t)g_loadDisplayPath);
    }

    // 2. The sounds, which the call above cannot deliver on any build: it walks
    //    the lvl with LoadDataChunk, and that dispatches only 'modl'/'tex_'/
    //    'skel'/'load'. Sound chunks need the engine's own reader - see
    //    core/lvl_read.hpp for the dispatch chain.
    //
    //    The name is the configured path with the extension and the selector, and
    //    no directory: ReadDataFileOnHeap prepends "data\_lvl_pc\" itself, so
    //    handing it `full` would ask for data\_lvl_pc\data\_lvl_pc\...
    char lvlName[512];
    if (group)
        _snprintf_s(lvlName, sizeof(lvlName), _TRUNCATE, "%s.lvl;%s", lvlPart, group);
    else
        _snprintf_s(lvlName, sizeof(lvlName), _TRUNCATE, "%s.lvl", lvlPart);

    if (!lvl_read_data_file(lvlName)) {
        warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
               "[BF1Ext] LoadSoundLVL \"%s\": LoadUtil::ReadDataFile could not "
               "open \"%s\" (resolved as \"%s\"), so no BF1 sound is "
               "registered.\n", want, lvlName, full);
        return;
    }

    // Reading the lvl and registering its properties are separate things, and
    // only the second is what the sound calls need. Probe one hash the config
    // actually named so a future regression says so here, at the cause, rather
    // than as "sound hash not found" from somewhere else entirely.
    const uint32_t probe = g_loadScreenCfg.barSoundHash    ? g_loadScreenCfg.barSoundHash
                         : g_loadScreenCfg.xTrackSoundHash ? g_loadScreenCfg.xTrackSoundHash
                         : g_loadScreenCfg.transitionSoundHash;
    if (probe && g_find_by_hash && !g_find_by_hash(probe)) {
        warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
               "[BF1Ext] LoadSoundLVL \"%s\" was read as \"%s\", but sound hash "
               "%08x from the same LoadConfig is still not registered - the lvl "
               "loaded without its sound properties.%s\n",
               want, lvlName, probe,
               group ? "" : " No sound group was given: a BF2 sound lvl is loaded"
                            " as \"<lvl>;<group>\", the way script-side"
                            " ReadDataFile(\"sound\\\\bes.lvl;bes1cw\") does, and"
                            " without the group the file opens but nothing is"
                            " registered. Try LoadSoundLVL = \"<path>;<group>\".");
    }
}

// =============================================================================
// LoadDisplay::Update hook — smooth asset-loading phase to ~30 fps
// =============================================================================

void __fastcall hooked_load_update(void* ecx, void* edx)
{
    if (g_inRealEnd) {
        return;
    }

    // BF1 mode: redirect s_loadHeap -> RunTimeHeap for the entire Update call.
    int saved_load_heap = -1;
    if (g_loadScreenCfg.bf1Enabled && g_s_load_heap_ptr && g_runtime_heap_idx) {
        saved_load_heap      = *g_s_load_heap_ptr;
        *g_s_load_heap_ptr   = *g_runtime_heap_idx;
    }

    const DWORD qpc_before = g_qpc_stamp ? *g_qpc_stamp : 0;
    g_orig_load_update(ecx, edx);

    if (saved_load_heap >= 0 && g_s_load_heap_ptr)
        *g_s_load_heap_ptr = saved_load_heap;

    // Tick the audio engine so queued voices are mixed to hardware.
    if (g_loadScreenCfg.bf1Enabled && g_snd_update) {
        const DWORD now = GetTickCount();
        const DWORD ms  = now - g_lastSndUpdateMs;
        if (ms > 0 && ms < 1000u) {
            g_snd_update((float)ms * 0.001f, 1);
        }
        g_lastSndUpdateMs = now;
    }

    if (!g_loadScreenCfg.bf1Enabled || !g_orig_load_render) return;

    if (g_qpc_stamp && *g_qpc_stamp != qpc_before) {
        g_lastRenderMs = GetTickCount();
    } else if (ecx && *load_display::at<uint8_t>(ecx, load_display::kBDisplay) != 0
               && GetTickCount() - g_lastRenderMs >= 33u) {
        g_lastRenderMs = GetTickCount();
        {
            int prevRenderHeap = -1;
            if (g_set_current_heap && g_runtime_heap_idx)
                prevRenderHeap = g_set_current_heap(*g_runtime_heap_idx);
            g_orig_load_render(ecx, nullptr);
            if (prevRenderHeap >= 0 && g_set_current_heap)
                g_set_current_heap(prevRenderHeap);
        }
    }
}

// =============================================================================
// LoadDisplay::End hook — delays teardown until animation completes
// =============================================================================

void __fastcall hooked_load_end(void* ecx, void* edx)
{
    // Before the g_endProcessed gate, not after: if End is reached twice, the
    // second call must still be able to silence anything left running.
    loading_screen_stop_all_sounds();

    if (g_endProcessed) return;

    if (g_loadScreenCfg.bf1Enabled && g_animStartMs != 0 && g_orig_load_update) {
        int nTrans = 0;
        for (int i = 0; i < g_loadScreenCfg.planetCount; ++i) {
            const auto& e = g_loadScreenCfg.planets[i];
            if (e.w > 0.0f && e.h > 0.0f) nTrans++;
            else break;
        }
        if (nTrans > 0) {
            const DWORD required = (DWORD)nTrans * kAnimCycleMs;
            const DWORD deadline = GetTickCount() + 30000u;

            if (g_orig_set_all_on && ecx)
                g_orig_set_all_on(load_display::at<uint8_t>(ecx, load_display::kProgressBar),
                                  nullptr);

            DWORD lastUpdateMs = 0;
            while (GetTickCount() - g_animStartMs < required) {
                if (GetTickCount() >= deadline) break;
                const DWORD now = GetTickCount();

                MSG msg;
                while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }

                if (now - lastUpdateMs >= 200u) {
                    lastUpdateMs = now;
                    const DWORD qpc_before = g_qpc_stamp ? *g_qpc_stamp : 0;
                    int saved_load_heap_end = -1;
                    if (g_s_load_heap_ptr && g_runtime_heap_idx) {
                        saved_load_heap_end    = *g_s_load_heap_ptr;
                        *g_s_load_heap_ptr     = *g_runtime_heap_idx;
                    }
                    g_orig_load_update(ecx, nullptr);
                    if (saved_load_heap_end >= 0 && g_s_load_heap_ptr)
                        *g_s_load_heap_ptr = saved_load_heap_end;
                    if (g_qpc_stamp && *g_qpc_stamp != qpc_before)
                        g_lastRenderMs = GetTickCount();

                    if (g_snd_update) {
                        const DWORD sndNow = GetTickCount();
                        const DWORD sndMs  = sndNow - g_lastSndUpdateMs;
                        if (sndMs > 0 && sndMs < 1000u)
                            g_snd_update((float)sndMs * 0.001f, 1);
                        g_lastSndUpdateMs = sndNow;
                    }
                }
                if (g_orig_load_render && GetTickCount() - g_lastRenderMs >= 33u) {
                    g_lastRenderMs = GetTickCount();
                    int prevRenderHeap = -1;
                    if (g_set_current_heap && g_runtime_heap_idx)
                        prevRenderHeap = g_set_current_heap(*g_runtime_heap_idx);
                    g_orig_load_render(ecx, nullptr);
                    if (prevRenderHeap >= 0 && g_set_current_heap)
                        g_set_current_heap(prevRenderHeap);
                }
                Sleep(1);
            }
        }
    }

    // Again after the spin loop, which plays sounds of its own as the animation
    // finishes.
    loading_screen_stop_all_sounds();

    g_endProcessed = true;
    g_inRealEnd = true;
    g_orig_load_end(ecx, edx);
    g_inRealEnd = false;
}

// =============================================================================
// Extension config key hashes
// =============================================================================
// Deferred out of loading_screen_install on purpose.  These need the engine's
// own PblHash::calcHash — a custom implementation would not be guaranteed to
// agree with the hashes the config munger stored — but install runs from
// dllmain while dllmain still has the exe sections mapped PAGE_READWRITE, i.e.
// non-executable.  Calling into .text there faults with an EXEC access
// violation under DEP; that is what crashed Steam at hash_string 0x00726e50 and
// GOG at 0x00727f20.  The first LoadConfig is long past that window.

void loading_screen_init_hashes()
{
    static bool s_done = false;
    if (s_done || !g_hash_string) return;
    s_done = true;

    kHash_ZoomSelectorTileSize = g_hash_string("ZoomSelectorTileSize");
    kHash_LoadSoundLVL         = g_hash_string("LoadSoundLVL");
    kHash_PC                   = g_hash_string("PC");
    kHash_PS2                  = g_hash_string("PS2");
    kHash_XBOX                 = g_hash_string("XBOX");
    kHash_RemoveToolTips       = g_hash_string("RemoveToolTips");
    kHash_RemoveLoadingBar     = g_hash_string("RemoveLoadingBar");
    kHash_RemoveLoadingText    = g_hash_string("RemoveLoadingText");
    kHash_RemoveMissionName    = g_hash_string("RemoveMissionName");
    kHash_RemoveModeName       = g_hash_string("RemoveModeName");
}

// =============================================================================
// PlatformRenderTexture — release calling convention
// =============================================================================
// modtools takes all fifteen arguments on the stack (__stdcall, RET 0x3c).  The
// release builds keep the same fifteen-argument signature but pass x0 and y0 in
// XMM2/XMM3 and only thirteen dwords on the stack (RET 0x34), and their body
// never reads the RedColor* or alphaBlend slots at all — LoadDisplay::
// RenderScreen doesn't even store them.  This thunk lets the renderer keep one
// call shape: it re-pushes the thirteen live dwords and lifts x0/y0 into the
// registers the release build expects.

static fn_prt_t g_prt_release = nullptr;

__declspec(naked) static void __stdcall prt_thunk_release(
    uint32_t, float, float, float, float, void*, int,
    float, float, float, float, float, float, float, float)
{
    __asm {
        push ebp
        mov  ebp, esp
        movss xmm2, dword ptr [ebp + 12]   // x0  (arg 2)
        movss xmm3, dword ptr [ebp + 16]   // y0  (arg 3)
        push dword ptr [ebp + 64]          // a
        push dword ptr [ebp + 60]          // b
        push dword ptr [ebp + 56]          // g
        push dword ptr [ebp + 52]          // r
        push dword ptr [ebp + 48]          // v1
        push dword ptr [ebp + 44]          // u1
        push dword ptr [ebp + 40]          // v0
        push dword ptr [ebp + 36]          // u0
        push dword ptr [ebp + 32]          // alphaBlend (ignored by the callee)
        push dword ptr [ebp + 28]          // RedColor*  (ignored by the callee)
        push dword ptr [ebp + 24]          // y1
        push dword ptr [ebp + 20]          // x1
        push dword ptr [ebp + 8]           // texture hash
        call dword ptr [g_prt_release]     // cleans its own 13 dwords (RET 0x34)
        mov  esp, ebp
        pop  ebp
        ret  60                            // our own 15 dwords
    }
}

// =============================================================================
// Install / Uninstall
// =============================================================================

void loading_screen_install(uintptr_t exe_base)
{
    // The startup crash that used to gate retail out of this module was a hook
    // arity error, not a heap or trampoline problem — see hooked_load_data_file.
    // The call the crash reports pointed at (LoadDisplay::Begin, returning to
    // 0x0053B5C3 in GameState::PreStateInit) reaches LoadDataFile through
    // LoadData, so the hook was on the startup path all along.

    const GameAddrs& a = *g_addr;

    // A zero in the table means "not reverse-engineered for this build" rather
    // than "does not exist", so resolve defensively and let the all-or-nothing
    // gate further down decide whether the module can run at all.
    auto at = [exe_base](uintptr_t va) -> void* {
        return va ? resolve(exe_base, va) : nullptr;
    };

    g_pbl_ctor       = (fn_pbl_ctor_t)      at(a.pbl_config_ctor);
    g_pbl_copy_ctor  = (fn_pbl_copy_ctor_t) at(a.pbl_config_copy_ctor);
    g_pbl_read_data  = (fn_pbl_read_data_t) at(a.pbl_read_next_data);
    g_pbl_read_scope = (fn_pbl_read_scope_t)at(a.pbl_read_next_scope);
    g_find_by_hash   = (fn_find_by_hash_t)  at(a.snd_find_by_hash_id);
    g_snd_play        = (fn_snd_play_t)        at(a.snd_sound_play);
    g_snd_play_ex     = (fn_snd_play_ex_t)     at(a.snd_sound_play);
    g_voice_to_handle = (fn_voice_to_handle_t) at(a.voice_to_handle);
    g_vvrelease       = (fn_vvrelease_t)       at(a.voice_virtual_release);
    g_snd_ctrl_stop   = (fn_snd_ctrl_stop_t)   at(a.gamesound_controllable_stop);
    g_snd_update      = (fn_snd_eng_update_t)  at(a.snd_engine_update);
    g_snd_stolen_callback =                    at(a.gamesound_stolen_callback);
    g_lastSndUpdateMs = GetTickCount();
    g_propsLoopByte    = a.snd_props_loop_byte    ? a.snd_props_loop_byte    : 0x1c;
    g_propsNextAllowed = a.snd_props_next_allowed ? a.snd_props_next_allowed : 0x68;

    // color_ptr_global is modtools-only; on release the argument is dead, so a
    // null here is expected and the renderer passes it through unused.
    g_color_ptr        = at(a.color_ptr_global);
    g_set_current_heap = (fn_set_current_heap_t) at(a.red_set_current_heap);
    g_runtime_heap_idx = (int*)                  at(a.runtime_heap_global);
    g_s_load_heap_ptr  = (int*)                  at(a.s_loadheap_global);

    if (g_build == GameBuild::Modtools) {
        g_prt = (fn_prt_t)at(a.platform_render_texture);
    } else {
        g_prt_release = (fn_prt_t)at(a.platform_render_texture);
        g_prt = g_prt_release ? (fn_prt_t)prt_thunk_release : nullptr;
    }

    g_hash_string = (fn_hash_string_t)at(a.hash_string);
    g_pbl_find    = (fn_pbl_find_t)   at(a.pbl_hash_table_find);
    g_tex_table   =                   at(a.tex_hash_table);

    // The extension's config keys are hashed lazily, NOT here — see
    // loading_screen_init_hashes().  PblHash::calcHash is game code, and install
    // runs from dllmain with the exe sections still mapped PAGE_READWRITE, so
    // calling it here dies with an EXEC access violation under DEP.

    g_orig_load_config    = (fn_load_config_t)   at(a.load_config_real);
    g_orig_render_screen  = (fn_render_screen_t) at(a.render_screen_real);
    g_orig_load_end       = (fn_load_end_t)      at(a.load_end_real);
    g_orig_set_all_on     = (fn_set_all_on_t)    at(a.progress_set_all_on);
    g_orig_load_update    = (fn_load_update_t)   at(a.load_update_real);
    g_orig_load_render    = (fn_load_render_t)   at(a.load_render_real);
    g_qpc_stamp           = (DWORD*)             at(a.load_update_qpc_stamp);

    g_orig_load_data_file = (fn_load_data_file_t)at(a.load_data_file_real);

    // Only the release builds need this: it is where their inlined path literal
    // comes from, and the LoadSoundLVL second load swaps its contents.
    if (g_build != GameBuild::Modtools)
        g_pathOperand = (const uint32_t*)at(a.enter_state_path_op);

    // Install all-or-nothing.  The four hook targets are what the module is
    // built on, and the PblConfig quartet plus the hasher are what makes the
    // extended config readable at all — a build missing any of them would
    // otherwise sit there logging a parse failure on every loading screen while
    // still holding live detours.  This is also the gate that keeps a
    // half-ported build out; GOG used to fail it on PblConfig::PblConfig alone,
    // and passes as of the 2026-07-28 port.
    //
    // No diagnostic here: install runs from dllmain with the exe sections still
    // non-executable, so calling into the game's logger would fault.
    if (!g_orig_load_data_file
        || !g_orig_load_config || !g_orig_render_screen
        || !g_orig_load_end  || !g_orig_load_update
        || !g_pbl_ctor || !g_pbl_copy_ctor || !g_pbl_read_data || !g_pbl_read_scope
        || !g_prt || !g_hash_string)
        return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_orig_load_data_file, hooked_load_data_file);
    DetourAttach(&(PVOID&)g_orig_load_config,   hooked_load_config);
    DetourAttach(&(PVOID&)g_orig_render_screen, hooked_render_screen);
    DetourAttach(&(PVOID&)g_orig_load_end,      hooked_load_end);
    DetourAttach(&(PVOID&)g_orig_load_update,   hooked_load_update);
    if (DetourTransactionCommit() != NO_ERROR) return;
    g_installed = true;

    // Must follow the LoadConfig detour: the guard calls LoadConfig's raw entry
    // point so hooked_load_config stays in the chain, exactly as vanilla does.
    load_data_guard_install(exe_base);
}

void loading_screen_uninstall()
{
    load_data_guard_uninstall();

    if (!g_installed) return;
    g_installed = false;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_orig_load_data_file, hooked_load_data_file);
    DetourDetach(&(PVOID&)g_orig_load_config,   hooked_load_config);
    DetourDetach(&(PVOID&)g_orig_render_screen, hooked_render_screen);
    DetourDetach(&(PVOID&)g_orig_load_end,      hooked_load_end);
    DetourDetach(&(PVOID&)g_orig_load_update,   hooked_load_update);
    DetourTransactionCommit();
}
