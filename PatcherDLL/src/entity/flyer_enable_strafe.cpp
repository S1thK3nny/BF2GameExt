#include "pch.h"
#include "flyer_enable_strafe.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "core/x86_emit.hpp"
#include "util/install_log.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <detours.h>

// =============================================================================
// EnableStrafe - per-class ODF property for EntityFlyer.
//
// What the engine does (EntityFlyer::Update, on the Controllable subobject):
//
//  1. Builds the FLIGHT FRAME  F = RotZ(-mBankRollAngle) * RotX(-mBankPitchAngle)
//     * World.  The object matrix (World) is what gets rendered and carries the
//     cosmetic bank; F is the frame the flyer actually flies in.  Auto-level
//     keeps F level (it drives F.right.y to 0 once the sticks are idle).
//  2. Ramps mSetStrafe toward  mSideRoll.x * TrickSideRollStrafeSpeed
//                              + StrafeSpeed * <strafe input>
//     where <strafe input> is a constant 0 in the shipped code (the shared
//     global 0.0f on modtools, a zeroed xmm7 on Steam/GOG).  Only the side-roll
//     trick ever moves a stock flyer sideways.
//  3. Composes velocity per state from  World.fwd*mSetSpeed - F.right*mSetStrafe
//     - F.up*mSetStrafeVertical.  TAKEOFF/LANDING interpolate it fresh each frame
//     by mFlightRatio.  FLYING blends it into the previous velocity by
//     dt*MomentumFilter and then rescales the WHOLE vector so its forward part
//     equals mSetSpeed:  if (0 < v.fwd != mSetSpeed) v *= mSetSpeed / v.fwd.
//  4. Integrates position from mVelocity and rotation from omega, both in F,
//     then re-applies the bank and calls SetMatrix.
//
// The rescale in step 3 was written for a flyer that only ever moves forward.
// A sustained strafe does not survive it: while the throttle climbs the factor
// is > 1 and the strafe is amplified every frame, and while hovering
// (mSetSpeed 0) any forward drift makes the factor 0 and wipes the whole
// velocity, strafe included, for that frame.
//
// Battlefront 1 has none of this.  Its FLYING case ramps mSetStrafe toward
// input * StrafeSpeed by Acceleration * dt and SETS velocity = X * strafe +
// Z * speed; TAKEOFF/LANDING interpolate the same term by the flight ratio.
//
// So for a flagged class the engine is kept blind to strafe (mSetStrafe held at
// 0 and the ramp input 0 while Update runs, so steps 2 and 3 are exactly stock),
// and the strafe is added the BF1 way at the head of step 4: a small cave there
// adds s_lat = -strafe * F.right (* mFlightRatio in TAKEOFF/LANDING) into
// mVelocity right before the engine integrates it.  In FLYING the next Update
// takes that vector back out first, so the engine's momentum never inherits it.
// The strafe itself ramps like BF1: toward input * StrafeSpeed by
// Acceleration * dt.  Nothing of the engine's own flight is changed.
//
// The strafe axis, Controllable::mControlStrafe (+0x84), is otherwise read once
// in Update as the ROLL input; a flagged class gets 0 there, so that axis no
// longer rolls.  The alternate control mode (+0x54 bit 0) takes roll from the
// turn axis through a different load, so it still rolls.  The lean sites
// (StrafeRollAngle * <strafe input>, in every state) get the real stick, so the
// flyer banks into the strafe; the bank is cosmetic and never tilts the strafe.
//
// Tricks keep the stock path: while mSideRoll is set, the engine gets the stick
// as its strafe input and does the side roll itself.  CRASHING and LANDED are
// stock.  It applies to every flyer of a flagged class, player and AI alike, so
// host and clients simulate the same thing in multiplayer.
//
// The same rescale also makes reverse lopsided.  Its guard is
// `d > S || (S > d && d > 0)` with d = v.fwd, S = mSetSpeed: slowing down,
// flipping through zero into reverse and speeding up in reverse all snap the
// forward speed to S, but braking OUT of reverse (d < 0, S > d) does not, so
// there only the momentum blend moves it, at Acceleration * MomentumFilter * dt
// per second instead of Acceleration (about 7x slower at 60 fps, and frame rate
// dependent: dt is the real frame time in single player).  BF1 sets velocity
// directly, so it has no such asymmetry.  For a flagged class in FLYING the same
// hook therefore also sets the forward component of mVelocity to mSetSpeed.
// Wherever the engine's rescale already ran that is a no-op; it only changes
// the reverse case, which then recovers at Acceleration like every other case.
//
// Patch shapes:
//   modtools  `FLD [ebx+0x84]` -> `FLD [&s_rollInput]`    (D9 83 / D9 05, 6 bytes)
//             `FMUL [zero]`    -> `FMUL [&s_rampInput]` at the ramp and
//                                 `FMUL [&s_leanInput]` at the lean sites
//   release   `MOVSS xmm2,[edi+0x84]` -> `MOVSS xmm2,[&s_rollInput]` (8 bytes)
//             `MULSS xmmA,xmm7 ; MULSS xmmB,xmmC` (8 bytes, no relative operands)
//             -> JMP cave: `MULSS xmmA,[&input] ; MULSS xmmB,xmmC ; JMP back`
//   both      `LEA EAX,[ESP+disp32]` (7 bytes) at the head of the integration
//             -> JMP cave: CALL integrate_hook, the displaced LEA, JMP back.
//             The site is followed on every build by LEA EAX / PUSH EAX /
//             LEA ECX / CALL, so EAX, ECX, EDX, the XMM registers and the
//             flags are dead there, and the x87 stack is empty: a plain cdecl
//             call is safe.  integrate_hook finds the flyer through s_in.
// =============================================================================

