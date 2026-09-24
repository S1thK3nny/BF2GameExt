#include "pch.h"
#include "soldier_bone_effect_null_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "core/x86_emit.hpp"

#include <cstring>

// =============================================================================
// Missing-bone crash in EntitySoldier::Render.
//
// -----------------------------------------------------------------------------
// The crash
//
//     EXCEPTION C0000005 at EIP=005373A1   AV: READ addr 00000000
//     ECX=00000010  ESI=00000000  EDI=0275EEB0
//
// modtools 0x005373A1 is a `REP MOVSD` with ECX=0x10 - a 64-byte copy, i.e. one
// 4x4 matrix - reading from ESI, and ESI is null.
//
// The block it sits in walks five bone-attached effect slots on the soldier:
//
//     0053734D  LEA  ECX,[EBX+0xA24]      ; slot array,      stride 8
//     00537353  LEA  EAX,[EBX+0xA4C]      ; bone-hash array, stride 4
//     00537361  MOV  [ESP+0x30],5         ; five slots
//   loop:
//     00537370  CALL 0x00411CCF           ; slot live?
//     00537377  JZ   0x005373B4           ; no -> next slot
//     00537379  MOV  ECX,[ESP+0x14]
//     0053737D  MOV  EDX,[ECX]            ; the bone's name hash
//     00537387  CALL 0x00405D85           ; PblHashTableCode::_Find(bones,0x100,hash)
//     0053738C  MOV  ESI,EAX              ; <-- result used with no null test
//     0053738E  MOV  ECX,0x10
//     00537393  LEA  EDI,[ESP+0xF0]
//     0053739A  LEA  EAX,[ESP+0xF0]
//     005373A1  REP  MOVSD                ; <-- AV when the bone was not found
//     ...
//     005373B4  (next slot)
//
// `_Find` is the engine's generic open-addressed hash lookup and returns null
// for a key that is not in the table, which is what a bone name the model does
// not have produces.  Nothing between the call and the copy tests EAX.
//
// So a class that pins an effect to a bone by name - SmolderEffect/SmolderBone
// is the property pair that reaches here - crashes the game as soon as a
// character whose skeleton lacks that bone is rendered.  Reported with
// `SmolderBone = "bone_l_forearm"` playing on General Grievous.
//
// -----------------------------------------------------------------------------
// The fix
//
// Skip the slot, which is already the engine's own answer for a slot it cannot
// draw: the `JZ` at 0x00537377 jumps to 0x005373B4, the top of the next
// iteration, and that is where the guard jumps too.  Registers are in the same
// state on both paths (0x005373B4 reloads ESI, ECX and the counter from the
// stack), so the skip is indistinguishable from an empty slot.
//
// Length-neutral, so nothing downstream shifts.  23 bytes in, 23 bytes out:
//
//     TEST EAX,EAX                 85 C0
//     JZ   0x005373B4              74 24
//     MOV  ESI,EAX                 8B F0
//     PUSH 0x10 / POP ECX          6A 10 59        (3 bytes, vs 5 for MOV ECX,imm32)
//     LEA  EAX,[ESP+0xF0]          8D 84 24 F0 00 00 00
//     MOV  EDI,EAX                 8B F8           (2 bytes, vs 7 for a second LEA)
//     REP  MOVSD                   F3 A5
//     NOP x3                       90 90 90
//
// The two folded instructions are what buys room for the test and the branch.
// `PUSH`/`POP` is balanced, so the `LEA` that follows still sees the same ESP
// the original `LEA` did, and `EAX` still holds [ESP+0xF0] on the way out
// because 0x005373A7 pushes it as the copy's destination.
//
// -----------------------------------------------------------------------------
// Steam and GOG: same bug, different codegen
//
// Both retail builds have the identical five-slot loop, at the same addresses as
// each other, with the arrays at [this+0xA08] and [this+0xA30] (modtools has
// them at +0xA24/+0xA4C - the debug build's extra padding).  The retail compiler
// did not emit an inline `REP MOVSD`; it pushed the pointer and called an
// out-of-line 4x4 copy:
//
//     004E3B4D  PUSH [EDI]                ; the bone's name hash
//     004E3B56  CALL <_Find(bones,0x100)> ; Steam 0x0043EB80, GOG 0x0043EB70
//     004E3B5B  PUSH EAX                  ; <-- result pushed with no null test
//     004E3B5C  LEA  ECX,[ESP+0x144]
//     004E3B63  CALL <copy 4x4>           ; Steam 0x0043A880, GOG 0x0043A870
//     ...
//     004E3B7E  (loop tail - reloads the counter from [ESP+0x10])
//
// The callee is the same 64-byte copy unrolled into 16 dword moves, and it
// dereferences its argument on the first instruction, so the fault just lands
// one frame deeper than it does on modtools.  Same bug, same trigger.
//
// The retail site has no room for a length-neutral rewrite - it needs 4 bytes
// for the test and branch and has none - so it is patched as a trampoline
// instead: the 8 bytes of `PUSH EAX` + `LEA ECX,[ESP+0x144]` become a `JMP` to
// a guard that performs the same two instructions when the pointer is good and
// jumps to the loop tail when it is not.  Every original CALL keeps its address
// and its operand, which is what makes this the low-risk shape here.
//
// The skip has to target 0x004E3B7E and NOT 0x004E3B82, where the loop's two
// existing skips go.  Those two branch before any call has run, so the counter
// is still live in ECX; by the time the guard fires, the lookup call has
// clobbered it, and 0x004E3B7E is the instruction that reloads it.
//
// Steam and GOG differ only in the three call targets, which the trampoline
// never touches, so both builds take byte-identical patch bytes.
// =============================================================================

