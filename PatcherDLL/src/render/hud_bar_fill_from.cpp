#include "pch.h"
#include "hud_bar_fill_from.hpp"
#include "hud_bar_fill_from_core.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <detours.h>

#include <string.h>
#include <utility>

// =============================================================================
// Read on each build, names from the Phantom PDB. Hooked: ReadData,
// PostReadSetup and, for the vertical modes, SetValue.
//
//                                     modtools    Steam       GOG
//   ElementBarBitmap::ReadData        0x00695900  0x0054B480  0x0054C1D0
//   ElementBarBitmap::PostReadSetup   0x00696340  0x0054B320  0x0054C070
//   ElementBarBitmap::SetValue        0x00696090  0x0054B070  0x0054BDC0
//   RedBitmapElement::GetRect         0x00838E50  0x006E4DB0  0x006E5E50
//   RedBitmapElement::GetTexCoords    0x008392A0  0x006E48F0  0x006E5990
//   RedBitmapElement::SetTexCoords    0x00839220  0x006E4B90  0x006E5C30
//
// ReadData is thiscall(PblConfig*, Data*) -> bool, RET 8; PostReadSetup is
// thiscall(), RET 0. SetValue is thiscall(float) -> float in ST0, RET 4, with
// `this` the ElementBar base at bar + 0x220 (its bitmap read is [this-0x170]).
// It only moves the right edge and U with flag bit 1 set, and crops U with
// bit 0 (ScaleTexture); bit 1 also gates its flash strip. GetRect(&left, &top, &right, &bottom) and
// GetTexCoords(&u0, &v0, &u1, &v1) are thiscall RET 0x10; SetTexCoords(u0, v0,
// u1, v1, bool) is thiscall RET 0x14, and the bool (which rotates the
// coordinates) is always false in stock code. SetRect(left, top, right, bottom)
// is the bitmap's vtable +0x4C, thiscall RET 0x10, the call SetValue makes on
// it.
//
// Same on every build, read off PostReadSetup, SetValue and ReadData:
//   +0xB0   RedBitmapElement* the bar draws with
//   +0x47C  mBarWidth, stored as right - left
//   +0x480  mBarU1, stored as u1 (the span only while u0 is 0)
//   +0x474  FlashyIncFadeOutTime    +0x478  FlashyDecFadeOutTime
//   +0x484  flags: bit 0 ScaleTexture, bit 1 edge moves (both on by default)
//   +0x220  the ElementBar base, whose mValue is at +0x1C
//
// Storing the bar end for end draws its quad right to left. The interface
// shader draws with culling off: pcInterfaceShader::Begin passes
// PCREDCULL_NONE (Phantom 0x008F29D4, modtools 0x00870284), and the stock
// flash already draws a right-to-left quad whenever a bar grows.
//
// The modtools HUD editor reads, changes and saves a bar through three of its
// vtable's entries, all thiscall with two arguments, RET 8: GetProperty (+0x18,
// modtools 0x00695490) and SetProperty (+0x14, 0x006952F0), (hash, value
// pointer) -> bool, which hand what they do not know (BitmapRect, TexCoords and
// the alignments among them) on to the bitmap's, and WriteData (+0x28,
// 0x00695B10), (PblFile*, int indent). All three work on the bar's live state:
// ScaleSize and ScaleTexture in the flags, the fade times, the rectangle (the
// writer widens it to mBarWidth for BitmapRect) and TexCoords (U1 from
// mBarU1). FillFrom changed all of those, so for a FillFrom bar each is handed
// the bar as the stock setup left it, kept from its PostReadSetup: the getter
// then answers as the file has it, the writer writes it and adds
// FillFrom("..."), and a change is kept as the file's new state and FillFrom
// laid out again from it, filled to the bar's value. The editor is off on
// Steam and GOG, so this is modtools only.
// =============================================================================

