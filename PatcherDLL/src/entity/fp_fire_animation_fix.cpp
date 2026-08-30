#include "pch.h"
#include "fp_fire_animation_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstring>

// =============================================================================
// "First shot in first person plays no animation" fix.
//
// FirstPersonRenderable::UpdateSoldier (modtools 0x4A9BE0, retail 0x51FB70) is
// the FP animation state machine.  The shoot animation is edge-triggered:
//
//   Weapon::SignalFire (modtools 0x61C870) sets Weapon+0xAC bit 1, mFiredFlag,
//   once per shot.  UpdateSoldier's FIRE/FIRE2 branch requires that bit, sets
//   the soldier state to 2 or 3 (shoot / shoot2), and *clears the bit* - the
//   request is consumed on the spot.
//
// Further down the same call, after the state has been chosen, comes:
//
//     if (bTransitionAllowed && fTransitionTimer < fTransitionTimerMax) {
//         soldierState  = this->mTransitionAnim;   <-- overwrites the shoot state
//         fTransitionTimer += sDeltaTime;
//     }
//
// bTransitionAllowed starts true and is only cleared by the JUMP and FLAIL
// branches, which the fire branch jumps clean past - so it is still true when a
// shot is being animated.  The state is replaced and mFiredFlag is already
// spent, so the shot's animation is lost, not merely delayed.
//
// What arms the transition on entering first person:
//
//   FirstPersonRenderable::Activate (modtools 0x4A74E0, vtable slot 1) sets
//   mBlendHandsDown = true and bReadyToRender = false.  UpdateSoldier consumes
//   that latch with fTransitionTimerMax = 1.0 and mTransitionAnim = 0 (idle),
//   i.e. a one second hands-down -> idle blend during which every shot fired is
//   swallowed.  Jump landings (0.5s -> land) and flailing (1.0s -> handsdown)
//   arm the same block, so they drop shots the same way.
//
// The fix exempts the shoot states from the override.  2 and 3 are produced by
// nothing but the fire branch (`state = (mState != FIRE) + 2`); the movement
// default yields 0/1, jump 7, flail 8, reload 5, and every mTransitionAnim the
// engine ever stores is 0, 9 or 10.  So testing for 2/3 is exact.  The timer is
// left advancing, so the blend still expires on schedule around the shot.
//
// Codegen at the patch site.  Both builds are the same instruction with a
// different register allocation, and both are exactly 6 bytes:
//
//   modtools 0x4A9F32:  8B AE 20 16 00 00   MOV EBP,[ESI+0x1620]   state=EBP, this=ESI
//     resume 0x4A9F38:  D8 86 18 16 00 00   FADD [ESI+0x1618]
//
//   Steam/GOG 0x51FEFC: 8B 9F 20 16 00 00   MOV EBX,[EDI+0x1620]   state=EBX, this=EDI
//     resume 0x51FF02:  F3 0F 11 8F 18 16 00 00  MOVSS [EDI+0x1618],XMM1
//
// The site is displaced into a code cave that skips the load for states 2 and 3.
// The cave is integer-only, so the in-flight x87 stack top (modtools, between
// FLD sDeltaTime and FADD) and XMM1 (retail, holding the advanced timer) are
// both untouched.  EFLAGS is dead across the site on both builds - the next
// reader is the fresh `CMP byte ptr [this+0x1624],..` of the mBlendHandsDown
// block - so the cave's CMPs are free to clobber it.
//
// Residual, documented and deliberately not patched: the mBlendHandsDown block
// immediately after the site also forces state 9 unconditionally.  It runs on
// exactly one frame (the flag is cleared as it fires), so it can still eat a
// shot if the two coincide - a one-frame race instead of a one-second window.
// Protecting the shoot state there too would push the hands-down blend a frame
// later on entering FP, which is a visible change for a 1-in-60 case.
// =============================================================================

static uint8_t* g_site        = nullptr;
static uint8_t  g_siteOrig[8] = {};
static size_t   g_siteLen     = 0;
static uint8_t* g_cave        = nullptr;