// ---------------------------------------------------------------------------
// Per-build layout (offsets from the Controllable subobject Update runs on)
// ---------------------------------------------------------------------------
// Every offset is read at that build's strafe ramp, flight-frame build or
// FLYING velocity code.  The world matrix rows (object +0xF0/+0x100/+0x110)
// are the same on every build.
struct FlyerLayout {
   int mClass;        // EntityFlyerClass*
   int mVelocity;     // PblVector3, world space
   int mSetSpeed;     // float, the throttle's target forward speed
   int mSetStrafe;    // float
   int mState;        // EntityFlyer::State
   int mFlightRatio;  // float, 0..1 through TAKEOFF / LANDING
   int mBankPitch;    // float, mBankPitchAngle
   int mBankRoll;     // float, mBankRollAngle
   int mSideRollX;    // float, mSideRoll.x (y follows)
   // EntityFlyerClass
   int clsAcceleration;
   int clsMaxSpeed;
   int clsBoostSpeed;
   int clsStrafeSpeed;
   int clsTrickSideRollStrafeSpeed;
   // Integration site, `LEA EAX,[ESP+disp32]`
   uint8_t leaOrig[7];
   // FLYING rescale site: the first instruction(s) of `v *= mSetSpeed / d`
   uint8_t rescaleLen;
   uint8_t rescaleOrig[8];
};
static constexpr FlyerLayout kLayoutModtools = {
   0x42C, 0x340, 0x358, 0x35C, 0x364, 0x368, 0x36C, 0x374, 0x398,
   0x884, 0x894, 0x898, 0x89C, 0xDA4,
   {0x8D, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00},
   6, {0xD9, 0x83, 0x58, 0x03, 0x00, 0x00} };              // FLD [ebx+0x358]
static constexpr FlyerLayout kLayoutRelease = {
   0x3EC, 0x300, 0x318, 0x31C, 0x324, 0x328, 0x32C, 0x334, 0x358,
   0x7BC, 0x7CC, 0x7D0, 0x7D4, 0xCDC,
   {0x8D, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00},
   8, {0xF3, 0x0F, 0x5E, 0xCA, 0xF3, 0x0F, 0x10, 0x01} };  // DIVSS xmm1,xmm2 ; MOVSS xmm0,[ecx]

static constexpr int kControlStrafe = 0x84;   // mControlStrafe, every build
static constexpr int kWorldRight    = -0x150; // object +0xF0
static constexpr int kWorldUp       = -0x140; // object +0x100
static constexpr int kWorldFwd      = -0x130; // object +0x110

enum : int { kStateTakeoff = 1, kStateFlying = 2, kStateLanding = 3 };

static FlyerLayout s_layout = kLayoutModtools;

// ---------------------------------------------------------------------------
// Values the patched sites read
// ---------------------------------------------------------------------------
// Only read from inside EntityFlyer::Update, which the detour below brackets
// (saving and restoring them, in case one flyer's Update ever runs another's).
struct StrafeInputs {
   float   ramp;        // StrafeSpeed * ramp at the engine's strafe ramp
   float   lean;        // StrafeRollAngle * lean at the four lean sites
   float   roll;        // replaces the mControlStrafe roll load
   void*   flyer;       // flagged flyer being updated, else null
   float   lat[3];      // added into mVelocity before integration
   uint8_t latActive;   // integrate_hook adds lat only when set
   uint8_t latApplied;  // integrate_hook sets it once it has added lat
   uint8_t fwdLock;     // pin the forward speed, and skip the FLYING rescale
   // Diagnostics for integrate_hook's anomaly report
   struct LatRecord* rec;
   float   dt;
   float   input;
   float   strafe;
   float   adopted;     // strafe adopted this frame, or 0
   uint8_t adopt;       // 1 when this frame adopted instead of subtracting
};
static StrafeInputs s_in = {};

// ---------------------------------------------------------------------------
// Game function types
// ---------------------------------------------------------------------------

using fn_hash_string_t = uint32_t(__cdecl*)(const char*);

