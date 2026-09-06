#include "pch.h"
#include "combo_damage_anim_guard.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <detours.h>

// =============================================================================
// Out-of-range soldier animation index crash fixes.
//
// -----------------------------------------------------------------------------
// The shared cause
//
// SoldierAnimatorClass::AnimationMap stores exactly 164 animation slots per map
// and NOTHING in the engine range-checks an animation index against that.
//
// A map is 0x4B8 = 1208 bytes (`IMUL ,0x12E`, 302 dwords) laid out as
//
//     mActionAnimation      [38]      pairs   offset    0
//     mMovementAnimation    [6][13]   = 78    offset  304
//     mWeaponAnimation      [3][6]    = 18  } aliased   928
//     mWeaponMeleeAnimation [30]      = 30  } union     928
//     mCustomAnimation      uint[10]          offset 1168
//
// 38 + 78 + 18 + 30 = 164, and the melee sub-array ends exactly where
// mCustomAnimation starts, which is why 0xA4 (164) is the engine's "no
// animation" sentinel: it is one past the end of the array, not a magic number.
// See docs/RE/ComboAnimationLimit.md for the derivation.
//
// [LimitIncreases] ComboAnimIncrease moves that sentinel to 0xFE (254) at 25
// sites so a mod can carry more than 30 distinct combo animation names. It does
// not widen the per-map block, so indices 164..253 address the NEXT map's slots.
//
// -----------------------------------------------------------------------------
// Bug 1 - the getters return garbage, and every consumer only tests for NULL
//
// SoldierAnimatorClass::Get{Upper,Lower}BodyAnimation (modtools 0x0057DD40 /
// 0x0057DD80, Steam 0x006439E0 / 0x00643A10, GOG 0x00644A80 / 0x00644AB0) are
// bare table reads with no rejection path at all:
//
//     IMUL EDX,map,0x12E
//     MOV  EAX,[ECX + EDX*4 + 0x24]      ; idx <  134
//     MOV  EAX,[ECX + EDX*4 - 0x6C]      ; idx >= 134
//
// Both branches return something for every index a uchar can hold. Two values
// therefore reach callers that only compare against NULL:
//
//   * an index past 163 -> whatever dword sits in the neighbouring map.
//     Confirmed live 2026-09-06 on both modtools and GOG: map 27, index 179,
//     returning 0x00000041 and 0x15AC5475.
//   * an in-range but unpopulated slot -> 0xFFFFFFFF, because a map is reset
//     with `OR EAX,-1 / REP STOSD` across all 302 dwords (modtools 0x0057E190).
//     This is the value behind the parked UpdateUpperBodyAnimation crash.
//
// Fixed by clamping inside the two getters, which is the choke point the whole
// family goes through - UpdateActionAnimation, UpdateMovementAnimation,
// SetupPose, EntitySoldier::Render and the damage resolver all come through
// here. Out of range and never-populated both become NULL, which is the answer
// every one of those callers already handles.
//
// -----------------------------------------------------------------------------
// Bug 2 - _ResolveDamageData does not test the lookup at all
//
// _ResolveDamageData (modtools 0x005FCD30, Steam/GOG 0x004727A0) is the melee
// damage-ray resolver. For each Attack in a combo state it walks the attack's
// animation and records the keyframed blade positions and directions the swing
// sweeps (docs/RE/ComboDamageResolver.md). It fetches that animation once:
//
//     modtools 0x005FCEB3  MOV CL,[EAX+0x28]        ; Attack::mAnimIndex
//              0x005FCEB6  MOV EAX,[EDI+8]          ; Combo::mMap
//              0x005FCEC9  CALL GetUpperBodyAnimation
//              0x005FCECE  MOV [ESP+0x24],EAX       ; stored, never tested
//
//     Steam    0x0047293E  MOV EAX,[EDI+8]          ; Combo::mMap
//              0x00472946  CALL 0x006439E0
//              0x0047294B  MOV EDI,EAX              ; stored, never tested
//
// and then dereferences it raw at modtools 0x005FD17F (`MOV EAX,[ESI]`). Its
// immediate sibling Combo::ResolveForWeapon does the identical lookup and DOES
// null-check it; this one does not, and has four unguarded uses.
//
// So the clamp is not enough for this caller, and it keeps its own guard:
// repeat the lookup, skip the call when the animation is unusable. Skipping is
// the engine's own answer to an attack it cannot resolve - the two RedWarning
// paths inside the function ("uses damage edge attached to invalid bone",
// "...to lower body bone ... without AnimatedMove!") both return false the same
// way, and the single caller on every build discards the return value. The
// attack contributes no damage samples instead of crashing. Nothing is
// constructed before the check, so there is nothing to unwind, which also keeps
// us clear of the SEH frame the retail builds set up.
//
// A negative map is bailed on too. That is the poisoned-ODF state described in
// setcharacterweapon_melee_animmap (WeaponMeleeClass::Build caches the first
// instance's animation MAP into the shared class and writes -1 when the lookup
// failed); feeding -1 to the getter reads far below the animator object.
//
// -----------------------------------------------------------------------------
// What this does NOT do
//
// It does not make an animation authored at index 164+ play. The storage is not
// there, and putting it there means widening the per-map block, which is a
// separate piece of work tracked in ROADMAP.md and
// docs/RE/ComboAnimationLimit.md. Until then an over-budget combo animation is
// silently absent rather than fatal, and the log names the map and index so the
// content side can be brought back under the limit.
//
// Reading a crash log for the resolver bug: [Features] Prone hooks the
// animation accessor at modtools 0x005701F0 with its own null guard
// (soldier_prone.cpp), so with Prone on a NULL survives four more instructions
// and the AV lands on the raw deref at 0x005FD17F rather than inside the
// accessor. Same bug either way.
// =============================================================================

