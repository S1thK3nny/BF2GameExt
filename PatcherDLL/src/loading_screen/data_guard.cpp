#include "pch.h"
#include "shared.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"

#include <detours.h>

// =============================================================================
// LoadDisplay::LoadDataChunk bounds guard
// =============================================================================
// The stock loop appends every 'modl' / 'tex_' / 'skel' chunk it finds to three
// fixed-size arrays with no bounds check on any build (m_models[10],
// m_textures[50], m_skeletons[10] — offsets in load_display.hpp).
//
// The arrays are contiguous, so an overflow runs models into textures, textures
// into skeletons, and skeletons into m_numModels itself.  Once a count field is
// clobbered, LoadDisplay::DeleteData iterates a garbage m_numTextures calling
// ~RedTexture + operator delete on non-objects.
//
// A modder reaches this just by adding art to load.lvl, and the failure surfaces
// far from the cause.  We replace the loop with an identical one that checks the
// caps and logs what it dropped.
//
// Reimplemented rather than post-clamped because the writes are what corrupt
// memory; there is nothing to repair after the fact.

namespace {

using namespace load_display;

// Four-CC chunk ids, little-endian as compared by the engine.
constexpr uint32_t kChunkModl = 0x6c646f6d;  // "modl"
constexpr uint32_t kChunkTex  = 0x5f786574;  // "tex_"
constexpr uint32_t kChunkLoad = 0x64616f6c;  // "load"
constexpr uint32_t kChunkSkel = 0x6c656b73;  // "skel"

// PblFileChunk is 20 bytes; only these three fields are used by the loop.
struct PblFileChunk {
    uint32_t iId;      // +0x00
    uint32_t _pad;     // +0x04
    int32_t  iEnd;     // +0x08
    int32_t  iNext;    // +0x0c
    uint32_t _tail;    // +0x10
};
static_assert(sizeof(PblFileChunk) == 0x14, "PblFileChunk must be 20 bytes");

// The engine's own "are there more children?" test, aligned up to 4.
inline bool chunk_has_more(const PblFileChunk* c) {
    const int32_t aligned = (int32_t)(((0u - (uint32_t)c->iNext) & 3u) + (uint32_t)c->iNext);
    return aligned < c->iEnd;
}

typedef void  (__fastcall* fn_read_next_child_t)(void* ecx, void* edx, PblFileChunk* dst);
typedef void* (__cdecl*    fn_chunk_read_t)     (PblFileChunk* chunk);
typedef void  (__fastcall* fn_load_data_chunk_t)(void* ecx, void* edx, PblFileChunk* chunk);
typedef void  (__fastcall* fn_load_config_entry_t)(void* ecx, void* edx, PblFileChunk* chunk);

fn_read_next_child_t   g_read_next_child   = nullptr;
fn_chunk_read_t        g_read_model        = nullptr;
fn_chunk_read_t        g_read_texture      = nullptr;
fn_chunk_read_t        g_read_skeleton     = nullptr;
// Raw VA of LoadConfig, NOT the Detours trampoline.  Calling the entry point
// keeps hooked_load_config in the chain exactly as the vanilla path does.
fn_load_config_entry_t g_load_config_entry = nullptr;

fn_load_data_chunk_t   g_orig_load_data_chunk = nullptr;

// Per-file drop tallies, reported once at the end of each LoadDataChunk call.
int g_droppedModels    = 0;
int g_droppedTextures  = 0;
int g_droppedSkeletons = 0;

void __fastcall hooked_load_data_chunk(void* ecx, void* edx, PblFileChunk* chunk)
{
    (void)edx;

    if (!ecx || !chunk || !g_read_next_child) {
        if (g_orig_load_data_chunk) g_orig_load_data_chunk(ecx, edx, chunk);
        return;
    }

    g_droppedModels = g_droppedTextures = g_droppedSkeletons = 0;

    int* const numModels    = at<int>(ecx, kNumModels);
    int* const numTextures  = at<int>(ecx, kNumTextures);
    int* const numSkeletons = at<int>(ecx, kNumSkeletons);

    void** const models    = at<void*>(ecx, kModels);
    void** const textures  = at<void*>(ecx, kTextures);
    void** const skeletons = at<void*>(ecx, kSkeletons);

    while (chunk_has_more(chunk)) {
        PblFileChunk child = {};
        g_read_next_child(chunk, nullptr, &child);

        switch (child.iId) {
        case kChunkModl: {
            if (*numModels >= kMaxModels) { ++g_droppedModels; break; }
            if (!g_read_model) break;
            models[(*numModels)++] = g_read_model(&child);
            break;
        }
        case kChunkTex: {
            if (!g_read_texture) break;
            void* tex = g_read_texture(&child);
            // Engine-identical dedup: skip if this exact pointer is already held.
            bool dup = false;
            for (int i = 0; i < *numTextures; ++i) {
                if (textures[i] == tex) { dup = true; break; }
            }
            if (dup) break;
            if (*numTextures >= kMaxTextures) { ++g_droppedTextures; break; }
            textures[(*numTextures)++] = tex;
            break;
        }
        case kChunkSkel: {
            if (*numSkeletons >= kMaxSkeletons) { ++g_droppedSkeletons; break; }
            if (!g_read_skeleton) break;
            skeletons[(*numSkeletons)++] = g_read_skeleton(&child);
            break;
        }
        case kChunkLoad: {
            if (g_load_config_entry) g_load_config_entry(ecx, nullptr, &child);
            break;
        }
        default:
            break;
        }
    }

    if (g_droppedModels || g_droppedTextures || g_droppedSkeletons) {
        warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
               "[LoadDisplay] load lvl exceeds LoadDisplay capacity - dropped "
               "%d model(s), %d texture(s), %d skeleton(s). Caps are %d/%d/%d. "
               "Without this guard the overflow would corrupt the LoadDisplay object.\n",
               g_droppedModels, g_droppedTextures, g_droppedSkeletons,
               kMaxModels, kMaxTextures, kMaxSkeletons);
    }
}

} // namespace

