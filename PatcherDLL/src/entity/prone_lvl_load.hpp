#pragma once

#include <stdint.h>

// =============================================================================
// Prone LVL auto-load
//
// The prone animations are not part of the stock asset set, so they have to be
// shipped separately.  Rather than requiring modders to inject human_5 into
// their ingame.lvl with LVLTool, this module loads a standalone prone.lvl
// alongside every ingame.lvl read.
//
// Hook point: the Lua "ReadDataFile" callback.  ingame.lvl isn't opened by the
// engine directly.  Every mission script calls ReadDataFile("ingame.lvl"), and
// the callback itself special-cases that exact name (stricmp) to run
// FirstPerson::Init and push the game_interface script.  We piggy-back on the
// same test: after the vanilla read completes we call LoadUtil::ReadDataFile
// again with "prone.lvl" (resolved to data\_lvl_pc\prone.lvl, same as any other
// level file).  That lands before the sides load, so anim_bank_append still
// picks human_5 up on the next AnimationFinder::_AddBank call.
//
// Interaction with [Features] Prone:
//   - Prone=0 in the INI  -> no prone.lvl read at all.
//   - prone.lvl missing   -> LoadUtil::ReadDataFile returns false and prone is
//                            switched off for that mission (it is re-evaluated
//                            on every ingame.lvl read, so a later mission that
//                            does find the file re-enables it).
//
// This file also owns the low-res half of the same asset story: prone.lvl
// carries a low-res donor bank whose prone animation is grafted into the stock
// humanlz bank so distant soldiers don't pop to a crouch pose.  See the graft
// section in the .cpp.
// =============================================================================

void prone_lvl_load_install(uintptr_t exe_base);
void prone_lvl_load_uninstall();
