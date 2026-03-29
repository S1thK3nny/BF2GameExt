#include "pch.h"
#include "anim_bank_append.hpp"
#include "core/resolve.hpp"

#pragma warning(disable: 4996) // strncpy, _snprintf deprecation

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <detours.h>

// =============================================================================
// Animation Bank Append — sub-bank merging across .lvl files
//
// Background:
//   Animation banks can be split into numbered sub-banks. The vanilla "human"
//   bank ships as human_0 through human_4 inside ingame.lvl.  Banks that
//   don't use sub-banks are stored as a single entry (e.g. just "simp", shout out to you Máté).
//
//   To add new animations (e.g. prone) without overwriting the vanilla
//   human_0-4 (which mods are likely to replace, all of mine especially), we ship them in a new
//   sub-bank: human_5. This way a mod can freely replace human_0-4 with
//   custom animations and human_5 survives untouched.
//
// The engine problem:
//   It is common practice that you read dc:ingame.lvl before you read ingame.lvl
//   ReadZaa has a NULL guard, meaning it won't overwrite sub-banks
//   that already have data, so the mod's human_0-4 are preserved and
//   human_5 from vanilla loads correctly into the global RedAnimation hash
//   table.  So far so good.
//
//   However, AnimationFinder::_AddBank("human") runs during dc:ingame.lvl
//   processing.  It iterates human_0, human_1, ... looking them up in the
//   hash table.  It finds 0-4, tries human_5 (doesn't exist yet because
//   vanilla ingame.lvl hasn't loaded), and stops.  By the time vanilla adds
//   human_5 to the hash table, the base bank iteration has already finished.
//   _AddBank("human") never runs again.  human_5 sits in the hash table
//   unused.
//
// The fix:
//   Hook AnimationFinder::_AddBank.  The engine keeps calling it for weapon-
//   specific banks (human_rifle, human_bazooka, ...) which happen AFTER
//   vanilla ingame.lvl has loaded.  On each call, we extract the root bank
//   name (before first underscore: "human_rifle" -> "human") and scan the
//   hash table for sub-banks (human_0, human_1, ...) that exist but aren't
//   in the AnimBank array yet.  Late arrivals like human_5 get appended.
//   Thus, the best practice to not force mods to include the new sub-bank
//   is to inject the human_5 prone animations into the vanilla ingame.lvl
//   with BAD_ALs LVLTool, and let this system append it to the bank array.
//   For a public release, we would include a patched ingame.lvl with the 
//   prone sub-bank already in place, depending on how legal that is.
//
// Generality:
//   This is not human-specific.  Any bank can be extended with numbered
//   sub-banks, even if the original doesn't use them.  Loading "simp" from
//   one .lvl and "simp_0" from another will result in both being in the
//   bank array.  The animation system searches all banks in the array, so
//   animations from every sub-bank are accessible.
//
//   Individual animations within an existing sub-bank cannot be appended —
//   the NULL guard means the first-loaded version of a sub-bank wins
//   entirely.  New animations must go in a NEW numbered sub-bank.
// =============================================================================

// ---------------------------------------------------------------------------
// Engine function types
// ---------------------------------------------------------------------------