namespace {
using namespace hud_bar_fill_from;

constexpr uint32_t kFillFrom      = pbl_hash("FillFrom");
constexpr uint32_t kBitmap        = 0xB0;
constexpr uint32_t kBarWidth      = 0x47C;
constexpr uint32_t kBarU1         = 0x480;
constexpr uint32_t kIncFade       = 0x474;
constexpr uint32_t kDecFade       = 0x478;
constexpr uint32_t kFlags         = 0x484;
constexpr uint8_t  kFlagScaleTexture = 0x01;
constexpr uint8_t  kFlagScaleRect    = 0x02;  // SetValue moves the edge (and flashes) only with this
constexpr uint32_t kBarBase       = 0x220;    // the ElementBar base: SetValue's `this`
constexpr uint32_t kValue         = 0x1C;     // mValue, in the ElementBar base
constexpr uint32_t kVt_SetRect    = 0x4C;
constexpr int      kMaxPending    = 64;
constexpr int      kMaxVertical   = 64;
constexpr int      kMaxAuthored   = 128;

enum Mode { kRight, kBottom, kTop };

// PblConfig::Data: the property hash, the argument count, then 128 DWORD
// arguments. A string argument is an offset from the start of the arguments.
struct ConfigData {
   uint32_t id;
   uint32_t count;
   uint32_t args[128];
};

using ReadDataFn = bool(__fastcall*)(void* self, void* edx, void* config, const ConfigData* data);
using PostReadFn = void(__fastcall*)(void* self, void* edx);
using SetValueFn = float(__fastcall*)(void* self, void* edx, float value);
using GetFn      = void(__fastcall*)(void* self, void* edx, float* a, float* b, float* c, float* d);
using SetUVFn    = void(__fastcall*)(void* self, void* edx, float u0, float v0, float u1, float v1,
                                     bool rotate);
using SetRectFn  = void(__fastcall*)(void* self, void* edx, float l, float t, float r, float b);
using WriteDataFn = void(__fastcall*)(void* self, void* edx, void* file, int indent);
using IndentFn   = void(__fastcall*)(void* file, void* edx, int count);
using FormatFn   = void(__cdecl*)(void* file, const char* format, ...);
using SetPropertyFn = bool(__fastcall*)(void* self, void* edx, uint32_t property, const void* value);
using GetPropertyFn = bool(__fastcall*)(void* self, void* edx, uint32_t property, void* value);

ReadDataFn s_readData   = nullptr;
PostReadFn s_postRead   = nullptr;
SetValueFn s_setValue   = nullptr;
GetFn      s_getRect    = nullptr;
GetFn      s_getUV      = nullptr;
SetUVFn    s_setUV      = nullptr;
WriteDataFn s_writeData = nullptr;
IndentFn   s_indent     = nullptr;
FormatFn   s_format     = nullptr;
SetPropertyFn s_setProperty = nullptr;
GetPropertyFn s_getProperty = nullptr;
const void* s_held = nullptr;   // a bar the HUD editor is changing: its SetValue leaves it alone

// Bars whose FillFrom has been read but whose setup has not run yet. Each
// entry is taken back out by PostReadSetup, so the table only ever holds the
// bars of the .hud being read.
struct Pending {
   void* bar;
   Mode  mode;
};
Pending s_pending[kMaxPending];
int     s_pendingCount = 0;
bool    s_warnedFull = false;
bool    s_warnedValue = false;

// Vertical bars and their full layout. Cleared when the HUD opens; an entry
// left by a freed bar is inert, since SetValue only lays out a bar whose two
// fill flags setup cleared, and a stock bar keeps at least the edge flag.
struct VerticalBar {
   void*    bar;
   Vertical layout;
};
VerticalBar s_vertical[kMaxVertical];
int         s_verticalCount = 0;
bool        s_warnedVerticalFull = false;

// Each FillFrom bar as the stock setup left it, before FillFrom changed it:
// what the HUD editor's writer is handed. Cleared when the HUD opens.
struct Stored {
   Rect      rect;
   TexCoords uv;
   float     width;
   float     spanU;
   float     incFade;
   float     decFade;
   uint8_t   flags;
};
struct Authored {
   void*  bar;
   Mode   mode;
   Stored state;
};
Authored s_authored[kMaxAuthored];
int      s_authoredCount = 0;
bool     s_warnedAuthoredFull = false;

int find_pending(void* bar)
{
   for (int i = 0; i < s_pendingCount; ++i)
      if (s_pending[i].bar == bar) return i;
   return -1;
}

void remember(void* bar, Mode mode)
{
   const int i = find_pending(bar);
   if (i >= 0) { s_pending[i].mode = mode; return; }
   if (s_pendingCount == kMaxPending) {
      if (!s_warnedFull) install_log("[BarFillFrom] over %d bars pending; the rest fill from the left",
                                     kMaxPending);
      s_warnedFull = true;
      return;
   }
   s_pending[s_pendingCount++] = { bar, mode };
}

bool take(void* bar, Mode& mode)
{
   const int i = find_pending(bar);
   if (i < 0) return false;
   mode = s_pending[i].mode;
   s_pending[i] = s_pending[--s_pendingCount];
   return true;
}

void forget(void* bar)
{
   Mode unused;
   take(bar, unused);
}

const VerticalBar* find_vertical(const void* bar)
{
   for (int i = 0; i < s_verticalCount; ++i)
      if (s_vertical[i].bar == bar) return &s_vertical[i];
   return nullptr;
}

bool register_vertical(void* bar, const Vertical& layout)
{
   for (int i = 0; i < s_verticalCount; ++i)
      if (s_vertical[i].bar == bar) { s_vertical[i].layout = layout; return true; }
   if (s_verticalCount == kMaxVertical) {
      if (!s_warnedVerticalFull) install_log("[BarFillFrom] over %d vertical bars; the rest fill "
                                             "from the left", kMaxVertical);
      s_warnedVerticalFull = true;
      return false;
   }
   s_vertical[s_verticalCount++] = { bar, layout };
   return true;
}

void set_rect(void* bitmap, const Rect& r)
{
   const auto setRect = reinterpret_cast<SetRectFn>((*reinterpret_cast<void***>(bitmap))[kVt_SetRect / 4]);
   setRect(bitmap, nullptr, r.left, r.top, r.right, r.bottom);
}

// A bar's state as the editor's writer reads it.
Stored read_state(uint8_t* bar, void* bitmap)
{
   Stored s;
   s_getRect(bitmap, nullptr, &s.rect.left, &s.rect.top, &s.rect.right, &s.rect.bottom);
   s_getUV(bitmap, nullptr, &s.uv.u0, &s.uv.v0, &s.uv.u1, &s.uv.v1);
   s.width   = *reinterpret_cast<float*>(bar + kBarWidth);
   s.spanU   = *reinterpret_cast<float*>(bar + kBarU1);
   s.incFade = *reinterpret_cast<float*>(bar + kIncFade);
   s.decFade = *reinterpret_cast<float*>(bar + kDecFade);
   s.flags   = bar[kFlags];
   return s;
}

void write_state(uint8_t* bar, void* bitmap, const Stored& s)
{
   set_rect(bitmap, s.rect);
   s_setUV(bitmap, nullptr, s.uv.u0, s.uv.v0, s.uv.u1, s.uv.v1, false);
   *reinterpret_cast<float*>(bar + kBarWidth) = s.width;
   *reinterpret_cast<float*>(bar + kBarU1)    = s.spanU;
   *reinterpret_cast<float*>(bar + kIncFade)  = s.incFade;
   *reinterpret_cast<float*>(bar + kDecFade)  = s.decFade;
   bar[kFlags] = s.flags;
}

void remember_authored(void* bar, Mode mode, const Stored& state)
{
   for (int i = 0; i < s_authoredCount; ++i)
      if (s_authored[i].bar == bar) { s_authored[i] = { bar, mode, state }; return; }
   if (s_authoredCount == kMaxAuthored) {
      if (!s_warnedAuthoredFull)
         install_log("[BarFillFrom] over %d FillFrom bars; the HUD editor saves the rest as they draw",
                     kMaxAuthored);
      s_warnedAuthoredFull = true;
      return;
   }
   s_authored[s_authoredCount++] = { bar, mode, state };
}

Authored* find_authored(const void* bar)
{
   for (int i = 0; i < s_authoredCount; ++i)
      if (s_authored[i].bar == bar) return &s_authored[i];
   return nullptr;
}

const char* mode_name(Mode mode)
{
   return mode == kRight ? "Right" : mode == kBottom ? "Bottom" : "Top";
}

void lay_out_vertical(uint8_t* bar, const Vertical& layout, float value)
{
   void* bitmap = *reinterpret_cast<void**>(bar + kBitmap);
   if (!bitmap) return;
   const Bar b = fill_vertical(layout, value);
   set_rect(bitmap, b.rect);
   s_setUV(bitmap, nullptr, b.uv.u0, b.uv.v0, b.uv.u1, b.uv.v1, false);
}

const char* string_arg(const ConfigData* d, uint32_t i)
{
   if (d->count > 128 || i >= d->count) return nullptr;
   const uint32_t offset = d->args[i];
   if (offset < d->count * 4 || offset >= sizeof(d->args)) return nullptr;
   const char* base = reinterpret_cast<const char*>(d->args);
   for (uint32_t n = offset; n < sizeof(d->args); ++n)
      if (base[n] == '\0') return base + offset;
   return nullptr;  // unterminated
}

bool __fastcall hooked_ReadData(void* self, void* edx, void* config, const ConfigData* data)
{
   bool handled = false;
   __try {
      if (data && data->id == kFillFrom) {
         handled = true;
         const char* value = string_arg(data, 0);
         if (value && _stricmp(value, "Right") == 0)       remember(self, kRight);
         else if (value && _stricmp(value, "Bottom") == 0) remember(self, kBottom);
         else if (value && _stricmp(value, "Top") == 0)    remember(self, kTop);
         else {
            forget(self);
            if ((!value || _stricmp(value, "Left") != 0) && !s_warnedValue) {
               install_log("[BarFillFrom] FillFrom takes \"Left\", \"Right\", \"Bottom\" or \"Top\"; "
                           "that bar fills from the left");
               s_warnedValue = true;
            }
         }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      handled = false;
   }
   return handled || s_readData(self, edx, config, data);
}

// Runs after the stock setup, which converted BitmapRect to pixels and stored
// the width and U span from the authored rectangle and coordinates.
// FillFrom laid out on a bar from the state the stock setup gives it, as the
// bar's PostReadSetup does; with `atValue`, also filled to the value it holds,
// as SetValue would fill it (which does nothing for an unchanged value).
void apply_fill_from(uint8_t* bar, void* bitmap, Mode mode, const Stored& authored, bool atValue)
{
   const float value = *reinterpret_cast<float*>(bar + kBarBase + kValue);
   if (mode == kRight) {
      const Bar full = anchor_right(authored.rect, authored.uv);
      const Bar b = atValue ? fill(full, value, (authored.flags & kFlagScaleTexture) != 0,
                                   (authored.flags & kFlagScaleRect) != 0)
                            : full;
      set_rect(bitmap, b.rect);
      s_setUV(bitmap, nullptr, b.uv.u0, b.uv.v0, b.uv.u1, b.uv.v1, false);
      *reinterpret_cast<float*>(bar + kBarWidth) = full.width;
      *reinterpret_cast<float*>(bar + kBarU1)    = full.spanU;
      // The fill judges growth by which way the moving edge went, which is now
      // reversed: trade the two fade times so each still goes with its change.
      *reinterpret_cast<float*>(bar + kIncFade) = authored.decFade;
      *reinterpret_cast<float*>(bar + kDecFade) = authored.incFade;
      bar[kFlags] = authored.flags;
      return;
   }
   // Vertical: take the stock fill (and its flash) off this bar; the
   // SetValue hook lays it out from the full rectangle instead.
   const Vertical layout = { authored.rect, authored.uv, mode == kTop, (authored.flags & kFlagScaleTexture) != 0 };
   if (!register_vertical(bar, layout)) return;
   bar[kFlags] = static_cast<uint8_t>(authored.flags & ~(kFlagScaleTexture | kFlagScaleRect));
   if (atValue) lay_out_vertical(bar, layout, value);
}

void __fastcall hooked_PostRead(void* self, void* edx)
{
   s_postRead(self, edx);
   Mode mode;
   if (!take(self, mode)) return;
   __try {
      uint8_t* bar = static_cast<uint8_t*>(self);
      void* bitmap = *reinterpret_cast<void**>(bar + kBitmap);
      if (!bitmap) return;
      // The bar as the stock setup left it, kept for the HUD editor (modtools)
      // before FillFrom changes it.
      const Stored authored = read_state(bar, bitmap);
      if (s_writeData) remember_authored(bar, mode, authored);
      apply_fill_from(bar, bitmap, mode, authored, false);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A bar that cannot be read stays as the stock setup left it.
   }
}

// ElementBarBitmap::SetValue, reached with its ElementBar base (bar + 0x220).
// The stock fill runs first and does nothing visible on a vertical bar, then
// the bar is laid out for the value it stored. A bar the HUD editor is
// changing is left as it is: the change is made to the bar as the file had it.
float __fastcall hooked_SetValue(void* self, void* edx, float value)
{
   if (s_held && static_cast<uint8_t*>(self) - kBarBase == s_held)
      return *reinterpret_cast<float*>(static_cast<uint8_t*>(self) + kValue);
   const float stored = s_setValue(self, edx, value);
   if (s_verticalCount) {
      __try {
         uint8_t* bar = static_cast<uint8_t*>(self) - kBarBase;
         const VerticalBar* vertical = find_vertical(bar);
         if (vertical && !(bar[kFlags] & (kFlagScaleTexture | kFlagScaleRect)))
            lay_out_vertical(bar, vertical->layout, stored);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         // A bar that cannot be read keeps the stock result.
      }
   }
   return stored;
}

// The HUD editor saving a bar (modtools). A FillFrom bar is handed to the
// stock writer as the stock setup left it, gets its FillFrom line, and is put
// back as FillFrom draws it.
void __fastcall hooked_WriteData(void* self, void* edx, void* file, int indent)
{
   const Authored* authored = find_authored(self);
   uint8_t* bar = static_cast<uint8_t*>(self);
   void* bitmap = authored ? *reinterpret_cast<void**>(bar + kBitmap) : nullptr;
   if (!bitmap) {
      s_writeData(self, edx, file, indent);
      return;
   }
   const Stored live = read_state(bar, bitmap);
   write_state(bar, bitmap, authored->state);
   s_writeData(self, edx, file, indent);
   s_indent(file, nullptr, indent);
   s_format(file, "FillFrom(\"%s\")\n", mode_name(authored->mode));
   write_state(bar, bitmap, live);
}

// The HUD editor changing a property of a bar (modtools): the bar's
// SetProperty, which hands BitmapRect, TexCoords and the like on to the
// bitmap's. On a FillFrom bar the change is made to the bar as the file had
// it, while the bar's own SetValue is held off it, which keeps the result as
// the bar the file now has; FillFrom is then laid out from that again, filled
// to the bar's value.
bool __fastcall hooked_SetProperty(void* self, void* edx, uint32_t property, const void* value)
{
   Authored* authored = find_authored(self);
   uint8_t* bar = static_cast<uint8_t*>(self);
   void* bitmap = authored ? *reinterpret_cast<void**>(bar + kBitmap) : nullptr;
   if (!bitmap) return s_setProperty(self, edx, property, value);
   write_state(bar, bitmap, authored->state);
   s_held = bar;
   const bool set = s_setProperty(self, edx, property, value);
   s_held = nullptr;
   authored->state = read_state(bar, bitmap);
   apply_fill_from(bar, bitmap, authored->mode, authored->state, true);
   return set;
}

// The HUD editor reading a property of a bar for its panel (modtools): a
// FillFrom bar answers as the file has it, not as FillFrom draws it.
bool __fastcall hooked_GetProperty(void* self, void* edx, uint32_t property, void* value)
{
   const Authored* authored = find_authored(self);
   uint8_t* bar = static_cast<uint8_t*>(self);
   void* bitmap = authored ? *reinterpret_cast<void**>(bar + kBitmap) : nullptr;
   if (!bitmap) return s_getProperty(self, edx, property, value);
   const Stored live = read_state(bar, bitmap);
   write_state(bar, bitmap, authored->state);
   const bool got = s_getProperty(self, edx, property, value);
   write_state(bar, bitmap, live);
   return got;
}

bool guard(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[BarFillFrom] NOT installed: prologue mismatch at %s 0x%08X", what, (unsigned)va);
         return false;
      }
   }
   return true;
}

