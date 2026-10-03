#include "pch.h"
#include "walker_foot_diag.hpp"
#include "walker_foot_diag_core.hpp"
#include "walker_stomp_fix.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "game/Battlefront2/Source/Character.h"
#include "game/Battlefront2/Source/EntityWalker.h"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <detours.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// =============================================================================
// Read on each build (names from the Phantom PDB):
//
//                                       modtools    Steam       GOG
//   EntityWalker::DoFootImpactEffects   0x00555B60  0x00500710  0x00500710   detoured
//     thiscall(), plain RET. Its one caller, EntityWalker::UpdateState, calls
//     it once per update of the walker: modtools at 0x0055BC98, through the
//     thunk at 0x0040C329, retail at 0x00503214.
//   NetGame::GetLocalPlayer             game_addrs net_game_get_local_player  called
//     cdecl(uint localIndex) -> Character*
//
// The walker's fields are layout::EntityWalker's. The player's walker is the
// Controllable in Character::mVehicle (layout::Character), whose Trackable
// part (+0x18) answers GetGameObject (+0x1C), as camera_shake.cpp finds a
// weapon's owner. In one of a walker's gun seats, the seat is there instead.
//
// One call is one update, so a foot's mLastFootHeight going in, less what it
// is coming out, is the drop DoFootImpactEffects tests, and the foot's bit in
// mFootState coming out says whether it counted a landing in that update.
// =============================================================================

bool g_walkerFootDiag = false;

namespace {

using namespace walker_foot_diag;
namespace walk = layout::EntityWalker;
namespace walk_class = layout::EntityWalkerClass;

using FootFxFn      = void(__fastcall*)(uint8_t* self, void* edx);
using LocalPlayerFn = uint8_t*(__cdecl*)(uint32_t localIndex);
using ObjectFn      = uint8_t*(__thiscall*)(void* self);

constexpr uint32_t kCtrl_Trackable   = 0x18;   // a Controllable's Trackable part
constexpr uint32_t kVt_GetGameObject = 0x1C;   // Trackable vtable

// Lines are kept and written out together at most this often, so writing the
// log does not stall the updates being measured.
constexpr double kFlushSeconds = 2.0;
// A drive logs a few lines a second; this many is plenty.
constexpr int kMaxLines = 5000;

FootFxFn        s_footFx      = nullptr;
LocalPlayerFn   s_localPlayer = nullptr;
uint32_t        s_nameOffset  = 0;   // a class's ODF name: only modtools keeps it

template<class T> T& at(uint8_t* p, uint32_t offset) { return *reinterpret_cast<T*>(p + offset); }

// -----------------------------------------------------------------------------
// The log, written in batches
// -----------------------------------------------------------------------------

char          s_buffer[16384];
size_t        s_used  = 0;
int           s_lines = 0;
LARGE_INTEGER s_freq  = {};
LARGE_INTEGER s_lastFlush = {};

void flush()
{
   if (!s_used) return;
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") == 0 && f) {
      fwrite(s_buffer, 1, s_used, f);
      fclose(f);
   }
   s_used = 0;
}

void add_line(const char* text)
{
   const size_t n = strlen(text);
   if (s_used + n + 1 > sizeof s_buffer) flush();
   memcpy(s_buffer + s_used, text, n);
   s_used += n;
   s_buffer[s_used++] = '\n';
}

void line(const char* fmt, ...)
{
   if (s_lines >= kMaxLines) return;
   char text[640];
   va_list ap;
   va_start(ap, fmt);
   _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, ap);
   va_end(ap);
   add_line(text);
   if (++s_lines == kMaxLines) {
      _snprintf_s(text, sizeof text, _TRUNCATE,
                  "[WalkerFootDiag] %d lines logged: nothing more this session", kMaxLines);
      add_line(text);
   }
}

void appendf(char* out, size_t cap, const char* fmt, ...)
{
   const size_t used = strlen(out);
   if (used + 1 >= cap) return;
   va_list ap;
   va_start(ap, fmt);
   _vsnprintf_s(out + used, cap - used, _TRUNCATE, fmt, ap);
   va_end(ap);
}