// EntityFlyerClass::SetProperty - __thiscall(this, uint hash, const char* value)
using fn_SetProperty_t = void(__fastcall*)(void* ecx, void* edx,
                                           unsigned int hash, const char* value);

// EntityFlyerClass::Derive - __thiscall(this, uint hash) -> new EntityClass*.
using fn_Derive_t = void*(__fastcall*)(void* ecx, void* edx, unsigned int hash);

// EntityFlyer::Update - __thiscall(this = Controllable subobject, float dt) -> bool
using fn_Update_t = bool(__fastcall*)(void* ecx, void* edx, float dt);

using fn_init_state_t = void(__cdecl*)();

static fn_hash_string_t fn_hash_string       = nullptr;
static fn_SetProperty_t original_SetProperty = nullptr;
static fn_Derive_t      original_Derive      = nullptr;
static fn_Update_t      original_Update      = nullptr;
static fn_init_state_t  original_init_state  = nullptr;

static uint32_t g_propHash = 0; // hash("EnableStrafe"), computed lazily

// ---------------------------------------------------------------------------
// Classes with the flag set
// ---------------------------------------------------------------------------

static constexpr int kMaxClasses = 32;

static void* g_enabled[kMaxClasses] = {};
static int   g_enabledCount = 0;

static bool isEnabled(const void* cls)
{
   for (int i = 0; i < g_enabledCount; i++) {
      if (g_enabled[i] == cls) return true;
   }
   return false;
}

static void setEnabled(void* cls, bool on)
{
   for (int i = 0; i < g_enabledCount; i++) {
      if (g_enabled[i] != cls) continue;
      if (!on) g_enabled[i] = g_enabled[--g_enabledCount]; // ODF override back to 0
      return;
   }
   if (!on) return;
   if (g_enabledCount >= kMaxClasses) {
      get_gamelog()("[EnableStrafe] class table full (%d), ignoring\n", kMaxClasses);
      return;
   }
   g_enabled[g_enabledCount++] = cls;
}

// ---------------------------------------------------------------------------
// Per-flyer record of the sideways velocity added last frame
// ---------------------------------------------------------------------------
// FLYING blends the new velocity into the previous one, so the vector added
// before last frame's integration has to come back out before the engine sees
// mVelocity again.  Keyed by the Controllable pointer.  A record is only
// trusted while the flyer's mSetStrafe still holds exactly what we wrote: a
// new flyer at a reused address, a network update or anything else that
// touched it simply gets adopted fresh instead.

static constexpr int kMaxFlyers = 128;

struct LatRecord {
   void*    key;
   float    lat[3];
   float    strafe;
   uint32_t stamp;
   bool     applied;
   bool     hasPrevV;
   float    prevV[3];   // mVelocity after last frame's integrate_hook
};

static LatRecord g_lat[kMaxFlyers] = {};
static uint32_t  g_latStamp = 0;

static LatRecord* lat_record(void* key)
{
   LatRecord* freeSlot = nullptr;
   LatRecord* oldest   = &g_lat[0];
   for (LatRecord& r : g_lat) {
      if (r.key == key) return &r;
      if (!r.key && !freeSlot) freeSlot = &r;
      if (r.stamp < oldest->stamp) oldest = &r;
   }
   LatRecord* r = freeSlot ? freeSlot : oldest;
   *r = {};
   r->key = key;
   return r;
}

static void lat_forget(void* key)
{
   for (LatRecord& r : g_lat) {
      if (r.key == key) r = {};
   }
}

// ---------------------------------------------------------------------------
// Hook: EntityFlyerClass::SetProperty
// ---------------------------------------------------------------------------

static void __fastcall hooked_SetProperty(void* ecx, void* /*edx*/,
                                          unsigned int hash, const char* value)
{
   // Lazy: game code cannot run inside the install window (see
   // droideka_ball_mode.cpp for the DEP reasoning).
   if (g_propHash == 0 && fn_hash_string)
      g_propHash = fn_hash_string("EnableStrafe");

   if (g_propHash != 0 && hash == g_propHash) {
      // Parsed as an int so a later mode 2 needs no rename; any nonzero is on.
      if (ecx && value) setEnabled(ecx, std::atoi(value) != 0);
      return;
   }

   original_SetProperty(ecx, nullptr, hash, value);
}

// ---------------------------------------------------------------------------
// Hook: EntityFlyerClass::Derive
// ---------------------------------------------------------------------------
// ODF inheritance copy-constructs the child from the parent; our flag lives in
// a side table, so carry it across by hand. The child's own ODF lines are
// applied afterwards, so `EnableStrafe = 0` on a child still overrides.

static void* __fastcall hooked_Derive(void* ecx, void* /*edx*/, unsigned int hash)
{
   void* derived = original_Derive(ecx, nullptr, hash);
   if (derived && ecx && isEnabled(ecx)) setEnabled(derived, true);
   return derived;
}

