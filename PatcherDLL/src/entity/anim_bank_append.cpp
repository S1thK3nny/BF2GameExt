#include "pch.h"
#include "anim_bank_append.hpp"
#include "core/resolve.hpp"

#pragma warning(disable: 4996) // strncpy, _snprintf deprecation

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <detours.h>

// =============================================================================
// Animation Bank Append — sub-bank merging across .lvl files + per-class bank
// array capacity raise (the "Out of space for soldier animation banks (max 18)"
// fix).
//
// PART A — sub-bank merging (original feature):
//   Animation banks can be split into numbered sub-banks. The vanilla "human"
//   bank ships as human_0 through human_4 inside ingame.lvl.  Banks that
//   don't use sub-banks are stored as a single entry (e.g. just "simp", shout out to you Máté).
//
//   To add new animations (e.g. prone) without overwriting the vanilla
//   human_0-4 (which mods are likely to replace, all of mine especially), we ship them in a new
//   sub-bank: human_5. This way a mod can freely replace human_0-4 with
//   custom animations and human_5 survives untouched.
//
//   The engine problem: it is common practice that you read dc:ingame.lvl before
//   ingame.lvl.  ReadZaa has a NULL guard, so it won't overwrite sub-banks that
//   already have data — the mod's human_0-4 are preserved and human_5 from
//   vanilla loads correctly into the global RedAnimation hash table.  But
//   AnimationFinder::_AddBank("human") runs during dc:ingame.lvl processing: it
//   iterates human_0, human_1, ... looking them up, finds 0-4, tries human_5
//   (doesn't exist yet because vanilla ingame.lvl hasn't loaded), and stops.  By
//   the time vanilla adds human_5, _AddBank("human") never runs again, so
//   human_5 sits unused.
//
//   The fix: hook _AddBank.  The engine keeps calling it for weapon-specific
//   banks (human_rifle, ...) which happen AFTER vanilla ingame.lvl has loaded.
//   On each call we extract the root bank name and scan for sub-banks that exist
//   but aren't in the AnimBank array yet; late arrivals like human_5 get
//   appended.  That is how prone.lvl works: entity/prone_lvl_load.cpp hard-loads
//   it straight after ingame.lvl and this system picks up its human_5.  Patching
//   the prone animations into a replaced ingame.lvl was the old route and was
//   abandoned long ago — do not reason about ingame.lvl contents when debugging
//   a missing prone clip.
//
//   Appending alone is not enough: the finder searches the bank array through
//   per-frame index windows, and the window that is open while we append belongs
//   to a single weapon type on the AddComboAnimation path.  widen_bank_ranges
//   below reopens the bank-wide window over what we added — without it a late
//   sub-bank is only ever found for weapon 0 ("rifle").
//
//   Not human-specific: any bank can be extended with numbered sub-banks, even
//   if the original doesn't use them.  Individual animations within an existing
//   sub-bank cannot be appended (the NULL guard means first-loaded wins); new
//   animations must go in a NEW numbered sub-bank.
//
// PART B — per-class bank array capacity raise (18 -> kBigBankMax):
//   `SoldierAnimatorClass::mAnimBankOld[18]` (the inline array of loaded .zaabin
//   sub-bank pointers) is hardcoded to 18 and can't grow in place (mJetPackMatrix
//   follows it).  It is load-time staging only: _AddBank fills it, the
//   AnimationFinder resolves each (bank,weapon) into direct SoldierAnimation*
//   pointers stored in mMapOld/mMapNew, and the runtime plays from the MAP and
//   never re-reads the array (confirmed via hardware-breakpoint tracing).  So we
//   relocate it to a larger per-class heap buffer and redirect EVERY consumer to
//   that buffer:
//     - finders, via the mAnimBank pointer (hooked_AddBank/raise_finder_capacity,
//       hooked_Resolve);
//     - the skeleton-shared inline writer FUN_0057dec0 (hooked_AddSkel);
//     - the inline search loop FUN_0057de40 (hooked_FindInBanks).
//   The count int stays inline — it's used only as an index value.
//
//   NOTE — a SEPARATE, harder limit exists that this does NOT (and must not) try
//   to raise: the number of distinct bank NAMES is capped at 16, and weapon
//   types at 20, fused into fixed-size STACK-LOCAL arrays inside SetupBodyMasks
//   (aiStack_81c0[16*20], stride 0x14 baked in at 6 sites; coupled to the global
//   AddBank cap of 16 and the per-class AnimationMap[30]).  Raising the AddBank
//   cap overflows those stack arrays -> stack smash, so do NOT do it.  Modded
//   soldier classes must keep to <=16 distinct bank names and <=20 weapon types;
//   within that, this PART B raise lets them load far more than 18 .zaabin
//   sub-banks (up to kBigBankMax).
// =============================================================================