double seconds_between(const LARGE_INTEGER& from, const LARGE_INTEGER& to)
{
   return static_cast<double>(to.QuadPart - from.QuadPart) / static_cast<double>(s_freq.QuadPart);
}

// -----------------------------------------------------------------------------
// The walker the player drives, and its feet
// -----------------------------------------------------------------------------

struct Driven {
   uint8_t*      walker    = nullptr;   // only compared once it may be gone
   bool          timed     = false;     // `last` holds its previous update
   LARGE_INTEGER last      = {};
   int           feet      = 0;
   int           type      = 0;         // StompDetectionType
   float         threshold = 0.0f;      // StompThreshold
   char          name[64]  = {};
   FootTracker   foot[walk::kMaxFeet];
   int           steps[walk::kMaxFeet]   = {};
   int           counted[walk::kMaxFeet] = {};
};
Driven s_driven;

uint8_t* driven_walker()
{
   __try {
      uint8_t* chr = s_localPlayer(0);
      uint8_t* vehicle = chr ? at<uint8_t*>(chr, layout::Character::kVehicle) : nullptr;
      if (!vehicle) return nullptr;
      void* trackable = vehicle + kCtrl_Trackable;
      return reinterpret_cast<ObjectFn>((*static_cast<void***>(trackable))[kVt_GetGameObject / 4])(trackable);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return nullptr;
   }
}

// Feet are numbered in the order the ODF lists TerrainLeft and TerrainRight:
// left then right, a leg pair at a time, front pair first in every stock walker.
void foot_name(int i, char* out, size_t cap)
{
   static const char* const kTwoPairs[]   = { "front", "back" };
   static const char* const kThreePairs[] = { "front", "middle", "back" };
   const char* side = i % 2 ? "right" : "left";
   const int pairs = s_driven.feet / 2;
   if (s_driven.feet % 2 || pairs < 1 || pairs > 3)
      _snprintf_s(out, cap, _TRUNCATE, "%d", i);
   else if (pairs == 1)
      _snprintf_s(out, cap, _TRUNCATE, "%d %s", i, side);
   else
      _snprintf_s(out, cap, _TRUNCATE, "%d %s %s", i, (pairs == 2 ? kTwoPairs : kThreePairs)[i / 2], side);
}

// The walker just left: its steps counted, foot by foot.
void summary()
{
   if (!s_driven.walker || !s_driven.feet) return;
   char text[512] = {};
   _snprintf_s(text, sizeof text, _TRUNCATE, "[WalkerFootDiag] left %s; steps BF2 counted:",
               s_driven.name[0] ? s_driven.name : "the walker");
   for (int i = 0; i < s_driven.feet; ++i) {
      char name[32];
      foot_name(i, name, sizeof name);
      appendf(text, sizeof text, "%s foot %s %d of %d", i ? "," : "", name, s_driven.counted[i],
              s_driven.steps[i]);
   }
   line("%s", text);
}

void leave()
{
   summary();
   flush();
   s_driven = Driven{};
}

void enter(uint8_t* self)
{
   leave();
   s_driven.walker = self;
   __try {
      uint8_t* cls = reinterpret_cast<uint8_t*>(walk::mClass(self));
      if (!cls) return;
      const int feet = walk_class::mNumFeet(cls);
      s_driven.type = walk_class::mStompDetectionType(cls);
      s_driven.threshold = walk_class::mStompThreshold(cls);
      if (s_nameOffset) {
         const char* src = reinterpret_cast<const char*>(cls + s_nameOffset);
         for (size_t i = 0; i + 1 < sizeof s_driven.name && src[i]; ++i)
            s_driven.name[i] = src[i] >= 0x20 && src[i] <= 0x7E ? src[i] : '?';
      }
      s_driven.feet = feet < walk::kMaxFeet ? feet : walk::kMaxFeet;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_driven.feet = 0;
   }
   char rule[200];
   if (!s_driven.type)
      _snprintf_s(rule, sizeof rule, _TRUNCATE, "Type 0 counts a step the update the foot comes down past "
                  "its lowest height so far plus StompThreshold.");
   else if (g_walkerStompFix)
      _snprintf_s(rule, sizeof rule, _TRUNCATE, "Type 1, with WalkerStompFix: a foot coming down faster "
                  "than %.1f m/s is re-armed, and counts a step the first update it is slower.",
                  kWalkerStompSpeed);
   else
      _snprintf_s(rule, sizeof rule, _TRUNCATE, "Type 1 counts a step the first update the foot drops less "
                  "than 0.1 m, once a drop of more than 0.1 m in a single update has re-armed it.");
   line("[WalkerFootDiag] driving %s: %d feet, StompDetectionType %d, StompThreshold %.2f. %s Heights are "
        "the foot over the walker's origin, as BF2 measures them; feet go in the ODF's TerrainLeft and "
        "TerrainRight order.",
        s_driven.name[0] ? s_driven.name : "a walker (no class names on this build)", s_driven.feet,
        s_driven.type, s_driven.threshold, rule);
}

