#include "pch.h"
#include "combo_damage_anim_guard.hpp"
#include "combo_anim_limit.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <detours.h>
#include <intrin.h>

// =============================================================================
// Soldier animation lookup and melee damage guards.
//
// A stock AnimationMap is 0x4B8 bytes: 38 action pairs, 78 movement pairs,
// a union of 18 weapon / 30 melee pairs, and ten custom-animation dwords.
// The union means 146 physical pairs expose logical indices 0..163. Indices
// beyond that range read custom data or a neighbouring map, and reset-fill
// 0xFFFFFFFF also survives the engine's ordinary NULL checks.
//
// With ComboAnimIncrease active, combo_anim_limit owns the body getters and
// supplies enlarged maps for logical indices 0..223. This module leaves those
// entry points alone and probes that same storage when resolving damage. With
// the feature off, the two stock getter clamps reject invalid maps, indices
// and reset-fill. Other direct map readers are handled by the limit extension.
//
// Combo::Attack::_ResolveDamageData (modtools 0x005FCD30, Steam/GOG 0x004727A0)
// still needs its own guard. It gets the upper-body animation and dereferences
// it without a NULL test, unlike its sibling Combo::ResolveForWeapon:
//
//     modtools 0x005FCEB3  MOV CL,[EAX+0x28]   ; Attack::mAnimIndex
//              0x005FCEB6  MOV EAX,[EDI+8]     ; Combo::mMap
//              0x005FCEC9  CALL GetUpperBodyAnimation
//              0x005FCECE  MOV [ESP+0x24],EAX ; later dereferenced raw
//
//     Steam    0x0047293E  MOV EAX,[EDI+8]
//              0x00472946  CALL 0x006439E0
//              0x0047294B  MOV EDI,EAX
//
// Repeat the lookup before entering the resolver and reject a missing clip.
// Returning false matches the function's existing invalid-bone failure paths;
// its caller discards the result, so the attack simply contributes no samples.
// Nothing has been constructed before this check, which also avoids touching
// the retail resolver's SEH frame. See docs/RE/ComboDamageResolver.md.
//
// The class contains 30 animation maps, followed by unrelated ordnance and
// bank data. Dividing the whole class allocation by a map stride does not give
// a legal map bound. Negative maps can result from failed weapon-map lookup.
//
// 0xFF is an unassigned byte-sized animation index, including on stock content.
// It is separate from the stock/expanded logical sentinel and is silent here.
// =============================================================================

namespace {

// Combo::Attack::_ResolveDamageData — __thiscall, five stack args, RET 0x14.
// Declared __fastcall with a dummy EDX so MSVC emits the same callee-cleanup.
// Returns "resolved" in AL; the sole caller on every build discards it.
typedef char(__fastcall* fn_ResolveDamageData_t)(void* ecx, void* edx, void* combo,
                                                 void* attack, uint32_t a3, uint32_t a4,
                                                 void* a5);

// SoldierAnimatorClass::GetUpperBodyAnimation — __thiscall(int map, int idx),
// RET 8, ECX = SoldierAnimatorClass::sInstance.  Byte-identical on all three
// builds apart from the frame (modtools reads its args off ESP, retail off EBP).
typedef void*(__fastcall* fn_GetUpperBodyAnim_t)(void* ecx, void* edx, int map, int idx);

struct build_addrs {
   uintptr_t resolve_damage_data;
   uintptr_t get_upper_body_anim;
   uintptr_t get_lower_body_anim;
   uintptr_t animator_instance;   // SoldierAnimatorClass::sInstance (a pointer cell)
   uintptr_t anim_idx_load;       // the `MOV r8,[EAX+disp8]` that reads the index
   uint8_t   attack_anim_idx_off; // Attack::mAnimIndex, differs modtools vs retail
};

// Verified against the shipped executables 2026-09-06.  Steam and GOG share the
// resolver address; only the getter and the instance cell shift.
//
// Attack is NOT laid out the same on both lineages: the modtools debug build
// carries 0xC extra bytes ahead of these fields, so mAnimIndex sits at +0x28
// there and +0x1C on retail (the flags dword the function tests next moves the
// same 0xC, +0x34 vs +0x28).  anim_idx_load points at the engine's own read of
// that field so install can prove the offset rather than trust this table.
const build_addrs kModtools = {0x005FCD30, 0x0057DD40, 0x0057DD80,
                               0x00B8D3C4, 0x005FCEB3, 0x28};
const build_addrs kSteam    = {0x004727A0, 0x006439E0, 0x00643A10,
                               0x01EAFB1C, 0x00472933, 0x1C};
const build_addrs kGOG      = {0x004727A0, 0x00644A80, 0x00644AB0,
                               0x01EB0FD0, 0x00472933, 0x1C};

// Entry signatures, checked before hooking so an unrecognised build no-ops
// instead of detouring the wrong code.
//   all builds: PUSH EBP / MOV EBP,ESP / AND ESP,-0x10
//   modtools:   MOV EAX,0x2084        (__chkstk frame size)
//   retail:     PUSH -1 / PUSH <seh>  (SEH frame)
const uint8_t kEntryModtools[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0,
                                  0xB8, 0x84, 0x20, 0x00, 0x00};
const uint8_t kEntryRetail[]   = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF, 0x68};