// The displaced transition-anim load, per build.  MOV r32,[r32+0x1620].
static const uint8_t kOrigModtools[] = {0x8B, 0xAE, 0x20, 0x16, 0x00, 0x00}; // MOV EBP,[ESI+0x1620]
static const uint8_t kOrigRetail[]   = {0x8B, 0x9F, 0x20, 0x16, 0x00, 0x00}; // MOV EBX,[EDI+0x1620]

// The timer store that must follow it, to positively identify the site.
static const uint8_t kNextModtools[] = {0xD8, 0x86, 0x18, 0x16, 0x00, 0x00};             // FADD [ESI+0x1618]
static const uint8_t kNextRetail[]   = {0xF3, 0x0F, 0x11, 0x8F, 0x18, 0x16, 0x00, 0x00}; // MOVSS [EDI+0x1618],XMM1

// FP soldier animation states produced by the fire branch, and by nothing else.
static constexpr uint8_t kStateShoot  = 2;
static constexpr uint8_t kStateShoot2 = 3;

void fp_fire_animation_fix_install(uintptr_t exe_base)
{
   const uint8_t* orig;
   size_t         origLen;
   const uint8_t* next;
   size_t         nextLen;

   switch (g_build) {
   case GameBuild::Modtools:
      orig = kOrigModtools; origLen = sizeof(kOrigModtools);
      next = kNextModtools; nextLen = sizeof(kNextModtools);
      break;
   case GameBuild::Steam:
   case GameBuild::GOG:
      orig = kOrigRetail;   origLen = sizeof(kOrigRetail);
      next = kNextRetail;   nextLen = sizeof(kNextRetail);
      break;
   default:
      return; // unknown build
   }

   if (g_addr->fp_transition_anim_override == 0) return;

   uint8_t* site = (uint8_t*)resolve(exe_base, g_addr->fp_transition_anim_override);

   // Bail (no-op) unless both the load and the timer store that follows it match.
   if (std::memcmp(site, orig, origLen) != 0 ||
       std::memcmp(site + origLen, next, nextLen) != 0) {
      get_gamelog()("[FPFireAnimFix] unexpected bytes at the FP transition override, skipping\n");
      return;
   }

   // The state register is the MOV's reg field; turn it into the modrm for
   // `CMP r32,imm8` (opcode 0x83 /7, mod=11).  Derived from the bytes we just
   // verified rather than hardcoded per build.
   const uint8_t stateReg = (uint8_t)((orig[1] >> 3) & 7);
   const uint8_t cmpModrm = (uint8_t)(0xC0 | (7 << 3) | stateReg);

   uint8_t* cave = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT,
                                          PAGE_EXECUTE_READWRITE);
   if (!cave) return;

   uint8_t* const resume = site + origLen;

   //  +0            CMP state,2
   //  +3            JZ  -> skip
   //  +5            CMP state,3
   //  +8            JZ  -> skip
   //  +10           <displaced MOV state,[this+0x1620]>
   //  +10+origLen   JMP resume          <- skip
   int o = 0;
   cave[o++] = 0x83; cave[o++] = cmpModrm; cave[o++] = kStateShoot;
   cave[o++] = 0x74; cave[o++] = (uint8_t)(3 + 2 + origLen); // over CMP+JZ+MOV
   cave[o++] = 0x83; cave[o++] = cmpModrm; cave[o++] = kStateShoot2;
   cave[o++] = 0x74; cave[o++] = (uint8_t)origLen;           // over MOV
   std::memcpy(cave + o, orig, origLen);
   o += (int)origLen;
   cave[o++] = 0xE9;
   *(int32_t*)(cave + o) = (int32_t)(resume - (cave + o + 4));
   o += 4;

   // JMP cave + NOP padding to fill the site exactly.  .text is RW during
   // install (dllmain re-protects afterwards), so no VirtualProtect here.
   std::memcpy(g_siteOrig, site, origLen);
   site[0] = 0xE9;
   *(int32_t*)(site + 1) = (int32_t)(cave - (site + 5));
   std::memset(site + 5, 0x90, origLen - 5);

   g_site    = site;
   g_siteLen = origLen;
   g_cave    = cave;
}

void fp_fire_animation_fix_uninstall()
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
