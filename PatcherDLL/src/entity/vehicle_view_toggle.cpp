#include "pch.h"
#include "vehicle_view_toggle.hpp"
#include "core/resolve.hpp"

using namespace game_addrs::modtools;

struct SlotPatch {
   uintptr_t slot_va;        // vtable+0x3C slot address (unrelocated)
   uintptr_t expected_thunk; // original thunk address we expect to find (unrelocated)
   uint32_t* slot_ptr;       // resolved slot pointer (set at install)
   uint32_t  orig_value;     // saved original for uninstall
};

static SlotPatch g_slots[] = {
   { veh_view_hover_vtable_3c_slot,      veh_view_hover_3c_orig_thunk,  nullptr, 0 },
   { veh_view_walker_vtable_3c_slot,     veh_view_walker_3c_orig_thunk, nullptr, 0 },
   { veh_view_cmd_hover_vtable_3c_slot,  veh_view_hover_3c_orig_thunk,  nullptr, 0 },
   { veh_view_cmd_walker_vtable_3c_slot, veh_view_walker_3c_orig_thunk, nullptr, 0 },
};

void vehicle_view_toggle_install(uintptr_t exe_base)
{
   const uint32_t new_value =
      (uint32_t)(uintptr_t)resolve(exe_base, veh_view_return_false_thunk);

   for (SlotPatch& s : g_slots) {
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
   for (SlotPatch& s : g_slots) {
      if (!s.slot_ptr) continue;
      *s.slot_ptr = s.orig_value;
      s.slot_ptr = nullptr;
   }
}