// ---------------------------------------------------------------------------
// Engine function types
// ---------------------------------------------------------------------------

using fn_AddBank_t   = bool(__fastcall*)(void* ecx, void* edx, char* name);
using fn_AddSkel_t   = bool(__fastcall*)(void* ecx, void* edx, void* skel);
using fn_Resolve_t   = int (__fastcall*)(void* ecx, void* edx, void* a2, int a3, unsigned a4);
using fn_FindBanks_t = int (__fastcall*)(void* ecx, void* edx, unsigned hash, char* name);
using fn_RedFind_t   = int (__fastcall*)(void* bank, void* edx, unsigned hash, char* name);
using fn_ZephyrFind_t= int (__fastcall*)(void* zephyrBank, void* edx, unsigned hash);
using fn_PblHash_t   = void(__thiscall*)(uint32_t* outHash, const char* str);
using fn_HashFind_t  = void*(__cdecl*)(void* table, int size, uint32_t hash);
using fn_GameLog_t   = void(__cdecl*)(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Resolved pointers
// ---------------------------------------------------------------------------

static fn_AddBank_t     original_AddBank     = nullptr;
static fn_AddSkel_t     original_AddSkel     = nullptr;   // FUN_0057dec0 (skeleton-shared inline writer)
static fn_Resolve_t     original_Resolve     = nullptr;   // FUN_0057f860 (resolve loop)
static fn_FindBanks_t   original_FindInBanks = nullptr;   // FUN_0057de40 (inline search loop)
static fn_RedFind_t     fn_RedFindAnimation  = nullptr;   // RedAnimation::FindAnimation (modtools; inlined away on Steam)
static fn_ZephyrFind_t  fn_ZephyrBankFind    = nullptr;   // ZephyrAnimBank find — Steam path (thiscall(bank, hash))
static fn_PblHash_t     fn_pblHash           = nullptr;
static fn_HashFind_t    fn_hashFind          = nullptr;
static fn_GameLog_t     fn_log               = nullptr;
static void*            g_animHashTable      = nullptr;

// ---------------------------------------------------------------------------
// AnimationFinder layout
// ---------------------------------------------------------------------------

static constexpr int kAF_MaxCount      = 0x208;
static constexpr int kAF_AnimBank      = 0x20C;
static constexpr int kAF_AnimBankCount = 0x210;

// m_Stack sits at +0: MyStack<AnimationFinder::Entry,3>, i.e. three 0xAC-byte
// Entry frames followed by the depth (which is why mMaxCount lands on 0x208).
// Entry (PDB-confirmed):
//   +0x00 BANK eBank
//   +0x04 int  iAnimBankStart_ByBank
//   +0x08 int  iAnimBankEnd_ByBank
//   +0x0C int  aiAnimBankStart_ByWeapon[20]
//   +0x5C int  aiAnimBankEnd_ByWeapon[20]
static constexpr int kAF_StackMaxDepth = 3;
static constexpr int kAF_StackDepth    = 0x204;
static constexpr int kAF_EntryStride   = 0xAC;
static constexpr int kAFE_BankEnd      = 0x08;

// RedAnimation layout
static constexpr int kRA_ZephyrAnimBank = 0x14;
static constexpr int kRA_MBFind         = 0x24;

// Max sub-bank index to search
static constexpr int kMaxSubBankSearch  = 32;

// SoldierAnimatorClass layout (the per-class object the finder points into)
static constexpr int kSAC_AnimBankOld   = 0xF72C;  // RedAnimation*[18] inline slab
static constexpr int kSAC_AnimBankCount = 0xF774;  // int, immediately after the 18 slots

// ---------------------------------------------------------------------------
// Per-class bank-array relocation (raises the inline 18-slot cap).
//
//   We keep ONE persistent heap buffer per SoldierAnimatorClass, keyed by the
//   address of its inline mAnimBankOld slab. Every consumer (finders + the two
//   inline accessors) is redirected to this buffer. The inline count int is
//   shared and used only as an index value, so it can exceed 18 safely.
// ---------------------------------------------------------------------------

static constexpr int kBigBankMax = 128;  // generous; the inline cap was 18

struct BankBufEntry { void** inlineArr; void** heap; };
static BankBufEntry g_bankBufs[64] = {};
static int          g_nBankBufs    = 0;

static void** find_or_make_big_array(void** inlineArr, int* pCount)
{
    for (int i = 0; i < g_nBankBufs; i++)
        if (g_bankBufs[i].inlineArr == inlineArr)
            return g_bankBufs[i].heap;

    if (g_nBankBufs >= 64) return nullptr;

    void** heap = (void**)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, kBigBankMax * sizeof(void*));
    if (!heap) return nullptr;

    // Copy whatever the engine has already placed in the inline slab (<=18).
    int n = pCount ? *pCount : 0;
    if (n > 18) n = 18;
    if (n > 0) memcpy(heap, inlineArr, n * sizeof(void*));

    g_bankBufs[g_nBankBufs++] = { inlineArr, heap };
    return heap;
}

