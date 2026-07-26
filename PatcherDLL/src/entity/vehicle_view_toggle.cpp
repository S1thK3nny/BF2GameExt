#include "pch.h"
#include "vehicle_view_toggle.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

struct SlotPatch {
   uintptr_t slot_va;        // vtable+0x3C slot address (unrelocated)
   uintptr_t expected_thunk; // original thunk address we expect to find (unrelocated)
   uint32_t* slot_ptr;       // resolved slot pointer (set at install)
   uint32_t  orig_value;     // saved original for uninstall
};

static SlotPatch g_slots[4];
static int       g_slot_count = 0;

void vehicle_view_toggle_install(uintptr_t exe_base)
{
   // Every address here is in the per-build table, so there is nothing
   // build-specific left to branch on — the slot layout (hover / walker and
   // their Command* twins, each patched to the const-false thunk) is the same
   // on all three builds.
   if (g_build == GameBuild::Unknown) return;

   if (!g_addr->veh_view_hover_vtable_3c_slot || !g_addr->veh_view_walker_vtable_3c_slot ||
       !g_addr->veh_view_cmd_hover_vtable_3c_slot || !g_addr->veh_view_cmd_walker_vtable_3c_slot ||
       !g_addr->veh_view_hover_3c_orig_thunk || !g_addr->veh_view_walker_3c_orig_thunk ||
       !g_addr->veh_view_return_false_thunk)
      return;

   g_slots[0] = { g_addr->veh_view_hover_vtable_3c_slot,      g_addr->veh_view_hover_3c_orig_thunk,  nullptr, 0 };
   g_slots[1] = { g_addr->veh_view_walker_vtable_3c_slot,     g_addr->veh_view_walker_3c_orig_thunk, nullptr, 0 };
   g_slots[2] = { g_addr->veh_view_cmd_hover_vtable_3c_slot,  g_addr->veh_view_hover_3c_orig_thunk,  nullptr, 0 };
   g_slots[3] = { g_addr->veh_view_cmd_walker_vtable_3c_slot, g_addr->veh_view_walker_3c_orig_thunk, nullptr, 0 };
   g_slot_count = 4;

   const uint32_t new_value = (uint32_t)(uintptr_t)resolve(exe_base, g_addr->veh_view_return_false_thunk);

   for (int i = 0; i < g_slot_count; i++) {
      SlotPatch& s = g_slots[i];
      uint32_t* slot = (uint32_t*)resolve(exe_base, s.slot_va);
      const uint32_t expected = (uint32_t)(uintptr_t)resolve(exe_base, s.expected_thunk);
      if (*slot != expected) continue;

      s.slot_ptr = slot;
      s.orig_value = *slot;
      *slot = new_value;
   }
}

void vehicle_view_toggle_uninstall()
{
   for (int i = 0; i < g_slot_count; i++) {
      SlotPatch& s = g_slots[i];
      if (!s.slot_ptr) continue;
      protected_write(s.slot_ptr, &s.orig_value, sizeof(s.orig_value)); // sections re-protected by now
      s.slot_ptr = nullptr;
   }
}
