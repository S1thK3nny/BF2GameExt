#include "pch.h"
#include "hud_editor_disable.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cstring>

// =============================================================================
// Retail HUD editor removal (Steam / GOG).
//
// The modtools build ships a complete in-game HUD layout editor: element list,
// property browser, live nudging, and a ".hud file" writer.  All of it is still
// linked into the retail executables -- HUD::Manager::Open constructs gEditor
// unconditionally, and only the 1 MB HUDEditHeap is gated (on __RedDebugHeap,
// which is -1 on retail).  So the toggle still works on Steam and GOG: a KEYCHAR
// of 0x12 with modifier bit 0 set reaches HUD::Editor::KeyboardEvent, which
// calls Editor::SetMode(2).
//
// SetMode(2) calls GameLoop::Pause() and enables the editor's screen group, but
// the editor is not actually drivable there, so the player ends up staring at a
// frozen game with a half-live dev tool over it.  Nothing in the shipped game
// wants this, so the entry point is removed.
//
// Where the patch goes.  Editor::SetMode has exactly two callers: Editor::Update
// (its own mode bookkeeping, which only ever re-applies the static sMode) and
// Editor::KeyboardEvent.  Neutering KeyboardEvent therefore closes the only door
// in, without touching the editor's construction or teardown -- important,
// because HUD::Manager::Close dereferences gEditor with no null check, so
// skipping the allocation in Open would turn a dead feature into a crash.
//
// With the key gone, sMode stays at its BSS 0 and the first Editor::Update after
// each Manager::Open drives mMode 1 -> 0 through SetMode(0) exactly as it already
// does today; Editor::Update's `mMode - 1` switch then falls through to its
// default arm every frame and the editor does nothing.
//
// The patch itself: overwrite the entry with the function's own epilogue.
//
//     55              PUSH EBP          ->   C2 14 00   RET 0x14
//     8B EC           MOV  EBP,ESP
//
// __thiscall with five stack dwords (RET 0x14 at every exit), and at the entry
// no frame has been set up yet, so returning straight away is well-formed.
//
// Addresses: docs/RE/HUDSystem.md.  Steam 0x00546E20 / GOG 0x00547B70, each the
// slot-2 entry of the Editor vtable named by the constructor's own vtable store
// (Steam 0x007A0578, GOG 0x007A13D4).  One vtable xref each, so neither is a
// COMDAT-folded body shared with some other class.
//
// Not INI-gated: this removes a dev leftover that only ever leaves the retail
// player stuck in a paused game.  The signature check below is the safety.
// =============================================================================

// HUD::Editor::KeyboardEvent prologue through the `CMP CL,0x12` that tests for
// the toggle key.  Byte-identical on Steam and GOG, and register/immediate only
// (no absolute operands), so one signature covers both builds.
//
//   55              PUSH EBP
//   8B EC           MOV  EBP,ESP
//   83 7D 08 02     CMP  dword ptr [EBP+0x8],0x2   ; event type == KEYCHAR
//   8B C1           MOV  EAX,ECX                   ; this
//   75 51           JNZ  <ret>
//   F6 45 18 01     TEST byte ptr [EBP+0x18],0x1   ; modifier bit 0
//   74 4B           JZ   <ret>
//   8A 4D 10        MOV  CL,byte ptr [EBP+0x10]    ; the character
//   80 F9 12        CMP  CL,0x12                   ; the editor toggle
static const uint8_t kKeyboardEventSig[] = {
   0x55, 0x8B, 0xEC, 0x83, 0x7D, 0x08, 0x02, 0x8B,
   0xC1, 0x75, 0x51, 0xF6, 0x45, 0x18, 0x01, 0x74,
   0x4B, 0x8A, 0x4D, 0x10, 0x80, 0xF9, 0x12,
};

// RET 0x14 -- the function's own stack cleanup, taken from its epilogues.
static const uint8_t kRet14[3] = {0xC2, 0x14, 0x00};

static uint8_t* s_site       = nullptr;
static uint8_t  s_orig[3]    = {};

void hud_editor_disable_install(uintptr_t exe_base)
{
   // Retail only.  The modtools editor is a working tool and stays.
   if (g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;

   const uintptr_t siteVA = g_addr->hud_editor_keyboard_event;
   if (siteVA == 0) return;

   uint8_t* site = (uint8_t*)resolve(exe_base, siteVA);

   // Positive-ID the handler before writing.  A wrong address on a build we have
   // not derived no-ops instead of corrupting whatever else lives there.
   if (std::memcmp(site, kKeyboardEventSig, sizeof(kKeyboardEventSig)) != 0) return;

   std::memcpy(s_orig, site, sizeof(s_orig));
   s_site = site;

   // .text is RW for the whole install window (dllmain re-protects afterwards).
   std::memcpy(site, kRet14, sizeof(kRet14));
}

void hud_editor_disable_uninstall()
{
   if (!s_site) return;
   protected_write(s_site, s_orig, sizeof(s_orig));
   s_site = nullptr;
}