// ---------------------------------------------------------------------------
// Hook: EntityFlyer::Update
// ---------------------------------------------------------------------------

template<class T> static T& field(void* base, int off)
{
   return *(T*)((uintptr_t)base + off);
}

static float move_toward(float cur, float target, float maxStep)
{
   if (target > cur) return (cur + maxStep < target) ? cur + maxStep : target;
   return (cur - maxStep > target) ? cur - maxStep : target;
}

// Right axis of the flight frame F = RotZ(-bankRoll) * RotX(-bankPitch) * World,
// i.e. row 0 of that product: cos(r)*R - sin(r)cos(p)*U + sin(r)sin(p)*F.
static void flight_right(void* ecx, float out[3])
{
   const float r = field<float>(ecx, s_layout.mBankRoll);
   const float p = field<float>(ecx, s_layout.mBankPitch);
   const float a = std::cos(r);
   const float b = -std::sin(r) * std::cos(p);
   const float c = std::sin(r) * std::sin(p);
   const float* R = &field<float>(ecx, kWorldRight);
   const float* U = &field<float>(ecx, kWorldUp);
   const float* F = &field<float>(ecx, kWorldFwd);
   for (int i = 0; i < 3; i++) out[i] = a * R[i] + b * U[i] + c * F[i];
}

static bool same_bits(float a, float b)
{
   uint32_t x, y;
   std::memcpy(&x, &a, 4);
   std::memcpy(&y, &b, 4);
   return x == y;
}

static bool update_flagged(void* ecx, float dt, uintptr_t cls, float axis)
{
   const float input = axis < -1.0f ? -1.0f : (axis > 1.0f ? 1.0f : axis);
   const int   state = field<int>(ecx, s_layout.mState);
   const bool  trick = field<float>(ecx, s_layout.mSideRollX) != 0.0f ||
                       field<float>(ecx, s_layout.mSideRollX + 4) != 0.0f;

   s_in.roll    = 0.0f; // the strafe axis never rolls a flagged class
   s_in.flyer   = ecx;
   s_in.fwdLock = 1;    // symmetric reverse, see the header

   const bool ours = !trick && dt > 0.0f &&
                     (state == kStateTakeoff || state == kStateFlying ||
                      state == kStateLanding);
   if (!ours) {
      // Side roll / flip: the engine does the trick with the stick as its strafe
      // input.  LANDED / CRASHING: stock.  Either way the engine now owns
      // whatever sideways motion there is, so drop our record.
      s_in.ramp = trick ? input : 0.0f;
      s_in.lean = trick ? input : 0.0f;
      lat_forget(ecx);
      return original_Update(ecx, nullptr, dt);
   }

   LatRecord* rec   = lat_record(ecx);
   float*     v     = &field<float>(ecx, s_layout.mVelocity);
   float&     fieldStrafe = field<float>(ecx, s_layout.mSetStrafe);
   float      right[3];
   flight_right(ecx, right);

   const float strafeSpeed = field<float>((void*)cls, s_layout.clsStrafeSpeed);
   const float strafeLimit = std::fabs(strafeSpeed) +
                             std::fabs(field<float>((void*)cls, s_layout.clsTrickSideRollStrafeSpeed));

   float prev = fieldStrafe;
   s_in.adopt   = 0;
   s_in.adopted = 0.0f;
   if (state == kStateFlying) {
      if (rec->applied && same_bits(rec->strafe, prev)) {
         // Take last frame's strafe back out so the engine blends only its own
         // velocity.  Only take out what is still there: if a collision since
         // then stopped or reversed that motion, subtracting the full vector
         // would leave the engine a phantom velocity the other way.
         const float mag = std::sqrt(rec->lat[0] * rec->lat[0] + rec->lat[1] * rec->lat[1] +
                                     rec->lat[2] * rec->lat[2]);
         if (mag > 1e-4f) {
            const float inv  = 1.0f / mag;
            const float cur  = (v[0] * rec->lat[0] + v[1] * rec->lat[1] + v[2] * rec->lat[2]) * inv;
            const float take = cur < 0.0f ? 0.0f : (cur > mag ? mag : cur);
            for (int i = 0; i < 3; i++) v[i] -= rec->lat[i] * inv * take;
         }
      }
      else {
         // First frame, or coming back from a trick: adopt the sideways motion
         // already there as the starting strafe, so nothing jumps.  Clamped to
         // what a strafe can reach; anything beyond stays the engine's own.
         const float lat = v[0] * right[0] + v[1] * right[1] + v[2] * right[2];
         float adopted = -lat;
         if (adopted >  strafeLimit) adopted =  strafeLimit;
         if (adopted < -strafeLimit) adopted = -strafeLimit;
         for (int i = 0; i < 3; i++) v[i] += right[i] * adopted; // v -= right * (-adopted)
         prev = adopted;
         s_in.adopt   = 1;
         s_in.adopted = adopted;
      }
   }

   const float accel       = field<float>((void*)cls, s_layout.clsAcceleration);
   const float strafe      = move_toward(prev, input * strafeSpeed, accel * dt);
   const float scale       = (state == kStateFlying)
                                ? 1.0f : field<float>(ecx, s_layout.mFlightRatio);

   // The engine's own term is `- F.right * mSetStrafe`; keep its sign.
   for (int i = 0; i < 3; i++) s_in.lat[i] = -strafe * scale * right[i];
   s_in.ramp       = 0.0f;  // engine blind to strafe...
   s_in.lean       = input; // ...but the bank still leans into it
   s_in.latActive  = 1;
   s_in.latApplied = 0;
   s_in.rec        = rec;
   s_in.dt         = dt;
   s_in.input      = input;
   s_in.strafe     = strafe;
   fieldStrafe     = 0.0f;

   const bool alive = original_Update(ecx, nullptr, dt);
   s_in.latActive = 0;

   // A false return means the flyer is being removed; leave it alone.
   if (!alive) {
      lat_forget(ecx);
      return false;
   }

   fieldStrafe  = strafe; // network, and next frame's ramp
   rec->applied = s_in.latApplied != 0;
   rec->strafe  = strafe;
   rec->stamp   = ++g_latStamp;
   for (int i = 0; i < 3; i++) rec->lat[i] = s_in.lat[i];
   return true;
}