bool matches(uintptr_t base, uintptr_t va, const char* bytes, size_t length)
{
   return va && memcmp(resolve(base, va), bytes, length) == 0;
}

// The HUD editor's side of a bar, modtools only: GameExt keeps the editor off
// on Steam and GOG. Its own transaction, since FillFrom works without it; all
// three or none, since each relies on the bar state the others keep.
void install_editor_hooks(uintptr_t base)
{
   const auto& a = *g_addr;
   if (!matches(base, a.hud_bar_bitmap_write_data, "\x83\xEC\x10\x53\x55\x56\x57\x8B\xF1\x8B\xBE\xB0\x00\x00\x00", 15) ||
       !matches(base, a.hud_bar_bitmap_set_property, "\x53\x8B\x5C\x24\x0C\x56\x57\x8B\x7C\x24\x10\x81\xFF\x40\xF3\xB5\x79", 17) ||
       !matches(base, a.hud_bar_bitmap_get_property, "\x56\x8B\x74\x24\x08\x81\xFE\x40\xF3\xB5\x79\x57\x8B\xF9", 14) ||
       !matches(base, a.hud_write_indent, "\x56\x8B\x74\x24\x08\x85\xF6\x57\x8B\xF9", 10) ||
       !matches(base, a.hud_write_format, "\x8B\x4C\x24\x08\x81\xEC\x00\x04\x00\x00", 10)) {
      install_log("[BarFillFrom] the HUD editor's bar code is not where expected; it will not save FillFrom");
      return;
   }
   s_indent      = reinterpret_cast<IndentFn>(resolve(base, a.hud_write_indent));
   s_format      = reinterpret_cast<FormatFn>(resolve(base, a.hud_write_format));
   s_writeData   = reinterpret_cast<WriteDataFn>(resolve(base, a.hud_bar_bitmap_write_data));
   s_setProperty = reinterpret_cast<SetPropertyFn>(resolve(base, a.hud_bar_bitmap_set_property));
   s_getProperty = reinterpret_cast<GetPropertyFn>(resolve(base, a.hud_bar_bitmap_get_property));
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)s_writeData, hooked_WriteData);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_setProperty, hooked_SetProperty);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_getProperty, hooked_GetProperty);
   if (r != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
      DetourTransactionAbort();
      s_writeData = nullptr;   // keeps PostReadSetup from keeping bar state for nothing
      install_log("[BarFillFrom] the HUD editor will not save FillFrom: its detours failed");
      return;
   }
   install_log("[BarFillFrom] the HUD editor edits and saves FillFrom bars (ElementBarBitmap::WriteData "
               "0x%08X, SetProperty 0x%08X, GetProperty 0x%08X)", (unsigned)a.hud_bar_bitmap_write_data,
               (unsigned)a.hud_bar_bitmap_set_property, (unsigned)a.hud_bar_bitmap_get_property);
}

} // namespace