void log_step(int i)
{
   const Step& s = s_driven.foot[i].step();
   ++s_driven.steps[i];
   if (s.counted) ++s_driven.counted[i];
   char name[32];
   foot_name(i, name, sizeof name);
   const float each = s.updates ? s.seconds / static_cast<float>(s.updates) : 0.0f;
   const float speed = s.biggestSeconds > 0.0f ? s.biggest / s.biggestSeconds : 0.0f;

   char verdict[200];
   if (s.counted)
      _snprintf_s(verdict, sizeof verdict, _TRUNCATE, "yes");
   else if (s_driven.type && g_walkerStompFix)
      _snprintf_s(verdict, sizeof verdict, _TRUNCATE, "NO, it never came down faster than %.1f m/s, so it "
                  "was not re-armed", kWalkerStompSpeed);
   else if (s_driven.type)
      _snprintf_s(verdict, sizeof verdict, _TRUNCATE, "NO, type 1 needs more than %.3f m in one update "
                  "(%.1f m/s at %.1f ms an update)", walk::kStompDrop,
                  each > 0.0f ? walk::kStompDrop / each : 0.0f, 1000.0f * each);
   else if (s.top <= s.line)
      _snprintf_s(verdict, sizeof verdict, _TRUNCATE, "NO, it started below %.3f m (its lowest plus "
                  "StompThreshold), so it could not come down past it", s.line);
   else
      _snprintf_s(verdict, sizeof verdict, _TRUNCATE, "NO, it stopped at %.3f m, above %.3f m (its "
                  "lowest plus StompThreshold)", s.bottom, s.line);

   line("[WalkerFootDiag] foot %s: came down %.3f m (%.3f to %.3f) over %d updates, %.1f ms each; biggest "
        "drop in one update %.3f m (%.1f m/s); counted: %s. This foot: %d of %d steps counted.",
        name, s.drop(), s.top, s.bottom, s.updates, 1000.0f * each, s.biggest, speed, verdict,
        s_driven.counted[i], s_driven.steps[i]);
}

void log_stray(int i, const FootUpdate& u)
{
   char name[32];
   foot_name(i, name, sizeof name);
   line("[WalkerFootDiag] foot %s: counted while not coming down (it moved %+.3f m that update, at %.3f m)",
        name, u.after - u.before, u.after);
}

struct FeetBefore {
   float    height[walk::kMaxFeet];
   float    lowest[walk::kMaxFeet];
   uint32_t state;
};

