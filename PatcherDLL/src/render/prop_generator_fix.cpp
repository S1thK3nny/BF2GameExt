#include "pch.h"
#include "prop_generator_fix.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <cstring>

// Retargets the layer reset CALL in PropGenerator::Cleanup at a stub that also
// clears the layer's RedLodData* at +0x90. See prop_generator_fix.hpp.

namespace {

// Stock layer reset body on every build: MOV dword [ECX+0x80],0 ; RET
constexpr uint8_t kLayerResetBody[] = {0xC7, 0x81, 0x80, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0xC3};

uintptr_t s_origLayerReset = 0;
uint8_t*  s_relSite        = nullptr; // rel32 operand of the patched CALL
int32_t   s_relOrig        = 0;

// ECX = layer. Register and flag transparent, like the original.
__declspec(naked) void layer_reset_stub()
{
   __asm {
      mov dword ptr [ecx + 0x90], 0
      jmp dword ptr [s_origLayerReset]
   }
}

} // namespace

void prop_generator_fix_install(uintptr_t exe_base)
{
   if (g_addr->prop_generator_cleanup_layer_reset_call == 0) return;

   uint8_t* const call =
      (uint8_t*)resolve(exe_base, g_addr->prop_generator_cleanup_layer_reset_call);

   if (*call != 0xE8) {
      install_log("[PropGeneratorFix] site %08X reads %02X, expected E8 -- left stock",
                  (unsigned)g_addr->prop_generator_cleanup_layer_reset_call, *call);
      return;
   }

   const int32_t relOrig = *(int32_t*)(call + 1);
   uint8_t*      target  = call + 5 + relOrig;

   // Modtools calls the reset through an incremental-link JMP thunk.
   if (*target == 0xE9) target = target + 5 + *(int32_t*)(target + 1);

   if (std::memcmp(target, kLayerResetBody, sizeof(kLayerResetBody)) != 0) {
      install_log("[PropGeneratorFix] site %08X does not call the layer reset -- left stock",
                  (unsigned)g_addr->prop_generator_cleanup_layer_reset_call);
      return;
   }

   s_origLayerReset = (uintptr_t)target;
   s_relSite        = call + 1;
   s_relOrig        = relOrig;
   *(int32_t*)s_relSite = (int32_t)((intptr_t)&layer_reset_stub - (intptr_t)(call + 5));
}

void prop_generator_fix_uninstall()
{
   // Sections are re-protected by now, so this cannot be a plain store.
   if (s_relSite) protected_write(s_relSite, &s_relOrig, sizeof(s_relOrig));
   s_relSite = nullptr;
}