void hud_bar_fill_from_install(uintptr_t base)
{
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   if (!g_addr->hud_bar_bitmap_read_data || !g_addr->hud_bar_bitmap_post_read ||
       !g_addr->hud_bar_bitmap_set_value ||
       !g_addr->red_bitmap_get_rect || !g_addr->red_bitmap_get_tex_coords ||
       !g_addr->red_bitmap_set_tex_coords) {
      install_log("[BarFillFrom] NOT installed: no address set for this build");
      return;
   }
   // Every function hooked or called is guarded: a wrong address behind a
   // function pointer is a crash on first use, not a decline.
   if (!guard(base, g_addr->hud_bar_bitmap_read_data, "ElementBarBitmap::ReadData",
              modtools ? "\x53\x56\x8B\x74\x24\x10\x8B\x06\x3D\x40\xF3\xB5"
                       : "\x55\x8B\xEC\x56\x57\x8B\x7D\x0C\x8B\xF1\x8B\x07\x3D\x40\xF3\xB5",
              modtools ? "xxxxxxxxxxxx" : "xxxxxxxxxxxxxxxx") ||
       !guard(base, g_addr->hud_bar_bitmap_post_read, "ElementBarBitmap::PostReadSetup",
              modtools ? "\x83\xEC\x10\x56\x57\x8B\xF1\xE8\x00\x00\x00\x00\x8D\x8E\x20\x02\x00\x00"
                       : "\x55\x8B\xEC\x83\xEC\x10\x56\x57\x8B\xF9\xE8\x00\x00\x00\x00\x8D\x8F\x20\x02\x00\x00",
              modtools ? "xxxxxxxx????xxxxxx" : "xxxxxxxxxxx????xxxxxx") ||
       !guard(base, g_addr->hud_bar_bitmap_set_value, "ElementBarBitmap::SetValue",
              modtools ? "\x83\xEC\x30\x53\x56\x8B\xF1\x8B\x4C\x24\x3C\x8B\x46\x1C"
                       : "\x55\x8B\xEC\x83\xE4\xF8\x83\xEC\x38\x56\x57\x8B\xF9\x51\xF3\x0F\x10\x47\x1C",
              modtools ? "xxxxxxxxxxxxxx" : "xxxxxxxxxxxxxxxxxxx") ||
       !guard(base, g_addr->red_bitmap_get_rect, "RedBitmapElement::GetRect",
              modtools ? "\x8B\x41\x78\x8B\x54\x24\x04\x89\x02\x8B\x81\x80\x00\x00\x00"
                       : "\x55\x8B\xEC\x8B\x51\x78\x8B\x45\x08\x89\x10\x8B\x45\x0C\x8B\x91\x80\x00\x00\x00",
              modtools ? "xxxxxxxxxxxxxxx" : "xxxxxxxxxxxxxxxxxxxx") ||
       !guard(base, g_addr->red_bitmap_get_tex_coords, "RedBitmapElement::GetTexCoords",
              modtools ? "\x8B\x81\x94\x00\x00\x00\x8B\x54\x24\x04\x89\x02"
                       : "\x55\x8B\xEC\x8B\x91\x94\x00\x00\x00\x8B\x45\x08\x89\x10",
              modtools ? "xxxxxxxxxxxx" : "xxxxxxxxxxxxxx") ||
       !guard(base, g_addr->red_bitmap_set_tex_coords, "RedBitmapElement::SetTexCoords",
              modtools ? "\x8A\x44\x24\x14\x84\xC0\x74\x06\xD9\x44\x24\x0C"
                       : "\x55\x8B\xEC\x8A\x45\x18\xF3\x0F\x10\x45\x08\xF3\x0F\x10\x55\x10",
              modtools ? "xxxxxxxxxxxx" : "xxxxxxxxxxxxxxxx"))
      return;

   s_readData = reinterpret_cast<ReadDataFn>(resolve(base, g_addr->hud_bar_bitmap_read_data));
   s_postRead = reinterpret_cast<PostReadFn>(resolve(base, g_addr->hud_bar_bitmap_post_read));
   s_setValue = reinterpret_cast<SetValueFn>(resolve(base, g_addr->hud_bar_bitmap_set_value));
   s_getRect  = reinterpret_cast<GetFn>(resolve(base, g_addr->red_bitmap_get_rect));
   s_getUV    = reinterpret_cast<GetFn>(resolve(base, g_addr->red_bitmap_get_tex_coords));
   s_setUV    = reinterpret_cast<SetUVFn>(resolve(base, g_addr->red_bitmap_set_tex_coords));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)s_readData, hooked_ReadData);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_postRead, hooked_PostRead);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_setValue, hooked_SetValue);
   if (r != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[BarFillFrom] NOT installed: DetourAttach failed (%ld)", (long)r);
      return;
   }
   const bool ok = DetourTransactionCommit() == NO_ERROR;
   install_log("[BarFillFrom] %s (ReadData 0x%08X, PostReadSetup 0x%08X, SetValue 0x%08X); inert "
               "unless a .hud uses FillFrom", ok ? "installed" : "commit failed",
               (unsigned)g_addr->hud_bar_bitmap_read_data, (unsigned)g_addr->hud_bar_bitmap_post_read,
               (unsigned)g_addr->hud_bar_bitmap_set_value);
   if (ok && modtools) install_editor_hooks(base);
}

void hud_bar_fill_from_open()
{
   // A new HUD: every bar from the last one is gone.
   s_verticalCount = 0;
   s_pendingCount = 0;
   s_authoredCount = 0;
}