// Repoint this finder's bank array to the class's big heap buffer and raise the
// cap, BEFORE the engine fills it.  No-op if the finder doesn't have the
// expected SoldierAnimatorClass layout (safety guard).
static void raise_finder_capacity(void* self)
{
    int*   pCount  = *(int**)((char*)self + kAF_AnimBankCount);
    void** curArr  = *(void***)((char*)self + kAF_AnimBank);
    if (!pCount || !curArr) return;

    char*  B         = (char*)pCount - kSAC_AnimBankCount;
    void** inlineArr = (void**)(B + kSAC_AnimBankOld);

    void** heap = nullptr;
    if (curArr == inlineArr) {
        // Fresh finder still pointing at the inline slab — adopt/seed the heap.
        heap = find_or_make_big_array(inlineArr, pCount);
    } else {
        // Already repointed by us on a previous call? Confirm it's our buffer.
        for (int i = 0; i < g_nBankBufs; i++)
            if (g_bankBufs[i].heap == curArr) { heap = curArr; break; }
        if (!heap) return;  // unknown layout — leave it alone
    }
    if (!heap) return;

    if (curArr != heap) *(void***)((char*)self + kAF_AnimBank) = heap;
    if (*(int*)((char*)self + kAF_MaxCount) < kBigBankMax)
        *(int*)((char*)self + kAF_MaxCount) = kBigBankMax;
}