// Getter signatures.  The 0x12E per-map stride is what makes these unambiguous,
// and the tail byte past it is what tells the upper- and lower-body getters
// apart: they are the same function up to how they turn the index into a slot
// (`SHL EAX,1` picks the upper of the pair, `LEA EAX,[EAX+EAX+1]` the lower).
const uint8_t kUpperModtools[] = {0x8B, 0x54, 0x24, 0x04, 0x0F, 0xB6, 0x44, 0x24,
                                  0x08, 0x69, 0xD2, 0x2E, 0x01, 0x00, 0x00,
                                  0xD1, 0xE0};
const uint8_t kLowerModtools[] = {0x8B, 0x54, 0x24, 0x04, 0x0F, 0xB6, 0x44, 0x24,
                                  0x08, 0x69, 0xD2, 0x2E, 0x01, 0x00, 0x00,
                                  0x8D, 0x44, 0x00, 0x01};
const uint8_t kUpperRetail[]   = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x0F, 0xB6,
                                  0xD0, 0x69, 0x45, 0x08, 0x2E, 0x01, 0x00, 0x00,
                                  0x03, 0xD2};
const uint8_t kLowerRetail[]   = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x0F, 0xB6,
                                  0xC0, 0x8D, 0x14, 0x45, 0x01, 0x00, 0x00, 0x00,
                                  0x69, 0x45, 0x08, 0x2E, 0x01, 0x00, 0x00};

// Combo::mMap, identical on all three builds (`MOV EAX,[EDI+8]` at modtools
// 0x005FCEB6 / retail 0x0047293E).  Attack::mAnimIndex is per-build and lives
// in build_addrs above.
constexpr size_t kComboMapOff = 0x08;

constexpr int kStockAnimIndexEnd = 164;

int animation_index_end()
{
   return combo_anim_limit_active() ? kComboAnimationEnd : kStockAnimIndexEnd;
}

// A byte-sized animation index field that was never assigned holds 0xFF. It is
// not an authored index and not a mod error - see clamp_body_animation.
constexpr int kUnassignedAnimIndex = 0xFF;

// Cheapest possible sanity test on a pointer the engine handed back, so a bad
// slot from any path we have not characterised degrades to a log line rather
// than an access violation inside this guard.
bool looks_like_pointer(const void* p)
{
   const uintptr_t v = (uintptr_t)p;
   return v >= 0x10000 && (v & 3) == 0;
}

fn_ResolveDamageData_t original_ResolveDamageData = nullptr;
fn_GetUpperBodyAnim_t  original_GetUpperBodyAnim  = nullptr;
fn_GetUpperBodyAnim_t  original_GetLowerBodyAnim  = nullptr;
fn_GetUpperBodyAnim_t  fn_getUpperBodyAnim        = nullptr;
void**                 g_animatorInstance         = nullptr;
size_t                 g_attackAnimIdxOff         = 0;

// Report each (map, index) pair once.  The resolver runs per attack per combo
// per weapon build, so an unguarded log here would flood bf2log.
struct reported_pair {
   int  map;
   int  idx;
};
reported_pair g_reported[32] = {};
int           g_reportedCount = 0;

bool already_reported(int map, int idx)
{
   for (int i = 0; i < g_reportedCount; ++i)
      if (g_reported[i].map == map && g_reported[i].idx == idx) return true;
   if (g_reportedCount < (int)(sizeof(g_reported) / sizeof(g_reported[0])))
      g_reported[g_reportedCount++] = {map, idx};
   return false;
}

// The getter clamp keeps its own table.  It runs on a different, far hotter
// path than the resolver guard, and sharing one table would let whichever fired
// first silence the other's report for the same (map, index) pair.
reported_pair g_getterReported[32] = {};
int           g_getterReportedCount = 0;