using fn_AddBank_t  = bool(__fastcall*)(void* ecx, void* edx, char* name);
using fn_PblHash_t  = void(__thiscall*)(uint32_t* outHash, const char* str);
using fn_HashFind_t = void*(__cdecl*)(void* table, int size, uint32_t hash);
using fn_GameLog_t  = void(__cdecl*)(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Resolved pointers
// ---------------------------------------------------------------------------

static fn_AddBank_t     original_AddBank = nullptr;
static fn_PblHash_t     fn_pblHash       = nullptr;
static fn_HashFind_t    fn_hashFind      = nullptr;
static fn_GameLog_t     fn_log           = nullptr;
static void*            g_animHashTable  = nullptr;

// ---------------------------------------------------------------------------
// AnimationFinder layout
// ---------------------------------------------------------------------------

static constexpr int kAF_MaxCount      = 0x208;
static constexpr int kAF_AnimBank      = 0x20C;
static constexpr int kAF_AnimBankCount = 0x210;

// RedAnimation layout
static constexpr int kRA_ZephyrAnimBank = 0x14;
static constexpr int kRA_MBFind         = 0x24;

// Max sub-bank index to search
static constexpr int kMaxSubBankSearch  = 32;


// ---------------------------------------------------------------------------
// try_append_sub_banks — scans for sub-banks of rootName that exist in the
// hash table but aren't in the AnimBank array yet.
// ---------------------------------------------------------------------------
static void try_append_sub_banks(char* self, const char* rootName)
{
    int   maxCount   = *(int*)(self + kAF_MaxCount);
    void** bankArray = *(void***)(self + kAF_AnimBank);
    int*  pCount     = *(int**)(self + kAF_AnimBankCount);

    if (!bankArray || !pCount) return;

    for (int i = 0; i < kMaxSubBankSearch; i++) {
        char subName[280];
        _snprintf(subName, sizeof(subName), "%s_%d", rootName, i);

        uint32_t hash;
        fn_pblHash(&hash, subName);

        void* entry = fn_hashFind(g_animHashTable, 0x800, hash);
        if (!entry) break;  // No more sub-banks

        // Skip entries with no animation data loaded
        if (*(void**)((char*)entry + kRA_ZephyrAnimBank) == nullptr)
            continue;

        // Duplicate check — already in the bank array?
        bool duplicate = false;
        int count = *pCount;
        for (int j = 0; j < count; j++) {
            if (bankArray[j] == entry) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        // Re-read maxCount (may have been updated by a previous expansion)
        maxCount = *(int*)(self + kAF_MaxCount);

        // Capacity check — expand if full
        if (count >= maxCount) {
            int newMax = maxCount + 16;
            void** newArray = (void**)HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY, newMax * sizeof(void*));
            if (!newArray) {
                if (fn_log)
                    fn_log("[AnimBankAppend] HeapAlloc failed\n");
                break;
            }
            memcpy(newArray, bankArray, count * sizeof(void*));

            *(void***)(self + kAF_AnimBank) = newArray;
            *(int*)(self + kAF_MaxCount) = newMax;
            bankArray = newArray;
            maxCount  = newMax;
        }

        // Mark as referenced and append
        *(uint8_t*)((char*)entry + kRA_MBFind) = 1;
        bankArray[*pCount] = entry;
        (*pCount)++;

    }
}


// ---------------------------------------------------------------------------
// hooked_AddBank
// ---------------------------------------------------------------------------
static bool __fastcall hooked_AddBank(void* ecx, void* edx, char* name)
{
    bool result = original_AddBank(ecx, edx, name);

    // Extract root bank name: everything before the FIRST underscore.
    // "human_rifle" -> "human", "human" -> "human", "pim_stormtrooper" -> "pim"
    char rootName[260];
    strncpy(rootName, name, 259);
    rootName[259] = '\0';
    char* us = strchr(rootName, '_');
    if (us) *us = '\0';

    // Scan for missing sub-banks of the root
    try_append_sub_banks((char*)ecx, rootName);

    return result;
}


// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void anim_bank_append_install(uintptr_t exe_base)
{
    using namespace game_addrs::modtools;

    original_AddBank = (fn_AddBank_t)resolve(exe_base, anim_finder_add_bank);
    fn_pblHash       = (fn_PblHash_t)resolve(exe_base, hash_string_thiscall);
    fn_hashFind      = (fn_HashFind_t)resolve(exe_base, pbl_hash_table_find);
    g_animHashTable  = (void*)resolve(exe_base, anim_hash_table);
    fn_log           = (fn_GameLog_t)resolve(exe_base, game_log);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)original_AddBank, hooked_AddBank);
    DetourTransactionCommit();
}

void anim_bank_append_uninstall()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (original_AddBank) DetourDetach(&(PVOID&)original_AddBank, hooked_AddBank);
    DetourTransactionCommit();
}
