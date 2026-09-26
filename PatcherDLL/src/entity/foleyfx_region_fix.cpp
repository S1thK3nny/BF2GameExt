#include "pch.h"
#include "foleyfx_region_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/resolve.hpp"

// See the header. The list head is a single pointer; an empty list points at
// itself, which is also its static initial value on every build.
void foleyfx_region_reset()
{
   if (g_addr->foleyfx_region_list == 0) return; // not derived for this build

   void** head = (void**)resolve((uintptr_t)GetModuleHandleW(nullptr), g_addr->foleyfx_region_list);
   *head = head;
}