static bool __fastcall hooked_Update(void* ecx, void* /*edx*/, float dt)
{
   const StrafeInputs saved = s_in; // re-entrancy guard

   const float axis = ecx ? field<float>(ecx, kControlStrafe) : 0.0f;

   uintptr_t cls = 0;
   if (g_enabledCount > 0 && ecx) {
      cls = field<uintptr_t>(ecx, s_layout.mClass);
      if (cls && !isEnabled((void*)cls)) cls = 0;
   }

   bool result;
   if (!cls) {
      s_in = {};
      s_in.roll = axis; // the stock load; ramp/lean stay the stock constant 0
      result = original_Update(ecx, nullptr, dt);
   }
   else {
      result = update_flagged(ecx, dt, cls, axis);
   }

   s_in = saved;
   return result;
}

// ---------------------------------------------------------------------------
// Hook: init_state (non-modtools only - see flyer_enable_strafe_reset)
// ---------------------------------------------------------------------------

static void __cdecl hooked_init_state()
{
   original_init_state();
   flyer_enable_strafe_reset();
}

// ---------------------------------------------------------------------------
// Site patching
// ---------------------------------------------------------------------------

static constexpr int kMaxSites = 9;

struct PatchedSite {
   uint8_t* at;
   uint8_t  orig[8];
   int      len;
};

static PatchedSite g_sites[kMaxSites] = {};
static int         g_siteCount = 0;
static uint8_t*    g_cave      = nullptr;
static int         g_caveUsed  = 0;

static constexpr int kCaveSize = 256;

static void remember(uint8_t* at, int len)
{
   PatchedSite& s = g_sites[g_siteCount++];
   s.at  = at;
   s.len = len;
   std::memcpy(s.orig, at, len);
}

// Restore everything written so far (used on a failed install and on uninstall).
static void restore_sites(bool installWindow)
{
   for (int i = 0; i < g_siteCount; i++) {
      if (installWindow) std::memcpy(g_sites[i].at, g_sites[i].orig, g_sites[i].len);
      else               protected_write(g_sites[i].at, g_sites[i].orig, g_sites[i].len);
   }
   g_siteCount = 0;
}

static uint8_t* cave_alloc(int size)
{
   if (!g_cave) {
      g_cave = (uint8_t*)VirtualAlloc(nullptr, kCaveSize, MEM_RESERVE | MEM_COMMIT,
                                      PAGE_EXECUTE_READWRITE);
      if (!g_cave) return nullptr;
   }
   if (g_caveUsed + size > kCaveSize) return nullptr;
   uint8_t* p = g_cave + g_caveUsed;
   g_caveUsed += size;
   return p;
}

static uint32_t abs32(const void* p) { return (uint32_t)(uintptr_t)p; }

static bool integrate_site_ok(uintptr_t exe_base)
{
   const uint8_t* site = (const uint8_t*)resolve(exe_base, g_addr->flyer_strafe_integrate);
   return std::memcmp(site, s_layout.leaOrig, sizeof(s_layout.leaOrig)) == 0;
}

