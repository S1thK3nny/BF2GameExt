#include "pch.h"
#include "map_queue_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstring>

// =============================================================================
// Map-queue next-mission fix — modtools only.
//
// When the post-match stats screen finishes, GameLoop::UpdateStats asks
// MissionPlayList::SelectNextEntry() for the next map.  SelectNextEntry does the
// whole job on every build: it advances sPlaylistPosn (honouring LoopForever and
// Randomize), calls GameLoop::SetNextMission(entry.mLuaFile), pushes the side and
// team hashes, runs PushToLua, and returns true when it queued something.
//
// Phantom (0x005d0c91) and Steam (0x00533343) then act on that result:
//
//     CALL SelectNextEntry
//     TEST AL,AL
//     JZ   shell                  ; nothing queued -> main menu
//     PUSH 0x8ff60339             ; MissionState
//     CALL GameState::SetState
//     ADD  ESP,4
//     POP  <saved reg>
//     RET
//   shell:
//     ...drain postLoadHostGame / postLoadJoinGame...
//     PUSH 0x11e1fc01             ; ShellState
//     CALL GameState::SetState
//
// The modtools build (0x0073309c) is compiled from an older GameLoop.cpp in which
// that first branch does not exist at all:
//
//     CALL SelectNextEntry
//     TEST AL,AL
//     JNZ  0x007330d2             ; queued a map -> jumps STRAIGHT to the shell
//     ...drain postLoadHostGame / postLoadJoinGame...
//   0x007330d2:
//     PUSH 0x11e1fc01             ; ShellState — the only outcome, either way
//
// So the playlist really does advance (SetNextMission has already stashed the
// next script name), but the state machine is told to go to the shell regardless
// and you get dumped to the main menu between every map.
//
// Nothing else is missing: modtools GameState::SetState (0x00401af0) still maps
// 0x8ff60339 to the MissionState object, and MissionState::Enter is the same one
// dlc_mission_init_fix already hooks.  Only the caller branch is absent, so the
// fix is to re-emit it.
//
// Implementation: a 5-byte JMP over `TEST AL,AL / JNZ` into a stub that
// reimplements the whole tail the way retail does.  The overwritten window also
// eats the first byte of the following `MOV ECX,[postLoadHostGame]`, which is
// fine — that instruction is not a branch target from anywhere (the only inbound
// edges in this region are 0x007330a3 -> 0x007330d2, 0x007330ad -> 0x007330c3 and
// 0x007330cb -> 0x007330d2), and the stub reimplements it rather than falling
// back into it.  The remaining orphan bytes are NOPed so the disassembly stays
// readable.
//
// Not applicable to Steam/GOG: both already have the branch.  Not INI-gated:
// a missing branch is a defect, not a preference.
// =============================================================================

// Resolved runtime addresses used by the stub.  ILT thunk for GameState::SetState
// (__cdecl, one hash argument), and the addresses OF the two post-load globals
// (each holds a NetPostLoad*Game* — the stub dereferences them, matching the
// original `MOV ECX,dword ptr [0x00c69268]`).
static uintptr_t s_setState          = 0;
static uintptr_t s_pPostLoadHostGame = 0;
static uintptr_t s_pPostLoadJoinGame = 0;

// Patch bookkeeping.
static uint8_t* s_site     = nullptr;
static uint8_t  s_orig[10] = {};

// GameState name hashes, as pushed by Phantom and retail:
//   0x8ff60339 -> MissionState, 0x11e1fc01 -> ShellState.
// Written as literals in the stub below so they are plain immediates rather than
// memory operands on named constants.