// =============================================================================
// Install / Uninstall
// =============================================================================

void load_data_guard_install(uintptr_t exe_base)
{
    const GameAddrs& a = *g_addr;
    auto at = [exe_base](uintptr_t va) -> void* {
        return va ? resolve(exe_base, va) : nullptr;
    };

    g_read_next_child   = (fn_read_next_child_t)  at(a.pbl_chunk_read_next_child);
    g_read_model        = (fn_chunk_read_t)       at(a.red_model_read);
    g_read_texture      = (fn_chunk_read_t)       at(a.red_texture_read);
    g_read_skeleton     = (fn_chunk_read_t)       at(a.red_skeleton_read);
    g_load_config_entry = (fn_load_config_entry_t)at(a.load_config_real);

    // Reimplementing the loop means every callee must be present; if any one is
    // missing, leave the stock function alone rather than half-processing files.
    // The complaint is deferred rather than logged here: install runs from
    // dllmain while the exe sections are still non-executable, so calling the
    // game's logger would fault (see lua_hooks_install).
    if (!g_read_next_child || !g_read_model || !g_read_texture
        || !g_read_skeleton || !g_load_config_entry) {
        g_guardUnresolved = true;
        return;
    }

    g_orig_load_data_chunk = (fn_load_data_chunk_t)at(a.load_data_chunk_real);
    if (!g_orig_load_data_chunk) {
        g_guardUnresolved = true;
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_orig_load_data_chunk, hooked_load_data_chunk);
    DetourTransactionCommit();
}

void load_data_guard_uninstall()
{
    if (!g_orig_load_data_chunk) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_orig_load_data_chunk, hooked_load_data_chunk);
    DetourTransactionCommit();
}
