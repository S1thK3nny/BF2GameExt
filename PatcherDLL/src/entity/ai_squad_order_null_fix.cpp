#include "pch.h"
#include "ai_squad_order_null_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstring>

// =============================================================================
// AILowLevel::UpdateIndirect null-target crash fix.
//
// The bug (modtools AILowLevel::UpdateIndirect, 0x005A2A80 / Steam 0x0043BC80 /
// GOG 0x0043BC70), in the mExitVehicle branch:
//
//     iVar3 = *(int*)(*(int*)(param_1 + 0x14) + 4);      // AI's controlled obj
//     if (*(int*)(iVar3 + 0x144) == 1 ||
//        (*(int*)(iVar3 + 0x144) == 4 && *(int*)(iVar3 + 0xd0) == iVar3))
//           target = NULL;                               // set to null ON PURPOSE
//     else  target = *(void**)(iVar3 + 0xd0);
//     handled = target->vtable[0x68](flag, 1);           // ... then called anyway
//
// That null-or-mPilot selection is Controllable::GetActivePilot inlined ("the
// separate rider, or null when the thing pilots itself"); vtable+0x68 is
// ExitVehicle(bool,bool).  A self-piloted vehicle has no rider, so the AI's
// "get out" request dereferences 0.
//
// modtools codegen (null in EAX):
//
//     005a2b7a: MOV EAX,[EAX+0xd0]
//     005a2b80: JMP 0x005a2b84
//     005a2b82: XOR EAX,EAX          <- null, deliberately
//     005a2b84: MOV ECX,[ESP+8]      } patch site (6 bytes)
//     005a2b88: MOV EDX,[EAX]        }  <- AV reading 0
//     005a2b8a: PUSH 1
//     005a2b8c: PUSH ECX
//     005a2b8d: MOV ECX,EAX
//     005a2b8f: CALL [EDX+0x68]
//     005a2b92: TEST AL,AL
//     005a2b94: JZ 0x005a2ba9        <- "not handled" continuation
//
// Steam/GOG codegen (null in ECX, and the two pushes happen *after* the
// dereference, so the null path has nothing pending on the stack):
//
//     0043bd27: MOV ECX,[EDX+0xd0]
//     0043bd2d: JMP 0x0043bd31
//     0043bd2f: XOR ECX,ECX          <- null, deliberately
//     0043bd31: MOV EAX,[ECX]        }  <- AV reading 0
//     0043bd33: PUSH 1               } patch site (7 bytes)
//     0043bd35: PUSH [EBP-4]         }
//     0043bd38: MOV EAX,[EAX+0x68]
//     0043bd3b: CALL EAX
//     0043bd3d: TEST AL,AL
//     0043bd3f: JZ 0x0043bd4a        <- "not handled" continuation
//
// (Steam VAs; GOG is the same code shifted -0x10.)
//
// Note the register can also arrive null from the mPilot load itself, so the
// guard belongs at the dereference rather than on the XOR branch.
//
// The fix routes the dereference through a code cave that null-checks first
// and, when there is no target, jumps to the engine's own "the virtual call
// returned false" continuation.  That is the right answer rather than an early
// return: the return value means "this order was handled", and a null target
// handled nothing, so the rest of UpdateIndirect should still run.  The sibling
// guarded call earlier in the same function uses exactly that convention.
//
// The patch site starts on the target of the preceding JMP on every build, so
// the incoming jump lands on our JMP.  Neither cave touches ESP or EBP, so the
// displaced `MOV ECX,[ESP+8]` / `PUSH [EBP-4]` stay valid inside it.
// =============================================================================

// Displaced bytes, also verified before patching so this silently no-ops on any
// build whose codegen differs.
//   modtools: MOV ECX,[ESP+8] ; MOV EDX,[EAX]
//   retail:   MOV EAX,[ECX]   ; PUSH 1 ; PUSH [EBP-4]
static constexpr uint8_t kOrigModtools[] = {0x8B, 0x4C, 0x24, 0x08, 0x8B, 0x10};
static constexpr uint8_t kOrigRetail[]   = {0x8B, 0x01, 0x6A, 0x01, 0xFF, 0x75, 0xFC};

// Second byte of `TEST r32,r32` for the register holding the (possibly null)
// target at the patch site.
static constexpr uint8_t kTestEax = 0xC0;
static constexpr uint8_t kTestEcx = 0xC9;

static uint8_t* g_site         = nullptr;
static uint8_t  g_siteOrig[8]  = {};
static size_t   g_siteLen      = 0;
static uint8_t* g_cave         = nullptr;

void ai_squad_order_null_fix_install(uintptr_t exe_base)
{
   const uint8_t* orig;
   size_t         origLen;
   uint8_t        testReg;

   switch (g_build) {
   case GameBuild::Modtools:
      orig = kOrigModtools; origLen = sizeof(kOrigModtools); testReg = kTestEax;
      break;
   case GameBuild::Steam:
   case GameBuild::GOG:
      orig = kOrigRetail;   origLen = sizeof(kOrigRetail);   testReg = kTestEcx;
      break;
   default:
      return; // unknown build
   }

   if (g_addr->ai_squad_order_guard_site == 0 ||
       g_addr->ai_squad_order_guard_resume == 0 ||
       g_addr->ai_squad_order_guard_skip == 0)
      return;

   uint8_t* site   = (uint8_t*)resolve(exe_base, g_addr->ai_squad_order_guard_site);
   uint8_t* resume = (uint8_t*)resolve(exe_base, g_addr->ai_squad_order_guard_resume);
   uint8_t* skip   = (uint8_t*)resolve(exe_base, g_addr->ai_squad_order_guard_skip);

   if (std::memcmp(site, orig, origLen) != 0) {
      get_gamelog()("[AISquadOrderNullFix] unexpected bytes at guard site, skipping\n");
      return;
   }

   uint8_t* cave = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT,
                                          PAGE_EXECUTE_READWRITE);
   if (!cave) return;

   //  +0            TEST reg,reg
   //  +2            JZ  -> the skip JMP
   //  +4            <the displaced instructions>
   //  +4+origLen    JMP resume (the dereference's continuation)
   //  +9+origLen    JMP skip   (the engine's "call returned false" path)
   int o = 0;
   cave[o++] = 0x85; cave[o++] = testReg;
   cave[o++] = 0x74; cave[o++] = (uint8_t)(origLen + 5);
   std::memcpy(cave + o, orig, origLen);
   o += (int)origLen;
   cave[o++] = 0xE9;
   *(int32_t*)(cave + o) = (int32_t)(resume - (cave + o + 4));
   o += 4;
   cave[o++] = 0xE9;
   *(int32_t*)(cave + o) = (int32_t)(skip - (cave + o + 4));
   o += 4;

   // JMP cave + NOP padding to fill the site exactly.
   std::memcpy(g_siteOrig, site, origLen);
   site[0] = 0xE9;
   *(int32_t*)(site + 1) = (int32_t)(cave - (site + 5));
   std::memset(site + 5, 0x90, origLen - 5);

   g_site    = site;
   g_siteLen = origLen;
   g_cave    = cave;
}

void ai_squad_order_null_fix_uninstall()
{
   // Sections are re-protected by the time this runs, so the restore cannot be
   // a plain write.
   if (g_site) {
      protected_write(g_site, g_siteOrig, g_siteLen);
      g_site = nullptr;
   }
   if (g_cave) {
      VirtualFree(g_cave, 0, MEM_RELEASE);
      g_cave = nullptr;
   }
}