// Rare-event report: if a flagged flyer's velocity goes far past anything its
// class can reach, or jumps by an absurd amount in one frame, log what this
// frame looked like.  Rate-limited; costs nothing while flight is sane.
static void report_anomaly(void* f, const float* v, float S, float d)
{
   LatRecord* rec = s_in.rec;
   if (!rec || !s_in.latActive || !(s_in.dt > 0.0f)) return;

   const uintptr_t cls = field<uintptr_t>(f, s_layout.mClass);
   const float maxSpeed  = std::fmax(field<float>((void*)cls, s_layout.clsMaxSpeed),
                                     field<float>((void*)cls, s_layout.clsBoostSpeed));
   const float strafeCap = std::fabs(field<float>((void*)cls, s_layout.clsStrafeSpeed)) +
                           std::fabs(field<float>((void*)cls, s_layout.clsTrickSideRollStrafeSpeed));
   const float cap = (std::fabs(maxSpeed) + strafeCap + 5.0f) * 1.25f;

   const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
   float jump = 0.0f;
   if (rec->hasPrevV) {
      const float dx = v[0] - rec->prevV[0], dy = v[1] - rec->prevV[1], dz = v[2] - rec->prevV[2];
      jump = std::sqrt(dx * dx + dy * dy + dz * dz);
   }
   for (int i = 0; i < 3; i++) rec->prevV[i] = v[i];
   rec->hasPrevV = true;

   // A change bigger than half the cap within one frame is not flight.
   if (speed <= cap && jump <= cap * 0.5f) return;

   static DWORD s_lastLog = 0;
   const DWORD now = GetTickCount();
   if (now - s_lastLog < 1000) return;
   s_lastLog = now;

   get_gamelog()("[EnableStrafe] anomaly: speed %.1f (cap %.1f) jump %.1f | state %d dt %.4f "
                 "S %.2f d %.2f | input %.2f strafe %.2f lat %.2f %.2f %.2f | %s %.2f\n",
                 speed, cap, jump, field<int>(f, s_layout.mState), s_in.dt, S, d,
                 s_in.input, s_in.strafe, s_in.lat[0], s_in.lat[1], s_in.lat[2],
                 s_in.adopt ? "adopted" : "subtracted", s_in.adopted);
}

// Runs at the head of the position integration, after the state's velocity
// code (and FLYING's rescale) and before `position += dt * mVelocity`.
static void __cdecl integrate_hook()
{
   void* f = s_in.flyer;
   if (!f) return; // not a flagged flyer: stock

   float* v = &field<float>(f, s_layout.mVelocity);

   if (s_in.latActive) {
      for (int i = 0; i < 3; i++) v[i] += s_in.lat[i];
      s_in.latApplied = 1;
   }

   // Pin the forward component to the throttle target.  Where the engine's
   // rescale already ran it is already there; this only acts when braking out
   // of reverse.  The strafe above lies along the flight frame's right axis,
   // which is perpendicular to World.fwd, so it does not change d.
   const float S = field<float>(f, s_layout.mSetSpeed);
   float d = 0.0f;
   if (s_in.fwdLock && field<int>(f, s_layout.mState) == kStateFlying) {
      const float* fwd = &field<float>(f, kWorldFwd);
      d = v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2];
      const float dS = S - d;
      for (int i = 0; i < 3; i++) v[i] += fwd[i] * dS;
   }

   report_anomaly(f, v, S, d);
}

// FLYING's `if (d > S || (S > d && d > 0)) v *= S / d` scales the whole velocity
// by S/d.  Near a hover d is tiny, so pulling away multiplies any drift the ship
// has several times over for a few frames.  integrate_hook already pins the
// forward component, which is all the rescale is for, so for a flagged class
// the scale is skipped:
//      CMP  byte [s_in.fwdLock], 0
//      JNE  end_of_scale_block
//      <displaced instruction(s)>
//      JMP  back
// Flags are dead at the site on every build, and the skipped block only writes
// registers that are rewritten before their next read (x87 empty on modtools;
// xmm0/xmm1 on Steam/GOG).
static bool rescale_site_ok(uintptr_t exe_base)
{
   const uint8_t* site = (const uint8_t*)resolve(exe_base, g_addr->flyer_strafe_rescale);
   return std::memcmp(site, s_layout.rescaleOrig, s_layout.rescaleLen) == 0;
}

static bool patch_rescale(uintptr_t exe_base)
{
   uint8_t*  site = (uint8_t*)resolve(exe_base, g_addr->flyer_strafe_rescale);
   uint8_t*  end  = (uint8_t*)resolve(exe_base, g_addr->flyer_strafe_rescale_end);
   const int len  = s_layout.rescaleLen;
   uint8_t*  c    = cave_alloc(32);
   if (!c) return false;

   int o = 0;
   c[o++] = 0x80; c[o++] = 0x3D;                      // CMP byte [abs32], 0
   *(uint32_t*)(c + o) = abs32(&s_in.fwdLock); o += 4;
   c[o++] = 0x00;
   c[o++] = 0x0F; c[o++] = 0x85;                      // JNE rel32
   *(int32_t*)(c + o) = x86::rel32(c + o + 4, end); o += 4;
   std::memcpy(c + o, site, len); o += len;
   x86::emit_jmp(c, o, site + len);

   remember(site, len);
   x86::write_branch(site, x86::kJmp, c, len);
   return true;
}

