#include "pch.h"
#include "world_kill_bounds.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <math.h>
#include <string.h>

// See world_kill_bounds.hpp.
//
// The constants are shared .rdata floats, so each compare's [disp32] operand is
// repointed instead. modtools is x87 (FCOMP, one operand per axis), Steam/GOG
// load 2500 once into XMM4 for both x and z, so side_ref1 is modtools only.

static constexpr float kStockSide   = 2500.0f;
static constexpr float kStockBottom = -1000.0f;
static constexpr float kStockTop    = 1000.0f;
static constexpr float kStockFall   = 60.0f;

static constexpr float kStockFlyerFloor    = -5000.0f;
static constexpr float kStockUnitFloor     = -500.0f;   // vehicles, droids, mines, turrets, powerups
static constexpr float kStockOrdnanceFloor = -2000.0f;
static constexpr float kStockBoxHalf       = 5000.0f;   // bullet box, all three axes

static float g_side   = kStockSide;
static float g_bottom = kStockBottom;
static float g_top    = kStockTop;
static float g_fall   = -kStockFall;   // compared against velocity.y

static float g_flyer_floor    = kStockFlyerFloor;
static float g_unit_floor     = kStockUnitFloor;
static float g_ordnance_floor = kStockOrdnanceFloor;

enum { kFall, kBottom, kSide0, kSide1, kTop, kItem,
       kFlyer, kDroideka, kHover, kWalker, kMine, kOrdnance,
       kDroid, kDefGrid, kPortable0, kPortable1, kPowerup, kRefCount };
static constexpr int kFirstOptional = kFlyer;

static uint32_t* g_refs[kRefCount]    = {};
static uint32_t  g_origRef[kRefCount] = {};

// OrdnanceBullet::Update's function-local static PblBox. Written in place and its
// guard bit set, so the game never initialises it to +/-5000.
struct WorldBox { float min[3]; float max[3]; };
static WorldBox* g_box      = nullptr;
static uint32_t* g_boxGuard = nullptr;
static uint32_t  g_boxMask  = 0;