namespace {

// Install-time logging goes to BF2GameExt.log through the CRT, never through the
// engine's logger: dllmain holds every exe section at PAGE_READWRITE (so
// non-executable) until all the installers have run, and calling engine code
// from an installer is an immediate EXEC access violation on the DEP-enabled
// retail builds. Runtime code below uses get_gamelog() instead.
void install_log(const char* fmt, ...)
{
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fclose(f);
}

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

// Upper bound on a legal animation map.  SoldierAnimatorClass is one
// `operator new(0xF7C0)` block (modtools 0x005817A9) holding per-map blocks of
// 0x4B8 bytes after a 0x24 header, so 52 maps fit.  64 is comfortably above any
// legal value and comfortably below anything that would read off the object.
constexpr int kMaxAnimMap = 64;

// Upper bound on a legal animation index, and the reason the stock sentinel is
// 0xA4.  Inside a map's 0x4B8-byte block this index addresses a sub-array at
// `idx*8 - 0x6C`, and the next sub-array starts at +0x4B4, so the last index
// that fits is 163 and 164 is one past the end.  ComboAnimIncrease raises the
// sentinel to 0xFE at 25 sites without widening the block, so 164..253 read
// into the NEXT map's slots and hand back whatever happens to sit there.
//
// That is not a hypothetical: observed 2026-09-06 on both modtools and GOG with
// map 27, index 179, returning 0x00000041 and 0x15AC5475 - non-NULL values that
// are not pointers. The read has to be refused before it happens; there is no
// checking the result afterwards, because the result is meaningless.
constexpr int kMaxAnimIndex = 164;

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

// -----------------------------------------------------------------------------
// The getter clamp.
//
// SoldierAnimatorClass::Get{Upper,Lower}BodyAnimation are bare table reads with
// no rejection path:
//
//     IMUL EDX,map,0x12E                 ; 302 dwords = 0x4B8 = 1208 bytes/map
//     MOV  EAX,[ECX + EDX*4 + 0x24]      ; idx <  134
//     MOV  EAX,[ECX + EDX*4 - 0x6C]      ; idx >= 134
//
// Both branches return something for any index a uchar can hold.  A map's block
// holds 164 slots (the melee sub-array is last and ends exactly where
// mCustomAnimation starts), so 164..255 address the NEXT map's block.
//
// Two values therefore reach callers that only test for NULL:
//
//   * an out-of-range index -> whatever dword sits in the neighbouring map
//     (observed: map 27, index 179 -> 0x00000041 and 0x15AC5475), and
//   * an in-range but unpopulated slot -> 0xFFFFFFFF, because a map is reset by
//     `OR EAX,-1 / REP STOSD` over all 302 dwords.
//
// Returning NULL for both is the answer every consumer already handles: they
// all compare the result against the "no animation" case and skip.  Nothing can
// be relying on -1 as a live value, because a consumer that received one would
// dereference it.
//
// This is the choke point for the whole family - UpdateActionAnimation,
// UpdateMovementAnimation, SetupPose, EntitySoldier::Render and the damage
// resolver all come through here - which is why it is a clamp on the getters
// rather than a guard bolted onto each caller.
//
// Callers push the index as a dword whose low byte is the index; the engine
// reads it with MOVZX, so only the low byte is meaningful and the test has to
// mask before comparing.
void* clamp_body_animation(fn_GetUpperBodyAnim_t original, void* ecx, void* edx,
                           int map, int idx, const char* which)
{
   const int animIdx = idx & 0xFF;

   if (animIdx >= kMaxAnimIndex) {
      if (!getter_already_reported(map, animIdx))
         warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
            "[ComboAnimGuard] no %s animation: index %d requested on animation map %d, "
            "but a map only stores indices 0-%d. The animation authored at that index "
            "cannot play. Reduce the number of distinct combo animation names, or turn "
            "off [LimitIncreases] ComboAnimIncrease - it raises the index limit without "
            "raising the storage behind it.\n",
            which, animIdx, map, kMaxAnimIndex - 1);
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
   return clamp_body_animation(original_GetUpperBodyAnim, ecx, edx, map, idx, "upper body");
}

void* __fastcall hooked_GetLowerBodyAnimation(void* ecx, void* edx, int map, int idx)
{
   return clamp_body_animation(original_GetLowerBodyAnim, ecx, edx, map, idx, "lower body");
}

char __fastcall hooked_ResolveDamageData(void* ecx, void* edx, void* combo, void* attack,
                                         uint32_t a3, uint32_t a4, void* a5)
{
   if (combo && attack && g_animatorInstance && fn_getUpperBodyAnim) {
      const int map = *(int*)((uint8_t*)combo + kComboMapOff);
      const int idx = *((uint8_t*)attack + g_attackAnimIdxOff);

      if (map < 0 || map >= kMaxAnimMap) {
         if (!already_reported(map, idx))
            get_gamelog()("[ComboDamageGuard] skipped attack: combo[%08X] has animation map "
                          "%d, which is not a valid map (animation index %d)\n",
                          *(uint32_t*)combo, map, idx);
         return 0;
      }

      // Out of range for the table, so the lookup itself would read out of
      // bounds. Refuse before the read, not after.
      if (idx >= kMaxAnimIndex) {
         if (!already_reported(map, idx))
            warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
               "[ComboDamageGuard] skipped attack: combo[%08X] uses animation index %d on "
               "animation map %d, but a map only holds indices 0-%d. Raise the combo's "
               "animation index limit or reduce the number of combo animations; with "
               "[LimitIncreases] ComboAnimIncrease=1 the engine hands out indices it cannot "
               "store.\n",
               *(uint32_t*)combo, idx, map, kMaxAnimIndex - 1);
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
      install_log("[ComboAnimGuard] %s NOT hooked at 0x%08X: expected %s... found %s...\n",
                  what, (unsigned)va, want, got);
      return false;
   };

   // -------------------------------------------------------------------------
   // The getter clamp. This is the part that matters most: it is the single
   // choke point every animation-index consumer goes through, so it is
   // installed on its own and a failure anywhere else does not take it out.
   uint8_t* upper = (uint8_t*)resolve(exe_base, a->get_upper_body_anim);
   uint8_t* lower = (uint8_t*)resolve(exe_base, a->get_lower_body_anim);

   const bool upperOk = signature_ok(upper, a->get_upper_body_anim, upperSig, upperLen,
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
                     "Detours commit failed\n");
      } else {
         install_log("[ComboAnimGuard] animation index clamp installed "
                     "(getters 0x%08X / 0x%08X, indices 0-%d)\n",
                     (unsigned)a->get_upper_body_anim, (unsigned)a->get_lower_body_anim,
                     kMaxAnimIndex - 1);
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
                  "(site 0x%08X reads %02X %02X %02X)\n",
                  a->attack_anim_idx_off, (unsigned)a->anim_idx_load,
                  idxLoad[0], idxLoad[1], idxLoad[2]);
      return;
   }

   // Probe through the trampoline when the clamp is in, so the guard's own
   // lookup does not re-enter the detour; fall back to the raw leaf otherwise.
   fn_getUpperBodyAnim        = original_GetUpperBodyAnim
                                   ? original_GetUpperBodyAnim
                                   : (upperOk ? (fn_GetUpperBodyAnim_t)upper : nullptr);
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
      install_log("[ComboDamageGuard] NOT installed: Detours commit failed\n");
      return;
   }

   install_log("[ComboDamageGuard] installed (resolver 0x%08X, Attack::mAnimIndex +0x%02X)\n",
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