// ---------------------------------------------------------------------------
// widen_bank_ranges — make freshly appended sub-banks visible to every weapon.
//
// AnimationFinder::FindZephyrAnimation searches two windows into the class's
// bank array: first the requesting weapon's own
// [aiAnimBankStart_ByWeapon[w], aiAnimBankEnd_ByWeapon[w]) — entered only for
// weapons whose name hash (_GetWeapon(w)+0x20) matches the requesting one, so no
// other weapon ever looks inside it — and then the bank-wide
// [iAnimBankStart_ByBank, iAnimBankEnd_ByBank).
//
// Both windows are stamped by whoever calls _AddBank, from the live count once it
// returns.  AnimationFinder::Push brackets the call with the bank-wide pair, but
// SoldierAnimationBank::AddComboAnimation brackets it with the by-weapon pair,
// once per weapon type.  We append from inside _AddBank, so a sub-bank that
// arrived too late for Push — exactly the case this module exists for — lands in
// the window of whichever weapon was adding its combo bank at that moment.  That
// is weapon 0, "rifle": every other weapon then resolves through the parent-weapon
// fallback as if the sub-bank were not loaded at all.
//
// A frame whose bank-wide window ends exactly where we began appending was still
// open on the end of the array, so widen it over what we added.  Push overwrites
// that field with the same value the instant we return, so this only ever changes
// anything for the weapon-bracketed calls.
// ---------------------------------------------------------------------------
static void widen_bank_ranges(char* self, int oldCount, int newCount)
{
    unsigned depth = *(unsigned*)(self + kAF_StackDepth);
    if (depth > kAF_StackMaxDepth) return;  // not a layout we recognise

    for (unsigned i = 0; i < depth; i++) {
        int* pEnd = (int*)(self + i * kAF_EntryStride + kAFE_BankEnd);
        if (*pEnd == oldCount)
            *pEnd = newCount;
    }
}