static bool operand_points_at(const uint8_t* op, float expected)
{
   // ModRM before the operand must be mod=00 rm=101 ([disp32]).
   if ((op[-1] & 0xC7) != 0x05) return false;
   __try {
      const float* p = *(float* const*)op;
      return memcmp(p, &expected, sizeof(float)) == 0;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

static bool box_holds(const WorldBox* b, float lo, float hi)
{
   for (int i = 0; i < 3; ++i)
      if (b->min[i] != lo || b->max[i] != hi) return false;
   return true;
}

// box_ref is a [disp32] holding &box. guard_ref is the guard load, followed by
// "test al, imm8" (Steam/GOG) or "mov eax, imm32" (modtools) giving the bit.
static void install_box(uintptr_t exe_base)
{
   if (!g_addr->world_kill_box_ref || !g_addr->world_kill_box_guard_ref) return;

   const uint8_t* boxOp   = (const uint8_t*)resolve(exe_base, g_addr->world_kill_box_ref);
   const uint8_t* guardOp = (const uint8_t*)resolve(exe_base, g_addr->world_kill_box_guard_ref);

   uint32_t mask = 0;
   if (guardOp[4] == 0xA8)      mask = guardOp[5];
   else if (guardOp[4] == 0xB8) mask = *(const uint32_t*)(guardOp + 5);

   const bool guardOk = (guardOp[-1] & 0xC7) == 0x05 || guardOp[-1] == 0xA1;
   if ((boxOp[-1] & 0xC7) != 0x05 || !guardOk || !mask || (mask & (mask - 1))) {
      install_log("[WorldKillBounds] unexpected bullet box code, ordnance box left stock");
      return;
   }

   WorldBox* box   = *(WorldBox* const*)boxOp;
   uint32_t* guard = *(uint32_t* const*)guardOp;

   // Untouched .bss, or already initialised to the stock box.
   bool ok = false;
   __try {
      ok = (*guard & mask) ? box_holds(box, -kStockBoxHalf, kStockBoxHalf)
                           : box_holds(box, 0.0f, 0.0f);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
   }
   if (!ok) {
      install_log("[WorldKillBounds] bullet box at %08X not as expected, ordnance box left stock",
                  (unsigned)(uintptr_t)box);
      return;
   }

   g_box      = box;
   g_boxGuard = guard;
   g_boxMask  = mask;
}

// Every other limit follows the soldier box but never gets tighter than stock.
static void apply_limits()
{
   g_flyer_floor    = fminf(kStockFlyerFloor, g_bottom);
   g_unit_floor     = fminf(kStockUnitFloor, g_bottom);
   g_ordnance_floor = fminf(kStockOrdnanceFloor, g_bottom);

   if (!g_box) return;
   const float side = fmaxf(kStockBoxHalf, g_side);
   g_box->min[0] = -side;
   g_box->min[1] = fminf(-kStockBoxHalf, g_bottom);
   g_box->min[2] = -side;
   g_box->max[0] = side;
   g_box->max[1] = fmaxf(kStockBoxHalf, g_top);
   g_box->max[2] = side;
   *g_boxGuard |= g_boxMask;
}

void world_kill_bounds_install(uintptr_t exe_base)
{
   const uintptr_t addrs[kRefCount] = {
      g_addr->world_kill_fall_ref,     g_addr->world_kill_bottom_ref, g_addr->world_kill_side_ref0,
      g_addr->world_kill_side_ref1,    g_addr->world_kill_top_ref,    g_addr->world_kill_item_ref,
      g_addr->world_kill_flyer_ref,    g_addr->world_kill_droideka_ref, g_addr->world_kill_hover_ref,
      g_addr->world_kill_walker_ref,   g_addr->world_kill_mine_ref,   g_addr->world_kill_ordnance_ref,
      g_addr->world_kill_droid_ref,    g_addr->world_kill_defgrid_ref, g_addr->world_kill_portable_ref0,
      g_addr->world_kill_portable_ref1, g_addr->world_kill_powerup_ref,
   };
   const float expected[kRefCount] = {
      -kStockFall, kStockBottom, kStockSide, kStockSide, kStockTop, kStockBottom,
      kStockFlyerFloor, kStockUnitFloor, kStockUnitFloor, kStockUnitFloor, kStockUnitFloor,
      kStockOrdnanceFloor,
      kStockUnitFloor, kStockUnitFloor, kStockUnitFloor, kStockUnitFloor, kStockUnitFloor,
   };
   float* const targets[kRefCount] = {
      &g_fall, &g_bottom, &g_side, &g_side, &g_top, &g_bottom,
      &g_flyer_floor, &g_unit_floor, &g_unit_floor, &g_unit_floor, &g_unit_floor,
      &g_ordnance_floor,
      &g_unit_floor, &g_unit_floor, &g_unit_floor, &g_unit_floor, &g_unit_floor,
   };

   // The soldier box (all but side_ref1) is required, the rest are optional.
   bool use[kRefCount] = {};
   for (int i = 0; i < kRefCount; ++i) {
      if (addrs[i] == 0) {
         if (i == kSide1 || i >= kFirstOptional) continue;
         return;
      }
      if (!operand_points_at((const uint8_t*)resolve(exe_base, addrs[i]), expected[i])) {
         install_log("[WorldKillBounds] unexpected operand at %08X, skipping", (unsigned)addrs[i]);
         if (i < kFirstOptional) return;
         continue;
      }
      use[i] = true;
   }

   // .text is RW during install.
   for (int i = 0; i < kRefCount; ++i) {
      if (!use[i]) continue;
      g_refs[i]    = (uint32_t*)resolve(exe_base, addrs[i]);
      g_origRef[i] = *g_refs[i];
      *g_refs[i]   = (uint32_t)(uintptr_t)targets[i];
   }

   install_box(exe_base);
   apply_limits();

   install_log("[WorldKillBounds] installed");
}

void world_kill_bounds_uninstall()
{
   for (int i = 0; i < kRefCount; ++i) {
      if (!g_refs[i]) continue;
      protected_write(g_refs[i], &g_origRef[i], sizeof(g_origRef[i]));
      g_refs[i] = nullptr;
   }

   // Leave the guard set, the game would have written the same stock box.
   if (g_box) {
      for (int i = 0; i < 3; ++i) {
         g_box->min[i] = -kStockBoxHalf;
         g_box->max[i] = kStockBoxHalf;
      }
      g_box = nullptr;
   }
}

void world_kill_bounds_reset()
{
   g_side   = kStockSide;
   g_bottom = kStockBottom;
   g_top    = kStockTop;
   g_fall   = -kStockFall;
   apply_limits();
}

bool world_kill_bounds_active()
{
   return g_refs[kFall] != nullptr;
}

void world_kill_bounds_set(float horizontal, float bottom, float top, float fall)
{
   g_side   = horizontal;
   g_bottom = bottom;
   g_top    = top;
   g_fall   = -fall;
   apply_limits();
}

void world_kill_bounds_get(float* horizontal, float* bottom, float* top, float* fall)
{
   *horizontal = g_side;
   *bottom     = g_bottom;
   *top        = g_top;
   *fall       = -g_fall;
}