// At the head of the position integration, `LEA EAX,[ESP+disp32]` becomes a JMP
// to:
//      CALL integrate_hook
//      LEA  EAX,[ESP+disp32]   ; displaced, ESP is back where it was after the call
//      JMP  back
static bool patch_integrate(uintptr_t exe_base)
{
   uint8_t* site = (uint8_t*)resolve(exe_base, g_addr->flyer_strafe_integrate);
   uint8_t* c    = cave_alloc(17);
   if (!c) return false;

   x86::encode_branch(c, c, x86::kCall, &integrate_hook);
   std::memcpy(c + 5, site, 7);
   x86::emit_jmp(c, 12, site + 7);

   remember(site, 7);
   x86::write_branch(site, x86::kJmp, c, 7);
   return true;
}

// modtools: 5 x `D8 0D <&zero>` (FMUL dword [abs]) and 1 x `D9 83 84000000`.
static bool patch_modtools(uintptr_t exe_base)
{
   const uintptr_t lean[] = {
      g_addr->flyer_strafe_lean_takeoff,
      g_addr->flyer_strafe_lean_flying,
      g_addr->flyer_strafe_lean_landing,
      g_addr->flyer_strafe_lean_crashing,
   };
   const uint32_t zero = (uint32_t)resolve(exe_base, g_addr->flyer_strafe_zero_const);
   uint8_t* ramp = (uint8_t*)resolve(exe_base, g_addr->flyer_strafe_ramp_mul);
   uint8_t* roll = (uint8_t*)resolve(exe_base, g_addr->flyer_strafe_roll_read);

   // Verify every site before touching any.
   auto isFmulZero = [&](const uint8_t* p) {
      return p[0] == 0xD8 && p[1] == 0x0D && *(const uint32_t*)(p + 2) == zero;
   };
   static constexpr uint8_t kRollOrig[6] = {0xD9, 0x83, 0x84, 0x00, 0x00, 0x00};
   if (std::memcmp(roll, kRollOrig, sizeof(kRollOrig)) != 0) return false;
   if (!isFmulZero(ramp)) return false;
   for (uintptr_t a : lean) {
      if (!isFmulZero((const uint8_t*)resolve(exe_base, a))) return false;
   }
   if (!integrate_site_ok(exe_base) || !rescale_site_ok(exe_base)) return false;

   remember(ramp, 6);
   *(uint32_t*)(ramp + 2) = abs32(&s_in.ramp);
   for (uintptr_t a : lean) {
      uint8_t* p = (uint8_t*)resolve(exe_base, a);
      remember(p, 6);
      *(uint32_t*)(p + 2) = abs32(&s_in.lean);
   }
   remember(roll, 6);
   roll[1] = 0x05; // FLD dword [disp32]
   *(uint32_t*)(roll + 2) = abs32(&s_in.roll);
   return patch_integrate(exe_base) && patch_rescale(exe_base);
}

// Steam/GOG: 5 x `F3 0F 59 (C0|A<<3|7) F3 0F 59 xx` and 1 x `F3 0F 10 97 84000000`.
static bool patch_release(uintptr_t exe_base)
{
   struct Site { uintptr_t addr; const float* input; };
   const Site mul[] = {
      {g_addr->flyer_strafe_ramp_mul,      &s_in.ramp},
      {g_addr->flyer_strafe_lean_takeoff,  &s_in.lean},
      {g_addr->flyer_strafe_lean_flying,   &s_in.lean},
      {g_addr->flyer_strafe_lean_landing,  &s_in.lean},
      {g_addr->flyer_strafe_lean_crashing, &s_in.lean},
   };
   uint8_t* roll = (uint8_t*)resolve(exe_base, g_addr->flyer_strafe_roll_read);

   static constexpr uint8_t kRollOrig[8] = {0xF3, 0x0F, 0x10, 0x97, 0x84, 0x00, 0x00, 0x00};
   if (std::memcmp(roll, kRollOrig, sizeof(kRollOrig)) != 0) return false;
   for (const Site& s : mul) {
      const uint8_t* p = (const uint8_t*)resolve(exe_base, s.addr);
      const bool firstIsMulXmm7 = p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x59 &&
                                  (p[3] & 0xC7) == 0xC7;             // mod=11, rm=xmm7
      const bool secondIsRegMul = p[4] == 0xF3 && p[5] == 0x0F && p[6] == 0x59 &&
                                  (p[7] & 0xC0) == 0xC0;             // mod=11
      if (!firstIsMulXmm7 || !secondIsRegMul) return false;
   }
   if (!integrate_site_ok(exe_base) || !rescale_site_ok(exe_base)) return false;

   for (const Site& s : mul) {
      uint8_t* p = (uint8_t*)resolve(exe_base, s.addr);
      uint8_t* cave = cave_alloc(17); // 8 MULSS abs + 4 displaced MULSS + 5 JMP
      if (!cave) return false;
      const uint8_t reg = (p[3] >> 3) & 7; // destination xmm of the dead MULSS

      // MULSS xmmReg, dword [input]
      cave[0] = 0xF3; cave[1] = 0x0F; cave[2] = 0x59;
      cave[3] = (uint8_t)((reg << 3) | 0x05);
      *(uint32_t*)(cave + 4) = abs32(s.input);
      std::memcpy(cave + 8, p + 4, 4);     // the displaced register MULSS
      x86::emit_jmp(cave, 12, p + 8);

      remember(p, 8);
      x86::write_branch(p, x86::kJmp, cave, 8);
   }

   remember(roll, 8);
   roll[3] = 0x15; // MOVSS xmm2, dword [disp32]
   *(uint32_t*)(roll + 4) = abs32(&s_in.roll);
   return patch_integrate(exe_base) && patch_rescale(exe_base);
}