// ---------------------------------------------------------------------------
// try_append_sub_banks — scans for sub-banks of rootName that exist in the
// hash table but aren't in the AnimBank array yet.  Returns true when rootName
// names a numbered bank at all, i.e. "<rootName>_0" is registered, whether or
// not anything needed appending.
// ---------------------------------------------------------------------------
static bool try_append_sub_banks(char* self, const char* rootName)
{
    int   maxCount   = *(int*)(self + kAF_MaxCount);
    void** bankArray = *(void***)(self + kAF_AnimBank);
    int*  pCount     = *(int**)(self + kAF_AnimBankCount);

    if (!bankArray || !pCount) return false;

    const int startCount = *pCount;
    bool      isNumbered = false;

    for (int i = 0; i < kMaxSubBankSearch; i++) {
        char subName[280];
        _snprintf(subName, sizeof(subName), "%s_%d", rootName, i);

        uint32_t hash;
        fn_pblHash(&hash, subName);

        void* entry = fn_hashFind(g_animHashTable, 0x800, hash);
        if (!entry) break;  // No more sub-banks
        isNumbered = true;

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

    if (*pCount != startCount)
        widen_bank_ranges(self, startCount, *pCount);

    return isNumbered;
}


// ---------------------------------------------------------------------------
// hooked_AddBank — repoint the finder to the per-class heap buffer + raise the
// cap BEFORE the engine fills it (so the _AddBank loop never trips the max-18
// check), run the original, then append any late-arriving sub-banks.
// ---------------------------------------------------------------------------
static bool __fastcall hooked_AddBank(void* ecx, void* edx, char* name)
{
    raise_finder_capacity(ecx);

    bool result = original_AddBank(ecx, edx, name);

    // Two kinds of caller reach _AddBank, and only one of them passes a bare
    // bank name.  AnimationFinder::Push passes "<bank>"; AddComboAnimation
    // passes "<bank>_<weapon>".  Bank names may themselves contain underscores
    // ("pim_stormtrooper"), so splitting on one is guesswork either way — cutting
    // at the first underscore turns that bank into root "pim" and scans a bank
    // that probably isn't there while missing the one that is.
    //
    // Let the hash table decide instead.  Try the name as given: if it is a real
    // numbered bank, "<name>_0" is registered and we are done, which is the Push
    // case and also the right answer for an underscored bank name.  Only when
    // nothing is registered under it — the AddComboAnimation case, where no bank
    // is called "human_rifle" — drop the last component and try again.
    char rootName[260];
    strncpy(rootName, name, 259);
    rootName[259] = '\0';

    if (!try_append_sub_banks((char*)ecx, rootName)) {
        char* us = strrchr(rootName, '_');
        if (us) {
            *us = '\0';
            try_append_sub_banks((char*)ecx, rootName);
        }
    }

    return result;
}


// ---------------------------------------------------------------------------
// hooked_AddSkel — reimplementation of FUN_0057dec0 (SoldierAnimatorClass.cpp
// line 671). The stock function adds the skeleton-shared bank straight into the
// inline mAnimBankOld slab via fixed offsets (this+0xF72C / count this+0xF774)
// with a hardcoded <18 cap — bypassing the finder pointer entirely.  That makes
// it the one consumer that desyncs when we redirect the finders to the heap.
// We redirect it to the SAME per-class heap buffer (keyed by the inline array
// address, exactly like raise_finder_capacity) and use the raised cap, while
// preserving the original's mZephyrSkeletonShared bookkeeping and guards.
//
// Original logic (verified against disassembly at 0x0057DEC0):
//   ecx = SoldierAnimatorClass* ; skel = ZephyrSkeletonShared* (stack arg)
//   if (!skel) return false;
//   if (*(this+0x18) == 0) *(this+0x18) = *(skel+0xC);
//   if (*(int*)(skel+0x14) == 0) return true;
//   dedup scan mAnimBankOld[0..count); if found return true;
//   if (count >= 18) { warn; return false; }
//   mAnimBankOld[count] = skel; count++; return true;
// ---------------------------------------------------------------------------
static bool __fastcall hooked_AddSkel(void* ecx, void* /*edx*/, void* skel)
{
    char* self = (char*)ecx;
    if (!skel) return false;

    if (*(void**)(self + 0x18) == nullptr)
        *(void**)(self + 0x18) = *(void**)((char*)skel + 0xC);

    if (*(int*)((char*)skel + 0x14) == 0)
        return true;

    void** inlineArr = (void**)(self + kSAC_AnimBankOld);
    int*   pCount    = (int*)(self + kSAC_AnimBankCount);

    // Same heap buffer the finders use (identical key). Fall back to the inline
    // slab if allocation ever fails — that reverts to stock behaviour for this
    // class rather than risking a bad pointer.
    void** arr = find_or_make_big_array(inlineArr, pCount);
    if (!arr) { arr = inlineArr; }

    int count = *pCount;
    for (int i = 0; i < count; i++)
        if (arr[i] == skel) return true;   // already present

    int cap = (arr == inlineArr) ? 18 : kBigBankMax;
    if (count >= cap) return false;

    arr[count] = skel;
    *pCount = count + 1;
    return true;
}


// ---------------------------------------------------------------------------
// hooked_Resolve — FUN_0057f860, the AnimationFinder resolve loop that reads
// finder->mAnimBank[i] and calls RedAnimation::FindAnimation. A finder used only
// for resolution never passes through _AddBank, so it was never repointed and
// still aims at the (now-empty) inline slab while reading the shared, raised
// count — reading a null bank and crashing in FindAnimation. Repoint it here too
// (raise_finder_capacity is idempotent: a no-op for already-redirected finders).
// ---------------------------------------------------------------------------
static int __fastcall hooked_Resolve(void* ecx, void* edx, void* a2, int a3, unsigned a4)
{
    raise_finder_capacity(ecx);
    return original_Resolve(ecx, edx, a2, a3, a4);
}


// ---------------------------------------------------------------------------
// hooked_FindInBanks — FUN_0057de40 / Steam FUN_006442a0,
// SoldierAnimatorClass::FindAnimation. The stock function loops
// this->mAnimBankOld[0..mAnimBankCount) looking each bank up, returning the
// first hit. It reads the inline slab by fixed offset (not via a finder), so
// once we move the banks to the heap it reads the empty slab and crashes.
// Reimplement over the same per-class heap buffer; the count int stays inline
// (shared, correct).
//
// Signature is identical on both builds (Steam still RETs 8 — the name arg is
// passed but unused there).  The per-bank lookup differs: modtools calls
// RedAnimation::FindAnimation(bank, hash, name) (which also does the
// name/used bookkeeping); Steam inlined that away and calls the ZephyrAnimBank
// finder on bank->_pZephyrAnimBank (+0x14) directly, so we do the same.
// ---------------------------------------------------------------------------
static int __fastcall hooked_FindInBanks(void* ecx, void* /*edx*/, unsigned hash, char* name)
{
    char*  self      = (char*)ecx;
    int*   pCount    = (int*)(self + kSAC_AnimBankCount);
    void** inlineArr = (void**)(self + kSAC_AnimBankOld);

    void** arr = find_or_make_big_array(inlineArr, pCount);
    if (!arr) arr = inlineArr;

    int count = *pCount;
    for (int i = 0; i < count; i++) {
        void* bank = arr[i];
        if (!bank) continue;                       // skip empty slots defensively
        int r;
        if (fn_RedFindAnimation) {
            r = fn_RedFindAnimation(bank, nullptr, hash, name);
        } else {
            void* zb = *(void**)((char*)bank + kRA_ZephyrAnimBank);
            if (!zb) continue;
            r = fn_ZephyrBankFind(zb, nullptr, hash);
        }
        if (r) return r;
    }
    return 0;
}


// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void anim_bank_append_install(uintptr_t exe_base)
{
    if (g_build == GameBuild::Unknown) return;

    // Everything the hooks need; a build with any of these unmapped (GOG)
    // skips cleanly.  red_find_animation / zephyr_anim_bank_find are the
    // per-build lookup alternatives — exactly one must be present.
    if (g_addr->anim_finder_add_bank == 0 || g_addr->anim_add_skeleton_bank == 0 ||
        g_addr->anim_finder_resolve == 0 || g_addr->anim_class_find_in_banks == 0 ||
        g_addr->anim_hash_table == 0 || g_addr->hash_string_thiscall == 0 ||
        g_addr->pbl_hash_table_find == 0)
        return;
    if (g_addr->red_find_animation == 0 && g_addr->zephyr_anim_bank_find == 0)
        return;

    original_AddBank     = (fn_AddBank_t)resolve(exe_base, g_addr->anim_finder_add_bank);
    original_AddSkel     = (fn_AddSkel_t)resolve(exe_base, g_addr->anim_add_skeleton_bank);
    original_Resolve     = (fn_Resolve_t)resolve(exe_base, g_addr->anim_finder_resolve);
    original_FindInBanks = (fn_FindBanks_t)resolve(exe_base, g_addr->anim_class_find_in_banks);
    fn_RedFindAnimation  = g_addr->red_find_animation
                              ? (fn_RedFind_t)resolve(exe_base, g_addr->red_find_animation)
                              : nullptr;
    fn_ZephyrBankFind    = g_addr->zephyr_anim_bank_find
                              ? (fn_ZephyrFind_t)resolve(exe_base, g_addr->zephyr_anim_bank_find)
                              : nullptr;
    fn_pblHash           = (fn_PblHash_t)resolve(exe_base, g_addr->hash_string_thiscall);
    fn_hashFind          = (fn_HashFind_t)resolve(exe_base, g_addr->pbl_hash_table_find);
    g_animHashTable      = (void*)resolve(exe_base, g_addr->anim_hash_table);
    fn_log               = get_gamelog();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)original_AddBank, hooked_AddBank);
    DetourAttach(&(PVOID&)original_AddSkel, hooked_AddSkel);
    DetourAttach(&(PVOID&)original_Resolve, hooked_Resolve);
    DetourAttach(&(PVOID&)original_FindInBanks, hooked_FindInBanks);
    DetourTransactionCommit();
}

void anim_bank_append_uninstall()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (original_AddBank)     DetourDetach(&(PVOID&)original_AddBank, hooked_AddBank);
    if (original_AddSkel)     DetourDetach(&(PVOID&)original_AddSkel, hooked_AddSkel);
    if (original_Resolve)     DetourDetach(&(PVOID&)original_Resolve, hooked_Resolve);
    if (original_FindInBanks) DetourDetach(&(PVOID&)original_FindInBanks, hooked_FindInBanks);
    DetourTransactionCommit();
}
