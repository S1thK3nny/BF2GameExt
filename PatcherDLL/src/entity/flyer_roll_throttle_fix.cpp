#include "pch.h"
#include "flyer_roll_throttle_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "core/x86_emit.hpp"
#include "util/install_log.hpp"

#include <cstring>

// See flyer_roll_throttle_fix.hpp for the mechanism.

bool g_flyerRollThrottleFix = true;

namespace {

constexpr uint32_t kCtrl_Trackable   = 0x18;  // a Controllable's Trackable part
constexpr uint32_t kVt_GetGameObject = 0x1C;  // Trackable vtable
constexpr uint32_t kVt_IsRtti        = 0x00;  // GameObject primary vtable
constexpr uint32_t kFlyerRtti        = pbl_hash("EntityFlyer");

using ObjectFn = uint8_t*(__thiscall*)(void* self);
using IsRttiFn = bool(__thiscall*)(void* self, uint32_t hash);

// The compare and the branch past the scaling, and what comes either side.
const uint8_t kSiteModtools[] = {
   0xD8, 0x15, 0x90, 0xA2, 0xA2, 0x00,          // FCOM [1.0]
   0xDF, 0xE0,                                  // FNSTSW AX
   0xF6, 0xC4, 0x41,                            // TEST AH,0x41
   0x75, 0x1A,                                  // JNE: not past the rim
};
const uint8_t kBeforeModtools[] = { 0xDE, 0xC1, 0xD9, 0xFA };               // FADDP ST(1); FSQRT
const uint8_t kAfterModtools[]  = { 0xD8, 0x3D, 0x90, 0xA2, 0xA2, 0x00 };   // FDIVR [1.0]

const uint8_t kSiteSteam[] = {
   0x0F, 0x2F, 0x0D, 0x04, 0x21, 0x7B, 0x00,    // COMISS XMM1,[1.0]
   0x76, 0x1C,                                  // JBE: not past the rim
};
const uint8_t kSiteGog[] = {
   0x0F, 0x2F, 0x0D, 0x7C, 0x30, 0x7B, 0x00,
   0x76, 0x1C,
};
const uint8_t kBeforeRelease[] = { 0x0F, 0x57, 0xC9, 0xF2, 0x0F, 0x5A, 0xC8 };   // XORPS XMM1,XMM1; CVTSD2SS XMM1,XMM0
const uint8_t kAfterRelease[]  = { 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x10,           // MOVSS XMM0,[ESP+0x10]
                                   0xF3, 0x0F, 0x5E, 0xC1 };                     // DIVSS XMM0,XMM1

// Where the 1.0's address sits in those. Steam and GOG move it with the exe
// (each has a relocation on it), so it is compared where the loader put it.
constexpr size_t kOneModtools = 2;   // FCOM [1.0], FDIVR [1.0]
constexpr size_t kOneRelease  = 3;   // COMISS XMM1,[1.0]
static_assert(sizeof kSiteModtools <= 16 && sizeof kSiteSteam <= 16 && sizeof kAfterModtools <= 16,
              "matches() copies into 16 bytes");

float    s_one   = 1.0f;     // the compare the shims stand in for
void*    s_scale = nullptr;  // the original scaling, after the compare
void*    s_keep  = nullptr;  // where the pair goes on as it came
bool     s_skip  = false;    // this call's answer, read after the registers come back
uint8_t* s_site  = nullptr;
uint8_t  s_orig[sizeof(kSiteModtools)] = {};
size_t   s_len   = 0;

// Whether the code at `at` is `bytes`, with the 1.0's address at `one` as the
// loader left it.
bool matches(const uint8_t* at, const uint8_t* bytes, size_t len, size_t one, uintptr_t base)
{
   uint8_t want[16];
   std::memcpy(want, bytes, len);
   rebase_operand(want, one, base);
   return std::memcmp(at, want, len) == 0;
}

// Whether `owner`, the Controllable a PlayerController drives, is a flyer.
bool __cdecl is_flyer(void* owner)
{
   if (!owner) return false;
   __try {
      uint8_t* trackable = static_cast<uint8_t*>(owner) + kCtrl_Trackable;
      uint8_t* obj = reinterpret_cast<ObjectFn>((*reinterpret_cast<void***>(trackable))[kVt_GetGameObject / 4])(trackable);
      return obj && reinterpret_cast<IsRttiFn>((*reinterpret_cast<void***>(obj))[kVt_IsRtti / 4])(obj, kFlyerRtti);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// modtools: EBX = this, ST0 = the pair's length. The length waits in memory
// across the call, which expects the x87 stack empty, and comes back for the
// original compare and scaling. Every register comes back as it was.
__declspec(naked) void cap_shim_modtools()
{
   __asm {
      sub    esp, 12
      fstp   tbyte ptr [esp]
      pushad
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      push   dword ptr [ebx + 4]          // mOwner
      call   is_flyer
      add    esp, 4
      mov    s_skip, al
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      add    esp, 0x80
      popad
      fld    tbyte ptr [esp]
      add    esp, 12
      cmp    s_skip, 0
      jne    keep
      fcom   s_one                        // the original: scale only past the rim
      fnstsw ax
      test   ah, 0x41
      jne    keep
      jmp    [s_scale]
   keep:
      jmp    [s_keep]                     // FSTP ST(0), then the stores
   }
}

// Steam and GOG: EDI = this, XMM1 = the pair's length. The helper may use
// any XMM register, so all eight are kept.
__declspec(naked) void cap_shim_release()
{
   __asm {
      pushad
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      push   dword ptr [edi + 4]          // mOwner
      call   is_flyer
      add    esp, 4
      mov    s_skip, al
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      add    esp, 0x80
      popad
      cmp    s_skip, 0
      jne    keep
      comiss xmm1, s_one                  // the original: scale only past the rim
      jbe    keep
      jmp    [s_scale]
   keep:
      jmp    [s_keep]                     // the stores, with the pair as it came
   }
}

} // namespace

void flyer_roll_throttle_fix_install(uintptr_t exe_base)
{
   if (!g_flyerRollThrottleFix) return;
   if (g_addr->player_controller_input_cap == 0) return;

   const uint8_t* site;
   const uint8_t* before;
   const uint8_t* after;
   size_t siteLen, beforeLen, afterLen;
   void* shim;
   const bool modtools = g_build == GameBuild::Modtools;
   if (modtools) {
      site = kSiteModtools;    siteLen = sizeof(kSiteModtools);
      before = kBeforeModtools; beforeLen = sizeof(kBeforeModtools);
      after = kAfterModtools;   afterLen = sizeof(kAfterModtools);
      shim = (void*)&cap_shim_modtools;
   } else {
      site = g_build == GameBuild::GOG ? kSiteGog : kSiteSteam;
      siteLen = sizeof(kSiteSteam);
      before = kBeforeRelease; beforeLen = sizeof(kBeforeRelease);
      after = kAfterRelease;   afterLen = sizeof(kAfterRelease);
      shim = (void*)&cap_shim_release;
   }

   uint8_t* at = (uint8_t*)resolve(exe_base, g_addr->player_controller_input_cap);
   const bool same = matches(at, site, siteLen, modtools ? kOneModtools : kOneRelease, exe_base) &&
                     std::memcmp(at - beforeLen, before, beforeLen) == 0 &&
                     (modtools ? matches(at + siteLen, after, afterLen, kOneModtools, exe_base)
                               : std::memcmp(at + siteLen, after, afterLen) == 0);
   if (!same) {
      install_log("[FlyerRollThrottle] PlayerController::Update at %08X does not match -- left stock",
                  (unsigned)g_addr->player_controller_input_cap);
      return;
   }

   // The branch's rel8 is its last byte: past it is the stores, with the pair
   // as it came; straight on is the scaling.
   s_scale = at + siteLen;
   s_keep  = at + siteLen + (int8_t)at[siteLen - 1];
   s_site  = at;
   s_len   = siteLen;
   std::memcpy(s_orig, at, siteLen);

   // .text is RW during install.
   x86::write_branch(at, x86::kJmp, shim, siteLen);
   install_log("[FlyerRollThrottle] installed at %08X: flyers keep full throttle and roll together",
               (unsigned)g_addr->player_controller_input_cap);
}

void flyer_roll_throttle_fix_uninstall()
{
   // Sections are re-protected by now, so this cannot be a plain store.
   if (s_site) protected_write(s_site, s_orig, s_len);
   s_site = nullptr;
}
