#pragma once

#include <stdint.h>

// =============================================================================
// CommandPost::SetTeam null-this guard.
//
// Observed crash (modtools, normal play, no round transition):
//
//   EXCEPTION C0000005 ACCESS_VIOLATION  EIP=0074D473   AV: READ addr 00001A24
//   ECX=00001A24 ESI=00001A24 EBP=00001A24
//
//   CommandPost::SetTeam  0064fc48  LEA EBP,[ESI + 0x1a24]   ; ESI = this = NULL
//                         0064fc52  CALL 0x00402ba3          ; -> Stop(handle, bool)
//                         0074d473  CMP word ptr [ESI],0x0   ; faults on 0x1A24
//
// `this` was NULL, so the embedded sound object at `this + 0x1A24` came out as
// the bare offset 0x1A24, and Stop() dereferenced it.  SetTeam stops the
// command post's capture sound before applying the new team, so the very first
// thing it touches on the object is enough to fault.  The engine calls SetTeam from Lua mission callbacks and
// from the capture-region update, and neither checks that the object it
// resolved is really a CommandPost -- the same session logged
//
//   LuaCallbacks_Mission.cpp(642) Entity "rep_pod2_holo" is EntityProp, but need GameObject
//
// which is that resolution failing out loud for other entities.
//
// The guard turns the crash into a log line naming the situation, so a modder
// can find the offending script reference instead of losing the session.  It
// does NOT paper over the content bug -- the post still will not change team;
// it just does not take the process down.
//
// modtools only for now.  The retail builds lay CommandPost out differently
// (the modtools `LEA reg,[reg+0x1A24]` byte pattern does not occur in either
// retail image), so their SetTeam has to be re-derived rather than ported, and
// g_addr->command_post_set_team is 0 there -- the installer no-ops.
// =============================================================================

extern bool g_commandPostNullFixEnabled;

void command_post_null_fix_install(uintptr_t exe_base);
void command_post_null_fix_uninstall();
