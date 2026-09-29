#include "pch.h"
#include "hud_sub_pixel.hpp"
#ifndef HUD_SUB_PIXEL_TEST
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"
#endif

#include <string.h>

// =============================================================================
// Read on each build. Every interface element, HUD and menus alike, is drawn
// through one RedInterfaceElement draw, and a group draws its children through
// it from inside its own draw:
//
//                                     modtools    Steam       GOG
//   element draw                      0x00816FA0  0x006C0DE0  0x006C1E70
//   CALL floor, x                     0x00816FFE  0x006C0E43  0x006C1ED3
//   CALL floor, y                     0x00817014  0x006C0E66  0x006C1EF6
//   floor                             0x008D5020  0x0075317C  0x0075427C
//
// The draw is thiscall(const PblMatrix* parent, const RedColor*), RET 8. It
// multiplies the element's local matrix by its parent's, rounds the world
// translation's x and y to the nearest pixel as floor(v + 0.5), and draws the
// element with the rounded matrix, which a group then hands to its children.
// Modtools adds the 0.5 with FADD, retail with ADDSS, and both pass the sum to
// floor as a double: FSTP qword [esp] / CALL floor. floor is the CRT's own on
// modtools and the MSVCR120 import thunk on retail.
//
// With HudSubPixel on, both CALLs go to keep_fraction, which takes the 0.5
// back off and returns the coordinate unrounded. An element's own pieces are
// still placed on whole pixels relative to it: RedBitmapElement::SetRect and
// the text glyph layout round those once, when they are set, and never move
// the element. The interface samples textures bilinearly (MAG/MIN/MIP LINEAR,
// read on modtools and Phantom), which is what makes a fractional position
// look smooth in motion and slightly soft at rest.
//
// Phantom draws without this rounding: its RedInterfaceScreen::Render and
// RedGroupElement::RenderUsingContext inline the same draw with no floor.
// =============================================================================

namespace {

const double kHalf = 0.5;

// Stands in for floor(v) at both sites, where v is a coordinate plus 0.5:
// cdecl(double) -> ST0 like floor, returning v - 0.5, the coordinate with its
// fraction. Uses nothing but ST0.
__declspec(naked) double __cdecl keep_fraction(double)
{
   __asm {
      fld  qword ptr [esp + 4]
      fsub qword ptr [kHalf]
      ret
   }
}

} // namespace

#ifndef HUD_SUB_PIXEL_TEST

bool g_hudSubPixel = false;

namespace {

// Where each site's 0.5 comes from: the address operand of the add before the
// CALL, as an offset from it. Modtools adds with FADD dword [imm32] (D8 05),
// retail with ADDSS xmm0, dword [imm32] (F3 0F 58 05).
constexpr int kHalfXModtools = -0x1A;
constexpr int kHalfYModtools = -0x07;
constexpr int kHalfXRetail   = -0x12;
constexpr int kHalfYRetail   = -0x15;

bool guard(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[HudSubPixel] NOT installed: prologue mismatch at %s 0x%08X", what, (unsigned)va);
         return false;
      }
   }
   return true;
}

// A site is FSTP qword [esp] / CALL floor, with the draw's add of 0.5 at the
// given offset.
bool site_matches(uintptr_t base, uintptr_t site, int halfOperand, bool modtools)
{
   static const uint8_t kStoreArg[] = { 0xDD, 0x1C, 0x24 };
   static const uint8_t kFadd[]     = { 0xD8, 0x05 };
   static const uint8_t kAddss[]    = { 0xF3, 0x0F, 0x58, 0x05 };
   const auto* call = static_cast<const uint8_t*>(resolve(base, site));
   if (memcmp(call - 3, kStoreArg, sizeof(kStoreArg)) != 0 || call[0] != 0xE8) return false;
   int32_t rel;
   memcpy(&rel, call + 1, sizeof(rel));
   if ((uintptr_t)(call + 5) + rel != (uintptr_t)resolve(base, g_addr->crt_floor)) return false;
   const uint8_t* add = call + halfOperand - (modtools ? sizeof(kFadd) : sizeof(kAddss));
   if (modtools ? memcmp(add, kFadd, sizeof(kFadd)) != 0 : memcmp(add, kAddss, sizeof(kAddss)) != 0)
      return false;
   // The operand is an absolute address, already relocated where the exe was.
   const float* half;
   memcpy(&half, call + halfOperand, sizeof(half));
   return *half == 0.5f;
}

bool sites_match(uintptr_t base, uintptr_t x, uintptr_t y, bool modtools)
{
   __try {
      return site_matches(base, x, modtools ? kHalfXModtools : kHalfXRetail, modtools)
          && site_matches(base, y, modtools ? kHalfYModtools : kHalfYRetail, modtools);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

void retarget(uintptr_t base, uintptr_t site)
{
   uint8_t* call = static_cast<uint8_t*>(resolve(base, site));
   const int32_t rel = (int32_t)((uintptr_t)&keep_fraction - (uintptr_t)(call + 5));
   memcpy(call + 1, &rel, sizeof(rel));
}

} // namespace

void hud_sub_pixel_install(uintptr_t base)
{
   if (!g_hudSubPixel) return;
   // Back on only once the draw is patched: until then GameExt's own position
   // events keep rounding to whole pixels, as the draw does.
   g_hudSubPixel = false;
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   const uintptr_t x = g_addr->hud_element_draw_floor_x;
   const uintptr_t y = g_addr->hud_element_draw_floor_y;
   if (!g_addr->hud_element_draw || !x || !y || !g_addr->crt_floor) {
      install_log("[HudSubPixel] NOT installed: no address set for this build");
      return;
   }
   // The draw's prologue up to its enabled-flag test, [this+0x14] bit 0x100;
   // on retail the security cookie's address is skipped, since it is relocated.
   if (!guard(base, g_addr->hud_element_draw, "RedInterfaceElement draw",
              modtools ? "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\x94\x00\x00\x00\x53\x8B\xD9\x8B\x43\x14\xF6\xC4\x01"
                       : "\x53\x8B\xDC\x83\xEC\x08\x83\xE4\xF0\x83\xC4\x04\x55\x8B\x6B\x04\x89\x6C\x24\x04"
                         "\x8B\xEC\x83\xEC\x58\xA1\x00\x00\x00\x00\x33\xC5\x89\x45\xFC\x8B\x43\x08\x56\x57"
                         "\x8B\xF9\xF7\x47\x14\x00\x01\x00\x00",
              modtools ? "xxxxxxxxxxxxxxxxxxxxx"
                       : "xxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxx"))
      return;
   // Both or neither: rounding only one axis would be worse than either.
   if (!sites_match(base, x, y, modtools)) {
      install_log("[HudSubPixel] NOT installed: the draw's floor calls (0x%08X, 0x%08X) are not "
                  "as expected", (unsigned)x, (unsigned)y);
      return;
   }
   retarget(base, x);
   retarget(base, y);
   g_hudSubPixel = true;
   install_log("[HudSubPixel] installed: interface elements are drawn at their exact position "
               "(floor calls 0x%08X, 0x%08X)", (unsigned)x, (unsigned)y);
}

#endif // HUD_SUB_PIXEL_TEST
