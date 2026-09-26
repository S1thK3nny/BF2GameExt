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
// Read on each build, names from the Phantom PDB. Hooked: ReadData and
// PostReadSetup. The stock fill, ElementBarBitmap::SetValue (modtools
// 0x00696090, Steam 0x0054B070), is left alone.
//
//                                     modtools    Steam       GOG
//   ElementBarBitmap::ReadData        0x00695900  0x0054B480  0x0054C1D0
//   ElementBarBitmap::PostReadSetup   0x00696340  0x0054B320  0x0054C070
//   RedBitmapElement::GetRect         0x00838E50  0x006E4DB0  0x006E5E50
//   RedBitmapElement::GetTexCoords    0x008392A0  0x006E48F0  0x006E5990
//   RedBitmapElement::SetTexCoords    0x00839220  0x006E4B90  0x006E5C30
//
// ReadData is thiscall(PblConfig*, Data*) -> bool, RET 8; PostReadSetup is
// thiscall(), RET 0. GetRect(&left, &top, &right, &bottom) and
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
//
// Storing the bar end for end draws its quad right to left. The interface
// shader draws with culling off: pcInterfaceShader::Begin passes
// PCREDCULL_NONE (Phantom 0x008F29D4, modtools 0x00870284), and the stock
// flash already draws a right-to-left quad whenever a bar grows.
// =============================================================================

namespace {
using namespace hud_bar_fill_from;

constexpr uint32_t kFillFrom      = pbl_hash("FillFrom");
constexpr uint32_t kBitmap        = 0xB0;
constexpr uint32_t kBarWidth      = 0x47C;
constexpr uint32_t kBarU1         = 0x480;
constexpr uint32_t kIncFade       = 0x474;
constexpr uint32_t kDecFade       = 0x478;
constexpr uint32_t kVt_SetRect    = 0x4C;
constexpr int      kMaxPending    = 64;

// PblConfig::Data: the property hash, the argument count, then 128 DWORD
// arguments. A string argument is an offset from the start of the arguments.
struct ConfigData {
   uint32_t id;
   uint32_t count;
   uint32_t args[128];
};

using ReadDataFn = bool(__fastcall*)(void* self, void* edx, void* config, const ConfigData* data);
using PostReadFn = void(__fastcall*)(void* self, void* edx);
using GetFn      = void(__fastcall*)(void* self, void* edx, float* a, float* b, float* c, float* d);
using SetUVFn    = void(__fastcall*)(void* self, void* edx, float u0, float v0, float u1, float v1,
                                     bool rotate);
using SetRectFn  = void(__fastcall*)(void* self, void* edx, float l, float t, float r, float b);

ReadDataFn s_readData   = nullptr;
PostReadFn s_postRead   = nullptr;
GetFn      s_getRect    = nullptr;
GetFn      s_getUV      = nullptr;
SetUVFn    s_setUV      = nullptr;

// Bars whose FillFrom("Right") has been read but whose setup has not run yet.
// Each entry is taken back out by PostReadSetup, so the table only ever holds
// the bars of the .hud being read.
void* s_pending[kMaxPending];
int   s_pendingCount = 0;
bool  s_warnedFull = false;
bool  s_warnedValue = false;

int find_pending(void* bar)
{
   for (int i = 0; i < s_pendingCount; ++i)
      if (s_pending[i] == bar) return i;
   return -1;
}

void remember(void* bar)
{
   if (find_pending(bar) >= 0) return;
   if (s_pendingCount == kMaxPending) {
      if (!s_warnedFull) install_log("[BarFillFrom] over %d bars pending; the rest fill from the left",
                                     kMaxPending);
      s_warnedFull = true;
      return;
   }
   s_pending[s_pendingCount++] = bar;
}

bool take(void* bar)
{
   const int i = find_pending(bar);
   if (i < 0) return false;
   s_pending[i] = s_pending[--s_pendingCount];
   return true;
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
         if (value && _stricmp(value, "Right") == 0) {
            remember(self);
         } else {
            take(self);
            if ((!value || _stricmp(value, "Left") != 0) && !s_warnedValue) {
               install_log("[BarFillFrom] FillFrom takes \"Left\" or \"Right\"; that bar fills from the left");
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
void __fastcall hooked_PostRead(void* self, void* edx)
{
   s_postRead(self, edx);
   if (!take(self)) return;
   __try {
      uint8_t* bar = static_cast<uint8_t*>(self);
      void* bitmap = *reinterpret_cast<void**>(bar + kBitmap);
      if (!bitmap) return;
      Rect r;
      TexCoords t;
      s_getRect(bitmap, nullptr, &r.left, &r.top, &r.right, &r.bottom);
      s_getUV(bitmap, nullptr, &t.u0, &t.v0, &t.u1, &t.v1);
      const Bar b = anchor_right(r, t);
      const auto setRect = reinterpret_cast<SetRectFn>((*reinterpret_cast<void***>(bitmap))[kVt_SetRect / 4]);
      setRect(bitmap, nullptr, b.rect.left, b.rect.top, b.rect.right, b.rect.bottom);
      s_setUV(bitmap, nullptr, b.uv.u0, b.uv.v0, b.uv.u1, b.uv.v1, false);
      *reinterpret_cast<float*>(bar + kBarWidth) = b.width;
      *reinterpret_cast<float*>(bar + kBarU1)    = b.spanU;
      // The fill judges growth by which way the moving edge went, which is now
      // reversed: trade the two fade times so each still goes with its change.
      std::swap(*reinterpret_cast<float*>(bar + kIncFade), *reinterpret_cast<float*>(bar + kDecFade));
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A bar that cannot be read stays as the stock setup left it.
   }
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

} // namespace

void hud_bar_fill_from_install(uintptr_t base)
{
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   if (!g_addr->hud_bar_bitmap_read_data || !g_addr->hud_bar_bitmap_post_read ||
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
   s_getRect  = reinterpret_cast<GetFn>(resolve(base, g_addr->red_bitmap_get_rect));
   s_getUV    = reinterpret_cast<GetFn>(resolve(base, g_addr->red_bitmap_get_tex_coords));
   s_setUV    = reinterpret_cast<SetUVFn>(resolve(base, g_addr->red_bitmap_set_tex_coords));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)s_readData, hooked_ReadData);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_postRead, hooked_PostRead);
   if (r != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[BarFillFrom] NOT installed: DetourAttach failed (%ld)", (long)r);
      return;
   }
   const bool ok = DetourTransactionCommit() == NO_ERROR;
   install_log("[BarFillFrom] %s (ReadData 0x%08X, PostReadSetup 0x%08X); inert unless a .hud "
               "uses FillFrom", ok ? "installed" : "commit failed",
               (unsigned)g_addr->hud_bar_bitmap_read_data, (unsigned)g_addr->hud_bar_bitmap_post_read);
}