bool getter_already_reported(int map, int idx)
{
   for (int i = 0; i < g_getterReportedCount; ++i)
      if (g_getterReported[i].map == map && g_getterReported[i].idx == idx) return true;
   if (g_getterReportedCount < (int)(sizeof(g_getterReported) / sizeof(g_getterReported[0])))
      g_getterReported[g_getterReportedCount++] = {map, idx};
   return false;
}

// The stock getter clamp. Callers pass the animation index as a dword but
// the original leaf reads only its low byte, so preserve that convention.
// The extension owns these entry points when active; its shared lookup already
// validates ownership, the live map count and the expanded index range.
//
// Keep the unassigned 0xFF field silent. Stock equality checks against 0xA4
// can let it reach the getter even though no animation was ever requested.
void* clamp_body_animation(fn_GetUpperBodyAnim_t original, void* ecx, void* edx,
                           int map, int idx, const char* which, void* caller)
{
   const int animIdx = idx & 0xFF;

   // Unrelocated form, so a report can be looked up directly in Ghidra.
   const uintptr_t site = (uintptr_t)caller - exe_base() + kUnrelocatedBase;

   if (animIdx == kUnassignedAnimIndex) return nullptr;

   if (map < 0 || map >= kComboStockMapCount) {
      if (!getter_already_reported(map, animIdx))
         get_gamelog()("[ComboAnimGuard] no %s animation: invalid animation map %d "
                       "(index %d, valid maps 0-%d). Called from %08X\n",
                       which, map, animIdx, kComboStockMapCount - 1, (unsigned)site);
      return nullptr;
   }

   if (animIdx >= kStockAnimIndexEnd) {
      if (!getter_already_reported(map, animIdx))
         warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
            "[ComboAnimGuard] no %s animation: index %d requested on animation map %d, "
            "but the stock map supports indices 0-%d. ComboAnimIncrease is inactive; "
            "check its startup status before loading content that needs extra combo "
            "animations. Called from %08X\n",
            which, animIdx, map, kStockAnimIndexEnd - 1, (unsigned)site);
      return nullptr;
   }

   void* anim = original(ecx, edx, map, idx);

   // 0xFFFFFFFF is the map reset fill, i.e. "this slot was never populated".
   if (anim == (void*)~(uintptr_t)0) {
      if (!getter_already_reported(map, animIdx))
         get_gamelog()("[ComboAnimGuard] no %s animation: index %d is empty on animation "
                       "map %d - nothing was ever assigned to that slot\n",
                       which, animIdx, map);
      return nullptr;
   }

   return anim;
}

void* __fastcall hooked_GetUpperBodyAnimation(void* ecx, void* edx, int map, int idx)
{
   return clamp_body_animation(original_GetUpperBodyAnim, ecx, edx, map, idx, "upper body",
                               _ReturnAddress());
}

void* __fastcall hooked_GetLowerBodyAnimation(void* ecx, void* edx, int map, int idx)
{
   return clamp_body_animation(original_GetLowerBodyAnim, ecx, edx, map, idx, "lower body",
                               _ReturnAddress());
}

// This wrapper is called only by our C++ guard, not by the retail engine's
// register-sensitive getter call sites. Never probe a stock-map trampoline
// when the extension owns the map storage.
void* __fastcall expanded_GetUpperBodyAnimation(void* owner, void* /*edx*/, int map, int idx)
{
   return combo_anim_limit_get_body(owner, map, idx, false);
}

char __fastcall hooked_ResolveDamageData(void* ecx, void* edx, void* combo, void* attack,
                                         uint32_t a3, uint32_t a4, void* a5)
{
   if (combo && attack && g_animatorInstance && fn_getUpperBodyAnim) {
      const int map = *(int*)((uint8_t*)combo + kComboMapOff);
      const int idx = *((uint8_t*)attack + g_attackAnimIdxOff);

      if (idx == kUnassignedAnimIndex) return 0;

      const int mapCount = combo_anim_limit_map_count();
      const int indexEnd = animation_index_end();
      if (map < 0 || map >= mapCount) {
         if (!already_reported(map, idx))
            get_gamelog()("[ComboDamageGuard] skipped attack: combo[%08X] has animation map "
                          "%d, which is not a valid map (animation index %d)\n",
                          *(uint32_t*)combo, map, idx);
         return 0;
      }

      // Out of range for the table, so the lookup itself would read out of
      // bounds. Refuse before the read, not after.
      if (idx >= indexEnd) {
         if (!already_reported(map, idx))
            warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
               "[ComboDamageGuard] skipped attack: combo[%08X] uses animation index %d on "
               "animation map %d, outside the active range 0-%d "
               "(ComboAnimIncrease %s).\n",
               *(uint32_t*)combo, idx, map, indexEnd - 1,
               combo_anim_limit_active() ? "active" : "inactive");
         return 0;
      }

      if (void* animator = *g_animatorInstance) {
         void* anim = fn_getUpperBodyAnim(animator, nullptr, map, idx);
         // Both a missing table slot and a slot whose clip never loaded are
         // fatal downstream: the first is dereferenced directly, the second is
         // handed to ZephyrPoseDyn::SetAnimation and killed one call later in
         // ZephyrPoseDyn::Update.
         if (!anim || !looks_like_pointer(anim) || !*(void**)anim) {
            if (!already_reported(map, idx))
               get_gamelog()("[ComboDamageGuard] skipped attack: combo[%08X] animation index "
                             "%d is missing from animation map %d - the combo names a soldier "
                             "animation this character's bank does not have\n",
                             *(uint32_t*)combo, idx, map);
            return 0;
         }
      }
   }

   return original_ResolveDamageData(ecx, edx, combo, attack, a3, a4, a5);
}

} // namespace