// ---------------------------------------------------------------------------
// Install / Uninstall / Reset
// ---------------------------------------------------------------------------

void flyer_enable_strafe_install(uintptr_t exe_base)
{
   switch (g_build) {
   case GameBuild::Modtools: s_layout = kLayoutModtools; break;
   case GameBuild::Steam:
   case GameBuild::GOG:      s_layout = kLayoutRelease;  break;
   default: return; // unknown build
   }

   if (g_addr->hash_string == 0 || g_addr->flyer_class_set_property == 0 ||
       g_addr->flyer_class_derive == 0 || g_addr->flyer_update == 0 ||
       g_addr->flyer_strafe_roll_read == 0 || g_addr->flyer_strafe_ramp_mul == 0 ||
       g_addr->flyer_strafe_lean_takeoff == 0 || g_addr->flyer_strafe_lean_flying == 0 ||
       g_addr->flyer_strafe_lean_landing == 0 || g_addr->flyer_strafe_lean_crashing == 0 ||
       g_addr->flyer_strafe_integrate == 0 || g_addr->flyer_strafe_rescale == 0 ||
       g_addr->flyer_strafe_rescale_end == 0)
      return;
   if (g_build == GameBuild::Modtools && g_addr->flyer_strafe_zero_const == 0)
      return;

   // Byte patches first: they are what the Update detour feeds, so without them
   // the property would parse and do nothing. .text is RW during install.
   const bool patched = (g_build == GameBuild::Modtools) ? patch_modtools(exe_base)
                                                         : patch_release(exe_base);
   if (!patched) {
      restore_sites(true);
      if (g_cave) { VirtualFree(g_cave, 0, MEM_RELEASE); g_cave = nullptr; }
      g_caveUsed = 0;
      install_log("[EnableStrafe] unexpected bytes at a strafe site, not installed");
      return;
   }

   fn_hash_string = (fn_hash_string_t)resolve(exe_base, g_addr->hash_string);
   g_propHash     = 0; // computed lazily in hooked_SetProperty

   original_SetProperty = (fn_SetProperty_t)resolve(exe_base, g_addr->flyer_class_set_property);
   original_Derive      = (fn_Derive_t)     resolve(exe_base, g_addr->flyer_class_derive);
   original_Update      = (fn_Update_t)     resolve(exe_base, g_addr->flyer_update);

   // On modtools lua_hooks already detours init_state and calls our reset.
   const bool ownInitState = (g_build != GameBuild::Modtools) && g_addr->init_state != 0;
   if (ownInitState)
      original_init_state = (fn_init_state_t)resolve(exe_base, g_addr->init_state);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   DetourAttach(&(PVOID&)original_Derive,      hooked_Derive);
   DetourAttach(&(PVOID&)original_Update,      hooked_Update);
   if (ownInitState)
      DetourAttach(&(PVOID&)original_init_state, hooked_init_state);
   DetourTransactionCommit();
}

void flyer_enable_strafe_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_SetProperty) DetourDetach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   if (original_Derive)      DetourDetach(&(PVOID&)original_Derive,      hooked_Derive);
   if (original_Update)      DetourDetach(&(PVOID&)original_Update,      hooked_Update);
   if (original_init_state)  DetourDetach(&(PVOID&)original_init_state,  hooked_init_state);
   DetourTransactionCommit();

   // Sections are re-protected by now, so the restore cannot be a plain write.
   restore_sites(false);
   if (g_cave) {
      VirtualFree(g_cave, 0, MEM_RELEASE);
      g_cave = nullptr;
   }
   g_caveUsed = 0;
}

void flyer_enable_strafe_reset()
{
   std::memset(g_enabled, 0, sizeof(g_enabled));
   g_enabledCount = 0;
   std::memset(g_lat, 0, sizeof(g_lat));
   g_latStamp = 0;
}