bool read_feet(uint8_t* self, FeetBefore& out)
{
   __try {
      const float* height = walk::mLastFootHeight(self);
      const float* lowest = walk::mMinFootHeight(self);
      for (int i = 0; i < s_driven.feet; ++i) {
         out.height[i] = height[i];
         out.lowest[i] = lowest[i];
      }
      out.state = walk::mFootState(self);
      return true;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

void measure(uint8_t* self, const FeetBefore& before, float seconds)
{
   FeetBefore after;
   if (!read_feet(self, after)) return;
   for (int i = 0; i < s_driven.feet; ++i) {
      FootUpdate u;
      u.before  = before.height[i];
      u.after   = after.height[i];
      u.lowest  = before.lowest[i];
      u.seconds = seconds;
      u.counted = ((after.state >> i) & 1u) != 0;
      const uint32_t disarmed = 1u << (walk::kFootDownBit + i);
      u.rearmed = s_driven.type != 0 && (before.state & disarmed) && !(after.state & disarmed);
      switch (s_driven.foot[i].update(u, s_driven.threshold)) {
      case Event::kStep:       log_step(i);     break;
      case Event::kStrayCount: log_stray(i, u); break;
      case Event::kNone:                        break;
      }
   }
}

void __fastcall hooked_FootFx(uint8_t* self, void* edx)
{
   if (self != driven_walker()) {
      if (self == s_driven.walker) leave();
      s_footFx(self, edx);
      return;
   }
   LARGE_INTEGER now;
   QueryPerformanceCounter(&now);
   if (self != s_driven.walker) enter(self);

   FeetBefore before;
   const bool read = read_feet(self, before);
   s_footFx(self, edx);
   if (read && s_driven.timed) measure(self, before, static_cast<float>(seconds_between(s_driven.last, now)));
   s_driven.last = now;
   s_driven.timed = true;

   if (seconds_between(s_lastFlush, now) >= kFlushSeconds) {
      flush();
      s_lastFlush = now;
   }
}

bool guard(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[WalkerFootDiag] NOT installed: prologue mismatch at %s 0x%08X", what, (unsigned)va);
         return false;
      }
   }
   return true;
}

} // namespace

void walker_foot_diag_install(uintptr_t base)
{
   if (!g_walkerFootDiag) return;
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   if (!g_addr->walker_do_foot_impact_effects || !g_addr->net_game_get_local_player) {
      install_log("[WalkerFootDiag] NOT installed: no address set for this build");
      return;
   }
   // DoFootImpactEffects' prologue up to its read of mClass; GetLocalPlayer's
   // as target_bar_latch.cpp checks it.
   if (!guard(base, g_addr->walker_do_foot_impact_effects, "EntityWalker::DoFootImpactEffects",
              modtools ? "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\x94\x00\x00\x00\x53\x56\x8B\xF1\x8B\x86\x98\x04\x00\x00"
                       : "\x53\x8B\xDC\x83\xEC\x08\x83\xE4\xF0\x83\xC4\x04\x55\x8B\x6B\x04\x89\x6C\x24\x04"
                         "\x8B\xEC\x81\xEC\x88\x00\x00\x00\x56\x57\x8B\xF9\x8B\x87\x60\x04\x00\x00",
              modtools ? "xxxxxxxxxxxxxxxxxxxxxx" : "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx") ||
       !guard(base, g_addr->net_game_get_local_player, "NetGame::GetLocalPlayer",
              modtools ? "\x55\x8B\xEC\x83\xEC\x0C\x8B\x45\x08\x3B\x05"
                       : "\x55\x8B\xEC\xE8\x00\x00\x00\x00\x39\x45\x08\x72",
              modtools ? "xxxxxxxxxxx" : "xxxx????xxxx"))
      return;

   s_nameOffset  = static_cast<uint32_t>(g_addr->entity_class_name_off);
   s_localPlayer = reinterpret_cast<LocalPlayerFn>(resolve(base, g_addr->net_game_get_local_player));
   s_footFx      = reinterpret_cast<FootFxFn>(resolve(base, g_addr->walker_do_foot_impact_effects));
   QueryPerformanceFrequency(&s_freq);
   QueryPerformanceCounter(&s_lastFlush);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG r = DetourAttach(&(PVOID&)s_footFx, hooked_FootFx);
   if (r != NO_ERROR) {
      DetourTransactionAbort();
      s_footFx = nullptr;
      install_log("[WalkerFootDiag] NOT installed: DetourAttach failed (%ld)", (long)r);
      return;
   }
   if (DetourTransactionCommit() != NO_ERROR) {
      s_footFx = nullptr;
      install_log("[WalkerFootDiag] NOT installed: DetourTransactionCommit failed");
      return;
   }
   install_log("[WalkerFootDiag] installed (DoFootImpactEffects 0x%08X): logging each step of the walker "
               "the player drives", (unsigned)g_addr->walker_do_foot_impact_effects);
}

void walker_foot_diag_uninstall()
{
   if (!s_footFx) return;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)s_footFx, hooked_FootFx);
   DetourTransactionCommit();
   s_footFx = nullptr;
   leave();
}