// Tail of GameLoop::UpdateStats, re-emitted with the missing MissionState branch.
//
// Entry state, inherited from the patched site: AL = SelectNextEntry's result,
// ESP -> the ESI the function pushed at 0x00732ee4 (the function's two float args
// sit above the return address and are caller-cleaned, hence the plain RET).
// ESI is the only live callee-saved register, and every exit pops it exactly as
// the original code at 0x007330c1 / 0x007330df does.
__declspec(naked) static void updatestats_playlist_tail()
{
   __asm {
      test al, al
      jz   shell_path

      // Playlist queued another map: SelectNextEntry has already run
      // GameLoop::SetNextMission, so just enter MissionState and bail out.
      push 0x8ff60339             // MissionState
      mov  eax, [s_setState]
      call eax
      add  esp, 4
      pop  esi
      ret

   shell_path:
      // Original 0x007330a5..0x007330d1 — drain whichever post-load game object
      // is pending (vtable slot 1, __thiscall, no args), then fall into the shell.
      mov  eax, [s_pPostLoadHostGame]
      mov  ecx, [eax]
      test ecx, ecx
      jz   try_join
      mov  edx, [ecx]
      call dword ptr [edx + 4]
      jmp  do_shell

   try_join:
      mov  eax, [s_pPostLoadJoinGame]
      mov  ecx, [eax]
      test ecx, ecx
      jz   do_shell
      mov  eax, [ecx]
      call dword ptr [eax + 4]

   do_shell:
      push 0x11e1fc01             // ShellState
      mov  eax, [s_setState]
      call eax
      add  esp, 4
      pop  esi
      ret
   }
}

void map_queue_fix_install(uintptr_t exe_base)
{
   // Modtools-only defect; Steam and GOG already emit the branch.
   if (g_build != GameBuild::Modtools) return;

   const uintptr_t siteVA = game_addrs::modtools::updatestats_playlist_branch;
   if (siteVA == 0) return;

   uint8_t* site = (uint8_t*)resolve(exe_base, siteVA);

   // Positive-ID the site before touching it, so a wrong address on an
   // un-derived build no-ops instead of corrupting code:
   //   site[-5]     : `E8` — the CALL to MissionPlayList::SelectNextEntry.
   //   site[0..9]   : TEST AL,AL / JNZ +0x2D / MOV ECX,[postLoadHostGame].
   //                  The encoded global address makes this near-unique.
   //   site[0x31]   : `68 01 FC E1 11` — the shell PUSH the JNZ targets.
   static const uint8_t kSite[] = {
      0x84, 0xC0,                          // TEST AL,AL
      0x75, 0x2D,                          // JNZ  site+0x31
      0x8B, 0x0D, 0x68, 0x92, 0xC6, 0x00,  // MOV  ECX,[0x00c69268]
   };
   static const uint8_t kShellPush[] = {0x68, 0x01, 0xFC, 0xE1, 0x11}; // PUSH 0x11e1fc01

   if (site[-5] != 0xE8) return;
   if (std::memcmp(site, kSite, sizeof(kSite)) != 0) return;
   if (std::memcmp(site + 0x31, kShellPush, sizeof(kShellPush)) != 0) return;

   s_setState          = (uintptr_t)resolve(exe_base, game_addrs::modtools::gamestate_set_state);
   s_pPostLoadHostGame = (uintptr_t)resolve(exe_base, game_addrs::modtools::post_load_host_game);
   s_pPostLoadJoinGame = (uintptr_t)resolve(exe_base, game_addrs::modtools::post_load_join_game);

   s_site = site;
   std::memcpy(s_orig, site, sizeof(s_orig));

   // E9 rel32 to the stub over bytes 0..4, NOP the 5 orphaned bytes of the
   // clipped MOV.  .text is RW during install (dllmain re-protects afterwards).
   const int32_t rel = (int32_t)((uintptr_t)&updatestats_playlist_tail - ((uintptr_t)site + 5));
   site[0] = 0xE9;
   *(int32_t*)(site + 1) = rel;
   std::memset(site + 5, 0x90, sizeof(s_orig) - 5);
   // No logging here: install runs with the exe's sections non-executable, so
   // calling game code (RedWarning::LogMessage) would fault.
}

void map_queue_fix_uninstall()
{
   if (!s_site) return;

   protected_write(s_site, s_orig, sizeof(s_orig));
   s_site = nullptr;
}