void combo_damage_anim_guard_install(uintptr_t exe_base)
{
   const build_addrs* a;
   const uint8_t*     entrySig;
   size_t             entryLen;
   const uint8_t*     upperSig;
   size_t             upperLen;
   const uint8_t*     lowerSig;
   size_t             lowerLen;

   switch (g_build) {
   case GameBuild::Modtools:
      a = &kModtools;
      entrySig = kEntryModtools; entryLen = sizeof(kEntryModtools);
      upperSig = kUpperModtools; upperLen = sizeof(kUpperModtools);
      lowerSig = kLowerModtools; lowerLen = sizeof(kLowerModtools);
      break;
   case GameBuild::Steam:
      a = &kSteam;
      entrySig = kEntryRetail;  entryLen = sizeof(kEntryRetail);
      upperSig = kUpperRetail;  upperLen = sizeof(kUpperRetail);
      lowerSig = kLowerRetail;  lowerLen = sizeof(kLowerRetail);
      break;
   case GameBuild::GOG:
      a = &kGOG;
      entrySig = kEntryRetail;  entryLen = sizeof(kEntryRetail);
      upperSig = kUpperRetail;  upperLen = sizeof(kUpperRetail);
      lowerSig = kLowerRetail;  lowerLen = sizeof(kLowerRetail);
      break;
   default:
      return; // unknown build
   }

   // A signature miss used to report only the address, which made a silent
   // no-op indistinguishable from "somebody else got here first". Print what is
   // actually in memory: another injected DLL detouring the same leaf shows up
   // as a JMP (E9) in the first bytes, a wrong address as unrelated code.
   auto signature_ok = [](const uint8_t* at, uintptr_t va, const uint8_t* sig,
                          size_t len, const char* what) {
      if (std::memcmp(at, sig, len) == 0) return true;
      char got[3 * 24 + 1] = {};
      char want[3 * 24 + 1] = {};
      const size_t show = len < 12 ? len : 12;
      for (size_t i = 0; i < show; ++i) {
         sprintf_s(got + i * 3, 4, "%02X ", at[i]);
         sprintf_s(want + i * 3, 4, "%02X ", sig[i]);
      }
      install_log("[ComboAnimGuard] %s NOT hooked at 0x%08X: expected %s... found %s...",
                  what, (unsigned)va, want, got);
      return false;
   };

   // ComboAnimIncrease owns both getters when active. Do not compare their
   // patched entries with stock signatures or place another detour over them.
   const bool expanded = combo_anim_limit_active();
   uint8_t* upper = (uint8_t*)resolve(exe_base, a->get_upper_body_anim);
   bool upperOk = false;

   if (expanded) {
      install_log("[ComboAnimGuard] using ComboAnimIncrease body getters "
                  "(indices 0-%d)", kComboAnimationEnd - 1);
   } else {
      uint8_t* lower = (uint8_t*)resolve(exe_base, a->get_lower_body_anim);
      upperOk = signature_ok(upper, a->get_upper_body_anim, upperSig, upperLen,
                              "GetUpperBodyAnimation");
      const bool lowerOk = signature_ok(lower, a->get_lower_body_anim, lowerSig, lowerLen,
                                        "GetLowerBodyAnimation");

      if (upperOk && lowerOk) {
         original_GetUpperBodyAnim = (fn_GetUpperBodyAnim_t)upper;
         original_GetLowerBodyAnim = (fn_GetUpperBodyAnim_t)lower;

         DetourTransactionBegin();
         DetourUpdateThread(GetCurrentThread());
         DetourAttach(&(PVOID&)original_GetUpperBodyAnim, hooked_GetUpperBodyAnimation);
         DetourAttach(&(PVOID&)original_GetLowerBodyAnim, hooked_GetLowerBodyAnimation);
         if (DetourTransactionCommit() != NO_ERROR) {
            original_GetUpperBodyAnim = nullptr;
            original_GetLowerBodyAnim = nullptr;
            install_log("[ComboAnimGuard] animation index clamp NOT installed: "
                        "Detours commit failed");
         } else {
            install_log("[ComboAnimGuard] animation index clamp installed "
                        "(getters 0x%08X / 0x%08X, indices 0-%d, maps 0-%d)",
                        (unsigned)a->get_upper_body_anim, (unsigned)a->get_lower_body_anim,
                        kStockAnimIndexEnd - 1, kComboStockMapCount - 1);
         }
      }
   }

   // -------------------------------------------------------------------------
   // The resolver guard. Still needed with the clamp in place: _ResolveDamageData
   // is the one consumer that does not test the lookup at all, so turning garbage
   // into NULL is not enough for it.
   uint8_t* fn      = (uint8_t*)resolve(exe_base, a->resolve_damage_data);
   uint8_t* idxLoad = (uint8_t*)resolve(exe_base, a->anim_idx_load);

   if (!signature_ok(fn, a->resolve_damage_data, entrySig, entryLen,
                     "Combo::Attack::_ResolveDamageData"))
      return;

   // Prove Attack::mAnimIndex rather than trusting the table: the engine's own
   // read of it is `MOV r8,[EAX+disp8]` (modtools 8A 48 28, retail 8A 40 1C),
   // so the displacement byte must match what we are about to index with.
   if (idxLoad[0] != 0x8A || (idxLoad[1] & 0xC7) != 0x40 ||
       idxLoad[2] != a->attack_anim_idx_off) {
      install_log("[ComboDamageGuard] NOT installed: Attack::mAnimIndex is not at +0x%02X "
                  "(site 0x%08X reads %02X %02X %02X)",
                  a->attack_anim_idx_off, (unsigned)a->anim_idx_load,
                  idxLoad[0], idxLoad[1], idxLoad[2]);
      return;
   }

   // Expanded maps must use the same lookup as the installed feature getters.
   // Only the stock path may probe through the old getter trampoline.
   if (expanded)
      fn_getUpperBodyAnim = expanded_GetUpperBodyAnimation;
   else if (original_GetUpperBodyAnim)
      fn_getUpperBodyAnim = original_GetUpperBodyAnim;
   else
      fn_getUpperBodyAnim = upperOk ? (fn_GetUpperBodyAnim_t)upper : nullptr;
   g_animatorInstance         = (void**)resolve(exe_base, a->animator_instance);
   g_attackAnimIdxOff         = a->attack_anim_idx_off;
   original_ResolveDamageData = (fn_ResolveDamageData_t)fn;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_ResolveDamageData, hooked_ResolveDamageData);
   if (DetourTransactionCommit() != NO_ERROR) {
      original_ResolveDamageData = nullptr;
      fn_getUpperBodyAnim        = nullptr;
      g_animatorInstance         = nullptr;
      g_attackAnimIdxOff         = 0;
      install_log("[ComboDamageGuard] NOT installed: Detours commit failed");
      return;
   }

   install_log("[ComboDamageGuard] installed (resolver 0x%08X, Attack::mAnimIndex +0x%02X)",
               (unsigned)a->resolve_damage_data, a->attack_anim_idx_off);
}

void combo_damage_anim_guard_uninstall()
{
   if (original_ResolveDamageData) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(&(PVOID&)original_ResolveDamageData, hooked_ResolveDamageData);
      DetourTransactionCommit();
      original_ResolveDamageData = nullptr;
   }

   if (original_GetUpperBodyAnim) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(&(PVOID&)original_GetUpperBodyAnim, hooked_GetUpperBodyAnimation);
      DetourDetach(&(PVOID&)original_GetLowerBodyAnim, hooked_GetLowerBodyAnimation);
      DetourTransactionCommit();
      original_GetUpperBodyAnim = nullptr;
      original_GetLowerBodyAnim = nullptr;
   }

   fn_getUpperBodyAnim   = nullptr;
   g_animatorInstance    = nullptr;
   g_attackAnimIdxOff    = 0;
   g_reportedCount       = 0;
   g_getterReportedCount = 0;
}