namespace {

// The exact 23 bytes the fix replaces, used to positively identify the site
// before writing.  A build whose address has not been derived, or an executable
// that is not stock, fails this and the fix no-ops rather than corrupting code.
const uint8_t kOriginal[] = {
   0x8B, 0xF0,                                 // MOV ESI,EAX
   0xB9, 0x10, 0x00, 0x00, 0x00,               // MOV ECX,0x10
   0x8D, 0xBC, 0x24, 0xF0, 0x00, 0x00, 0x00,   // LEA EDI,[ESP+0xF0]
   0x8D, 0x84, 0x24, 0xF0, 0x00, 0x00, 0x00,   // LEA EAX,[ESP+0xF0]
   0xF3, 0xA5,                                 // REP MOVSD
};

const uint8_t kPatched[] = {
   0x85, 0xC0,                                 // TEST EAX,EAX
   0x74, 0x24,                                 // JZ  +0x24 -> next slot
   0x8B, 0xF0,                                 // MOV ESI,EAX
   0x6A, 0x10,                                 // PUSH 0x10
   0x59,                                       // POP  ECX
   0x8D, 0x84, 0x24, 0xF0, 0x00, 0x00, 0x00,   // LEA EAX,[ESP+0xF0]
   0x8B, 0xF8,                                 // MOV EDI,EAX
   0xF3, 0xA5,                                 // REP MOVSD
   0x90, 0x90, 0x90,                           // padding to the original length
};

static_assert(sizeof(kPatched) == sizeof(kOriginal),
              "the rewrite must be length-neutral");

// Retail: the 8 bytes the trampoline replaces, and their stand-in.
const uint8_t kRetailOriginal[] = {
   0x50,                                       // PUSH EAX
   0x8D, 0x8C, 0x24, 0x44, 0x01, 0x00, 0x00,   // LEA ECX,[ESP+0x144]
};

// Where the guard resumes the engine's own code on each path.  Set by install.
void* s_retailContinue = nullptr;  // the 4x4 copy call, pointer was good
void* s_retailSkip     = nullptr;  // the loop tail, bone was not on the model

// EAX = whatever the bone lookup returned.  Reached by JMP, so ESP is exactly
// what the replaced `PUSH EAX` would have seen and the `LEA` below is computed
// against the same ESP the original used.
__declspec(naked) void bone_matrix_guard_retail()
{
   __asm {
      test eax, eax
      jz   skip
      push eax                   // original PUSH EAX  (the matrix pointer)
      lea  ecx, [esp + 0x144]    // original LEA ECX,[ESP+0x144]  (the destination)
      jmp  [s_retailContinue]
   skip:
      jmp  [s_retailSkip]        // nothing pushed, so ESP is at loop-body level
   }
}

uint8_t* s_site = nullptr;
size_t   s_siteLen = 0;

} // namespace

// -----------------------------------------------------------------------------
void soldier_bone_effect_null_fix_install(uintptr_t exe_base)
{
   uintptr_t siteVA = 0, continueVA = 0, skipVA = 0;
   switch (g_build) {
   case GameBuild::Modtools:
      siteVA = game_addrs::modtools::soldier_render_bone_matrix_copy;
      break;
   case GameBuild::Steam:
      siteVA     = game_addrs::steam::soldier_render_bone_matrix_copy;
      continueVA = game_addrs::steam::soldier_render_bone_copy_continue;
      skipVA     = game_addrs::steam::soldier_render_bone_copy_skip;
      break;
   case GameBuild::GOG:
      siteVA     = game_addrs::gog::soldier_render_bone_matrix_copy;
      continueVA = game_addrs::gog::soldier_render_bone_copy_continue;
      skipVA     = game_addrs::gog::soldier_render_bone_copy_skip;
      break;
   default:
      return; // unknown build
   }
   if (siteVA == 0) return; // not derived on this build

   uint8_t* site = (uint8_t*)resolve(exe_base, siteVA);

   // .text is RW for the whole install window (dllmain re-protects afterwards),
   // so no VirtualProtect on either path here.
   if (g_build == GameBuild::Modtools) {
      // The JZ displacement is baked into kPatched, so the site has to be the
      // exact codegen it was measured against.  Anything else, leave it alone.
      if (std::memcmp(site, kOriginal, sizeof(kOriginal)) != 0) return;

      std::memcpy(site, kPatched, sizeof(kPatched));
      s_site    = site;
      s_siteLen = sizeof(kOriginal);
      return;
   }

   // Retail: trampoline.  Both resume points have to be real before anything is
   // written, or the guard would jump through a null.
   if (continueVA == 0 || skipVA == 0) return;
   if (std::memcmp(site, kRetailOriginal, sizeof(kRetailOriginal)) != 0) return;

   s_retailContinue = resolve(exe_base, continueVA);
   s_retailSkip     = resolve(exe_base, skipVA);

   // JMP rel32 to the guard, then NOP out the rest of the replaced pair so the
   // bytes after it stay decodable if anything ever walks them.
   uint8_t patch[sizeof(kRetailOriginal)];
   x86::encode_branch(patch, site, x86::kJmp, &bone_matrix_guard_retail, sizeof(patch));

   std::memcpy(site, patch, sizeof(patch));
   s_site    = site;
   s_siteLen = sizeof(kRetailOriginal);
}

void soldier_bone_effect_null_fix_uninstall()
{
   if (!s_site) return;

   const uint8_t* orig = (s_siteLen == sizeof(kOriginal)) ? kOriginal : kRetailOriginal;

   DWORD oldProt;
   if (VirtualProtect(s_site, s_siteLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
      std::memcpy(s_site, orig, s_siteLen);
      VirtualProtect(s_site, s_siteLen, oldProt, &oldProt);
   }
   s_site    = nullptr;
   s_siteLen = 0;
}
