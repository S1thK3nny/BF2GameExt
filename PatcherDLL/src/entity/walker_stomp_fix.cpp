#include "pch.h"
#include "walker_stomp_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <detours.h>

#include <cstring>

// See walker_stomp_fix.hpp for the mechanism.

bool g_walkerStompFix = true;

namespace {

using UpdateStateFn = void(__fastcall*)(uint8_t* self, void* edx, float dt, float arg2, float arg3,
                                        float* arg4, float arg5);

// Type 1's compares, each ending in the 0.1 constant's address.
const uint8_t kTestModtools[] = { 0xD8, 0x1D, 0x74, 0xC0, 0xA2, 0x00 };              // FCOMP [0x00A2C074]
const uint8_t kRearmSteam[]   = { 0x0F, 0x2F, 0x05, 0x60, 0x1F, 0x7B, 0x00 };        // COMISS XMM0,[0x007B1F60]
const uint8_t kLandSteam[]    = { 0xF3, 0x0F, 0x10, 0x0D, 0x60, 0x1F, 0x7B, 0x00 };  // MOVSS XMM1,[0x007B1F60]
const uint8_t kRearmGog[]     = { 0x0F, 0x2F, 0x05, 0xD8, 0x2E, 0x7B, 0x00 };        // COMISS XMM0,[0x007B2ED8]
const uint8_t kLandGog[]      = { 0xF3, 0x0F, 0x10, 0x0D, 0xD8, 0x2E, 0x7B, 0x00 };  // MOVSS XMM1,[0x007B2ED8]

UpdateStateFn s_updateState = nullptr;

// What type 1's compares read: BF2's 0.1 until a walker's update sets it.
float s_stompDrop = 0.1f;

// Where each compare names the constant, and what it named.
struct Operand {
   uint8_t* at   = nullptr;
   uint8_t  orig[4] = {};
};
Operand s_operands[2];
int     s_operandCount = 0;

void __fastcall hooked_UpdateState(uint8_t* self, void* edx, float dt, float arg2, float arg3, float* arg4,
                                   float arg5)
{
   s_stompDrop = walker_stomp_drop(dt, s_stompDrop);
   s_updateState(self, edx, dt, arg2, arg3, arg4, arg5);
}

bool guard(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[WalkerStompFix] NOT installed: prologue mismatch at %s 0x%08X", what, (unsigned)va);
         return false;
      }
   }
   return true;
}

// Notes the operand of the compare at `va`, if the code is `bytes` and the
// constant they end in holds 0.1.
bool take_operand(uintptr_t base, uintptr_t va, const uint8_t* bytes, size_t len)
{
   uint8_t* code = static_cast<uint8_t*>(resolve(base, va));
   if (std::memcmp(code, bytes, len) != 0) return false;
   uint32_t constant;
   std::memcpy(&constant, bytes + len - 4, sizeof constant);
   if (*static_cast<const float*>(resolve(base, constant)) != 0.1f) return false;
   Operand& op = s_operands[s_operandCount++];
   op.at = code + len - 4;
   std::memcpy(op.orig, op.at, sizeof op.orig);
   return true;
}

} // namespace

void walker_stomp_fix_install(uintptr_t base)
{
   if (!g_walkerStompFix) return;
   g_walkerStompFix = false;   // on again once installed
   const bool modtools = g_build == GameBuild::Modtools;
   const bool gog = g_build == GameBuild::GOG;
   if (!modtools && !gog && g_build != GameBuild::Steam) return;
   if (!g_addr->walker_update_state || !g_addr->walker_stomp_drop_site ||
       (!modtools && !g_addr->walker_stomp_drop_site2)) {
      install_log("[WalkerStompFix] NOT installed: no address set for this build");
      return;
   }
   // UpdateState's prologue, up to its reads of its arguments.
   if (!guard(base, g_addr->walker_update_state, "EntityWalker::UpdateState",
              modtools ? "\x83\xEC\x3C\xD9\x44\x24\x44\x8B\x44\x24\x40"
                       : "\x55\x8B\xEC\x83\xE4\xF8\x83\xEC\x48\xF3\x0F\x10\x5D\x0C",
              modtools ? "xxxxxxxxxxx" : "xxxxxxxxxxxxxx"))
      return;

   s_operandCount = 0;
   const bool found =
      modtools ? take_operand(base, g_addr->walker_stomp_drop_site, kTestModtools, sizeof kTestModtools)
               : take_operand(base, g_addr->walker_stomp_drop_site, gog ? kRearmGog : kRearmSteam,
                              gog ? sizeof kRearmGog : sizeof kRearmSteam) &&
                 take_operand(base, g_addr->walker_stomp_drop_site2, gog ? kLandGog : kLandSteam,
                              gog ? sizeof kLandGog : sizeof kLandSteam);
   if (!found) {
      s_operandCount = 0;
      install_log("[WalkerStompFix] NOT installed: DoFootImpactEffects' type 1 test (0x%08X) is not as expected",
                  (unsigned)g_addr->walker_stomp_drop_site);
      return;
   }

   s_updateState = reinterpret_cast<UpdateStateFn>(resolve(base, g_addr->walker_update_state));
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG r = DetourAttach(&(PVOID&)s_updateState, hooked_UpdateState);
   if (r != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
      if (r != NO_ERROR) DetourTransactionAbort();
      s_updateState = nullptr;
      s_operandCount = 0;
      install_log("[WalkerStompFix] NOT installed: detouring EntityWalker::UpdateState failed (%ld)", (long)r);
      return;
   }

   // .text is RW during install.
   const uint32_t ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_stompDrop));
   for (int i = 0; i < s_operandCount; ++i) std::memcpy(s_operands[i].at, &ours, sizeof ours);
   g_walkerStompFix = true;
   install_log("[WalkerStompFix] installed (UpdateState 0x%08X, type 1 test 0x%08X): type 1 walkers test a "
               "foot's speed, %.1f m/s, at any frame rate", (unsigned)g_addr->walker_update_state,
               (unsigned)g_addr->walker_stomp_drop_site, kWalkerStompSpeed);
}

void walker_stomp_fix_uninstall()
{
   // The compares first, so nothing reads the value once the detour is gone.
   // Sections are re-protected by now, so these cannot be plain stores.
   for (int i = 0; i < s_operandCount; ++i) protected_write(s_operands[i].at, s_operands[i].orig, 4);
   s_operandCount = 0;
   if (!s_updateState) return;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)s_updateState, hooked_UpdateState);
   DetourTransactionCommit();
   s_updateState = nullptr;
}
