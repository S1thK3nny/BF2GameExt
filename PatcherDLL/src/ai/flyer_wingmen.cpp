#include "pch.h"
#include "flyer_wingmen.hpp"
#include "flyer_real_flight.hpp"
#include "flyer_wingmen_core.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "entity/odf_gameext_props.hpp"
#include "game/Battlefront2/Source/Character.h"
#include "util/install_log.hpp"

#include <detours.h>

#include <cmath>
#include <cstring>
#include <unordered_map>

// See flyer_wingmen.hpp for the behaviour and the reads.

bool  g_flyerWingmen        = false;
int   g_flyerWingmenCount   = flyer_wingmen::kDefaultWingmen;
float g_flyerWingmenSpacing = flyer_wingmen::kDefaultSpacing;
bool  g_flyerWingmenAuto    = true;

namespace {

using flyer_wingmen::ClassKeys;
using flyer_wingmen::Frame;
using flyer_wingmen::Stats;
using flyer_wingmen::Trim;
using flyer_wingmen::V3;

// ---- Modtools -----------------------------------------------------------------
constexpr uint32_t kFly_Controllable = 0x240;   // UpdateIndirect's `this`, from the entity
constexpr uint32_t kFly_Ctrl         = 0x308;   // Controllable::mCtrl
constexpr uint32_t kFly_State        = 0x5A4;
constexpr uint32_t kFly_Flags        = 0x5F4;
constexpr uint32_t kFly_FreeFlyTime  = 0x1CDC;
constexpr uint32_t kFly_TargetSpeed  = 0x598;   // the speed its throttle asks for (Controllable +0x358)
constexpr uint32_t kCtl_Squad        = 0x60;    // the Controllable's squad-key trigger
constexpr uint32_t kSquadPressed     = 3;       // down, and changed this update
constexpr int      kStateFlying      = 2;
constexpr uint8_t  kFlagPaths        = 0x08;    // flies its paths when it may
constexpr uint8_t  kFlagSliding      = 0x10;    // sliding round a barrier
constexpr uint8_t  kFlagTrick        = 0x03;    // in a trick: rolling (1, RollAdd) or flipping (2, FlipAdd)
constexpr float    kHoldFreeFly      = 0.5f;    // s of free flight a path flyer is held to
constexpr uint32_t kFly_SideRoll     = 0x5D8;   // mSideRoll: a side roll goes left with x below 0
constexpr uint32_t kFly_LastTrick    = 0x5E4;   // mLastTrickPerformed, set when DoTrick goes ahead
constexpr uint32_t kFly_EnergyValue  = 0x61C;   // the energy bar (+0x618): its energy ...
constexpr uint32_t kFly_EnergyFlags  = 0x620;   // ... and flags, bit 0 spent out (SpendEnergy refuses)
constexpr int      kTrickSideRoll    = 6;       // EntityFlyer::Trick: 0-5 the flips, 6 SIDE_ROLL
constexpr int      kTrickCount       = 7;

constexpr uint32_t kUC_LowLevel   = 0x2C4;
constexpr uint32_t kLL_Dest       = 0x1C;
constexpr size_t   kDestSize      = 0x34;
constexpr uint32_t kDest_Has      = 0x00;
constexpr uint32_t kDest_Pos      = 0x04;
constexpr uint32_t kDest_Original = 0x10;
constexpr uint32_t kDest_Dir      = 0x1C;
constexpr uint32_t kDest_Safe     = 0x28;
constexpr uint32_t kDest_Taper    = 0x2C;
constexpr uint32_t kDest_Speed    = 0x30;
constexpr int      kSafeHeightFull = 0;

// ---- Any build ------------------------------------------------------------------
constexpr uint32_t kVt_IsRtti       = 0x00;   // a GameObject's or a Controller's
constexpr uint32_t kVt_GetVelocity  = 0x44;   // GameObject
constexpr uint32_t kGO_Class        = 0x8;    // GameObject: its class ...
constexpr uint32_t kClass_Id        = 0x18;   // ... whose mId is the PblHash of its ODF name
constexpr uint32_t kGO_MatrixIface  = 0xC;    // the interface a GameObject's matrix comes from ...
constexpr uint32_t kVt_GetMatrix    = 0x40;   // ... at its vptr +0x40
constexpr uint32_t kGO_Flags        = 0x1FC;  // bit 3: alive
constexpr uint32_t kGO_HandleId     = 0x204;
constexpr uint32_t kGO_Team         = 0x234;  // the low 4 bits, signed
constexpr uint32_t kCtrl_Owner      = 0x04;   // Controller::mOwner, the Controllable
constexpr uint32_t kOwner_Trackable = 0x18;   // a Controllable's Trackable interface ...
constexpr uint32_t kVt_GetTrackable = 0x20;   // ... whose vptr +0x20 gives the GameObject
constexpr uint32_t kNodeNext        = 0x04;   // sAICtrls nodes
constexpr uint32_t kNodePayload     = 0x0C;
constexpr int      kMaxNodes        = 4096;
constexpr uint32_t kRttiFlyer          = pbl_hash("EntityFlyer");
constexpr uint32_t kRttiUnitController = pbl_hash("UnitController");
constexpr uint32_t kHashClass = pbl_hash("WingmanClass");
constexpr uint32_t kHashCount = pbl_hash("WingmanCount");
constexpr uint32_t kHashAuto  = pbl_hash("WingmanAuto");
constexpr uint32_t kHashSlot  = pbl_hash("WingmanSlot");

constexpr double kTickSeconds   = 1.0;
constexpr double kReportSeconds = 5.0;
constexpr double kKeyRepeat     = 0.3;   // s: one press, seen by every flyer's update that frame
constexpr int    kMaxReports    = 400;

using fn_is_rtti_t   = bool(__thiscall*)(void* self, uint32_t hash);
using fn_get_obj_t   = uint8_t*(__thiscall*)(void* self);
using fn_get_vec_t   = const float*(__thiscall*)(void* self);
using fn_local_t     = uint8_t*(__cdecl*)(uint32_t localIndex);
using fn_update_t    = bool(__fastcall*)(void* ctrl, void* edx, float dt);
using fn_speed_t     = float(__fastcall*)(uint8_t* flyer, void* edx);
using fn_trick_t     = void(__fastcall*)(uint8_t* flyer, void* edx, int trick);
using fn_calc_dest_t = bool(__thiscall*)(void* flyer, float dt, void* dest, float* point, float* speed,
                                         float* myPos);

// Detoured: after the install each holds the original's trampoline.
fn_update_t s_update   = nullptr;
fn_speed_t  s_maxSpeed = nullptr;
fn_speed_t  s_minSpeed = nullptr;
fn_speed_t  s_midSpeed = nullptr;   // called, not detoured
fn_trick_t  s_doTrick  = nullptr;
uintptr_t   s_calcDest = 0;
fn_local_t  s_local    = nullptr;
uint8_t**   s_list     = nullptr;   // the global holding sAICtrls
bool        s_detoured = false;
bool        s_odf      = false;
double      s_ticksPerSecond = 0.0;

struct Site {
   uint8_t* at = nullptr;
   uint8_t  orig[5] = {};
};
Site s_callSite;

// ---- The ODF keys, per class ----------------------------------------------------------
std::unordered_map<const void*, ClassKeys> s_classes;

bool on_property(void* cls, uint32_t hash, const char* value)
{
   if (hash == kHashClass) {
      if (cls) {
         ClassKeys& k = s_classes[cls];
         bool any = false;
         flyer_wingmen::for_each_name(value, [&](const char* b, unsigned len) {
            flyer_wingmen::add_class(k, pbl_hash(b, len));
            any = true;
         });
         if (!any) flyer_wingmen::add_class(k, 0);   // an empty list: no class follows
      }
      return true;   // no stock SetProperty knows these
   }
   if (hash == kHashCount) {
      if (cls) flyer_wingmen::set_count(s_classes[cls], value);
      return true;
   }
   if (hash == kHashAuto) {
      if (cls) flyer_wingmen::set_auto(s_classes[cls], value);
      return true;
   }
   if (hash == kHashSlot) {
      if (cls)
         flyer_wingmen::add_slot(s_classes[cls], value, [](const char* b, unsigned n) { return pbl_hash(b, n); });
      return true;
   }
   return flyer_real_flight::on_property(cls, hash, value);   // shares this listener slot
}

// Every class is made from a base: it starts with the base's keys or none, so a
// reused address never keeps an old class's.
void on_derive(const void* parent, void* child)
{
   const auto it = s_classes.find(parent);
   if (it != s_classes.end()) s_classes[child] = flyer_wingmen::inherited(it->second);
   else s_classes.erase(child);
   flyer_real_flight::on_derive(parent, child);
}

// ---- The squadron -----------------------------------------------------------------
struct Wingman {
   uint8_t* obj = nullptr;     // the flyer
   uint32_t handle = 0;
   uint8_t* uc = nullptr;      // the UnitController flying it
   int      slot = 0;
   Trim     trim;
   Stats    stats;
   int      trick = -1;        // the leader's trick it copies at trickAt, or -1
   float    trickSide = 0.0f;  // which way the leader's side roll went (mSideRoll.x)
   double   trickAt = 0.0;
   bool     holding = false;   // flying straight through the leader's trick and its own
   double   holdUntil = 0.0;   // once its copy is done (or passed over)
};

// The leader's DoTrick, seen before it runs; it went ahead if the leader is in a
// trick by the next update.
struct LeaderTrick {
   int    requested = -1;
   float  side = 0.0f;
   double at = 0.0;
};
LeaderTrick s_leaderTrick;
int         s_trickLogs = 0;
constexpr int kMaxTrickLogs = 50;
bool        s_mirror = false;   // the formation left for right, after an odd number of the leader's flips

Wingman  s_wing[flyer_wingmen::kMaxWingmen];
int      s_count = 0;
uint8_t* s_leader = nullptr;
uint32_t s_leaderHandle = 0;
float    s_leaderSpeed = 0.0f;     // as last seen by a wingman's steering: how fast it flies ...
float    s_leaderTarget = 0.0f;    // ... and the speed its throttle asks for
bool     s_formUp = true;          // the order: form up, or break off
double   s_nextTick = 0.0;
double   s_nextReport = 0.0;
double   s_lastPress = -1e9;
int      s_reports = 0;

double seconds()
{
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return (double)t.QuadPart / s_ticksPerSecond;
}

bool is_rtti(void* obj, uint32_t hash)
{
   void** vt = *(void***)obj;
   return ((fn_is_rtti_t)vt[kVt_IsRtti / 4])(obj, hash);
}

uint8_t* object_of(uint8_t* controllable)
{
   void* const trackable = controllable + kOwner_Trackable;
   return ((fn_get_obj_t)(*(void***)trackable)[kVt_GetTrackable / 4])(trackable);
}

bool alive(const uint8_t* obj) { return ((*(const uint32_t*)(obj + kGO_Flags)) >> 3) & 1u; }
uint32_t handle_of(const uint8_t* obj) { return *(const uint32_t*)(obj + kGO_HandleId); }
int team_of(const uint8_t* obj) { return (int32_t)(*(const uint32_t*)(obj + kGO_Team) << 28) >> 28; }
bool flying(const uint8_t* flyer) { return *(const int*)(flyer + kFly_State) == kStateFlying; }
uint32_t class_id(const uint8_t* obj)
{
   const uint8_t* cls = *(const uint8_t* const*)(obj + kGO_Class);
   return cls ? *(const uint32_t*)(cls + kClass_Id) : 0;
}

const ClassKeys* keys_of(const uint8_t* obj)
{
   const auto it = s_classes.find(*(const void* const*)(obj + kGO_Class));
   return it == s_classes.end() ? nullptr : &it->second;
}

const float* matrix_of(uint8_t* obj)
{
   void* const iface = obj + kGO_MatrixIface;
   return ((fn_get_vec_t)(*(void***)iface)[kVt_GetMatrix / 4])(iface);
}

V3 velocity_of(uint8_t* obj)
{
   const float* v = ((fn_get_vec_t)(*(void***)obj)[kVt_GetVelocity / 4])(obj);
   return v ? V3{v[0], v[1], v[2]} : V3{};
}

V3 row(const float* m, int r) { return {m[r * 4], m[r * 4 + 1], m[r * 4 + 2]}; }   // right, up, forward, position

// The flyer the host's own player flies, while it flies.
uint8_t* player_flyer()
{
   uint8_t* const chr = s_local ? s_local(0) : nullptr;
   uint8_t* const vehicle = chr ? *(uint8_t**)(chr + layout::Character::kVehicle) : nullptr;
   if (!vehicle) return nullptr;
   uint8_t* const obj = object_of(vehicle);
   if (!obj || !is_rtti(obj, kRttiFlyer) || !alive(obj) || !flying(obj)) return nullptr;
   return obj;
}

// Whether this leader has wingmen at all: its class's keys decide where it has
// any, else the INI.
bool leads(const uint8_t* leader)
{
   const ClassKeys* k = keys_of(leader);
   return (flyer_wingmen::sets_any(k) || g_flyerWingmen) && flyer_wingmen::count_for(k, g_flyerWingmenCount) > 0;
}

Wingman* find(const uint8_t* obj)
{
   for (int i = 0; i < s_count; ++i)
      if (s_wing[i].obj == obj) return &s_wing[i];
   return nullptr;
}

// A wingman in formation, steered, held to its leader's speed and kept from tricks.
bool in_formation(const uint8_t* obj) { return s_formUp && s_leader && find(obj); }

void drop(int i, const char* why)
{
   install_log("[Wingmen] slot %d (#%u) let go: %s", s_wing[i].slot + 1, s_wing[i].handle, why);
   s_wing[i] = s_wing[--s_count];
   s_wing[s_count] = Wingman{};
}

void release_all(const char* why)
{
   if (s_count) install_log("[Wingmen] %d let go: %s", s_count, why);
   s_count = 0;
   for (auto& w : s_wing) w = Wingman{};
   s_mirror = false;   // a new formation starts the right way round
}

// Still a wingman? Why not, or null.
const char* lost(const Wingman& w, uint8_t* leader)
{
   if (handle_of(w.obj) != w.handle || !alive(w.obj)) return "gone";
   if (*(uint8_t* const*)(w.obj + kFly_Ctrl) != w.uc) return "flown by someone else";
   if (!flying(w.obj)) return "not flying";
   if (team_of(w.obj) != team_of(leader)) return "changed sides";
   if (!s_formUp) return nullptr;   // broken off, it may chase a fight anywhere
   const float* a = matrix_of(w.obj);
   const float* b = matrix_of(leader);
   if (!a || !b) return "no position";
   if (flyer_wingmen::length(row(a, 3) - row(b, 3)) > flyer_wingmen::kKeepRange) return "too far";
   return nullptr;
}

bool slot_taken(int slot)
{
   for (int i = 0; i < s_count; ++i)
      if (s_wing[i].slot == slot) return true;
   return false;
}

// The leader's free slots, in order, each to the nearest friendly AI flyer
// its class lets follow and the slot lets in (a WingmanSlot line may keep it
// for some classes).
void fill(uint8_t* leader)
{
   uint8_t* const list = s_list ? *s_list : nullptr;
   if (!list) return;
   const ClassKeys* keys = keys_of(leader);
   const int total = flyer_wingmen::count_for(keys, g_flyerWingmenCount);
   if (s_count >= total) return;
   const float* lm = matrix_of(leader);
   if (!lm) return;
   const V3 lp = row(lm, 3);
   const float leaderMax = s_maxSpeed(leader, nullptr);
   const int team = team_of(leader);

   // Every friendly AI flyer pool enough that may follow, the nearest first.
   constexpr int kMaxCandidates = 32;
   struct Candidate { uint8_t* obj; uint8_t* uc; uint32_t cls; float d; };
   Candidate pool[kMaxCandidates];
   int found = 0;
   uint8_t* node = *(uint8_t**)(list + kNodeNext);
   for (int i = 0; i < kMaxNodes && node; ++i) {
      uint8_t* const uc = *(uint8_t**)(node + kNodePayload);
      if (!uc) break;   // the list's end
      node = *(uint8_t**)(node + kNodeNext);
      if (!is_rtti(uc, kRttiUnitController)) continue;
      uint8_t* const owner = *(uint8_t**)(uc + kCtrl_Owner);
      if (!owner) continue;
      uint8_t* const obj = object_of(owner);
      if (!obj || obj == leader || !is_rtti(obj, kRttiFlyer) || !alive(obj) || !flying(obj)) continue;
      if (*(uint8_t* const*)(obj + kFly_Ctrl) != uc || team_of(obj) != team || find(obj)) continue;
      const float* m = matrix_of(obj);
      if (!m) continue;
      const float d = flyer_wingmen::length(row(m, 3) - lp);
      if (!(d <= flyer_wingmen::kRecruitRange)) continue;
      const uint32_t cls = class_id(obj);
      if (!flyer_wingmen::accepts(keys, cls, flyer_wingmen::can_keep_up(s_maxSpeed(obj, nullptr), leaderMax)))
         continue;
      if (found < kMaxCandidates) pool[found++] = {obj, uc, cls, d};
      else if (d < pool[kMaxCandidates - 1].d) pool[kMaxCandidates - 1] = {obj, uc, cls, d};
      else continue;
      for (int k = found - 1; k > 0 && pool[k].d < pool[k - 1].d; --k) {
         const Candidate t = pool[k];
         pool[k] = pool[k - 1];
         pool[k - 1] = t;
      }
   }
   bool used[kMaxCandidates] = {};
   for (int slot = 0; slot < total && s_count < flyer_wingmen::kMaxWingmen; ++slot) {
      if (slot_taken(slot)) continue;
      for (int c = 0; c < found; ++c) {
         if (used[c] || !flyer_wingmen::slot_accepts(keys, slot, pool[c].cls)) continue;
         used[c] = true;
         Wingman& w = s_wing[s_count++];
         w = Wingman{};
         w.obj = pool[c].obj;
         w.handle = handle_of(pool[c].obj);
         w.uc = pool[c].uc;
         w.slot = slot;
         install_log("[Wingmen] slot %d: #%u joined, %.0f m off, top speed %.0f m/s against the leader's %.0f",
                     slot + 1, w.handle, (double)pool[c].d, (double)s_maxSpeed(w.obj, nullptr), (double)leaderMax);
         break;
      }
   }
}

void report(double now)
{
   if (now < s_nextReport || s_reports >= kMaxReports) return;
   s_nextReport = now + kReportSeconds;
   for (int i = 0; i < s_count; ++i) {
      Stats& s = s_wing[i].stats;
      if (!s.samples) continue;
      const double n = s.samples;
      ++s_reports;
      install_log("[Wingmen] slot %d: off its slot by %.1f m on average, %.1f m at worst (slot %.1f m ahead, %.1f "
                  "right, %.1f up); %.0f m/s against the leader's %.0f; trim %.1f m up, %.1f m/s",
                  s_wing[i].slot + 1, s.distance / n, (double)s.worst, s.ahead / n, s.right / n, s.up / n,
                  s.speed / n, s.leaderSpeed / n, (double)s_wing[i].trim.climb, (double)s_wing[i].trim.speed);
      s = Stats{};
   }
}

void tick(double now)
{
   uint8_t* const leader = player_flyer();
   if (!leader || !leads(leader)) {
      release_all(leader ? "the player's flyer leads none" : "the player is not flying");
      s_leader = nullptr;
      s_leaderHandle = 0;
      return;
   }
   if (leader != s_leader || handle_of(leader) != s_leaderHandle) {
      release_all("a new leader");
      s_leader = leader;
      s_leaderHandle = handle_of(leader);
      s_formUp = true;
   }
   for (int i = s_count - 1; i >= 0; --i)
      if (const char* why = lost(s_wing[i], leader)) drop(i, why);
   const ClassKeys* keys = keys_of(leader);
   const int want = flyer_wingmen::count_for(keys, g_flyerWingmenCount);
   if (s_formUp && flyer_wingmen::auto_for(keys, g_flyerWingmenAuto) && s_count < want) fill(leader);
   report(now);
}

// The squad key, in the air: recruits when it leads none, else switches
// between forming up and breaking off.
void squad_key(double now)
{
   if (!s_leader || handle_of(s_leader) != s_leaderHandle) return;
   const uint32_t trigger = *(const uint32_t*)(s_leader + kFly_Controllable + kCtl_Squad);
   if ((trigger & kSquadPressed) != kSquadPressed || now - s_lastPress < kKeyRepeat) return;
   s_lastPress = now;
   if (!s_count) {
      s_formUp = true;
      fill(s_leader);
      install_log("[Wingmen] squad key: %s", s_count ? "form up" : "no one near enough to join");
   } else {
      s_formUp = !s_formUp;
      install_log("[Wingmen] squad key: %s", s_formUp ? "form up" : "break off");
   }
}

// ---- The leader's tricks, copied down the formation ----------------------------------
const char* trick_name(int trick, float side)
{
   static const char* const kNames[kTrickCount] = {"flip up", "flip down", "flip up right", "flip down right",
                                                    "flip up left", "flip down left", "side roll"};
   if (trick == kTrickSideRoll) return side < 0.0f ? "side roll left" : "side roll right";
   return trick >= 0 && trick < kTrickCount ? kNames[trick] : "trick";
}

// From DoTrick's stand-in, before the leader's trick runs: what was asked, and
// which way a side roll goes (the game sets mSideRoll.x from the strafe key
// first). A leader already in a trick is refused, so that call is passed over.
void note_leader_trick(const uint8_t* flyer, int trick)
{
   if (flyer != s_leader || !s_formUp || !s_count || (flyer[kFly_Flags] & kFlagTrick)) return;
   if (trick < 0 || trick >= kTrickCount) return;
   s_leaderTrick.requested = trick;
   s_leaderTrick.side = *(const float*)(flyer + kFly_SideRoll);
   s_leaderTrick.at = seconds();
}

// Once the leader is in its trick, each wingman in formation gets its turn.
void ripple_leader_trick(double now)
{
   if (s_leaderTrick.requested < 0) return;
   if (!s_leader || handle_of(s_leader) != s_leaderHandle || !s_formUp) {
      s_leaderTrick = LeaderTrick{};
      return;
   }
   if (!(s_leader[kFly_Flags] & kFlagTrick)) {
      if (now - s_leaderTrick.at > flyer_wingmen::kTrickWait) s_leaderTrick = LeaderTrick{};   // refused
      return;
   }
   // The trick it did: its class may have turned a flip into a side roll.
   const int done = *(const int*)(s_leader + kFly_LastTrick);
   const int trick = done >= 0 && done < kTrickCount ? done : s_leaderTrick.requested;
   int slots[flyer_wingmen::kMaxWingmen];
   for (int i = 0; i < s_count; ++i) slots[i] = s_wing[i].slot;
   for (int i = 0; i < s_count; ++i) {
      Wingman& w = s_wing[i];
      w.trick = trick;
      w.trickSide = s_leaderTrick.side;
      w.trickAt = now + flyer_wingmen::ripple_delay(flyer_wingmen::ripple_rank(w.slot, slots, s_count));
      w.holding = true;   // straight on until its copy is done
   }
   // A flip turns the leader round: its wingmen take the slots on the side they
   // come out on.
   if (flyer_wingmen::is_flip(trick)) s_mirror = !s_mirror;
   if (s_trickLogs < kMaxTrickLogs) {
      ++s_trickLogs;
      install_log("[Wingmen] the leader's %s: %d wingmen copy it, %.2f s apart%s", trick_name(trick, s_leaderTrick.side),
                  s_count, flyer_wingmen::kTrickRipple,
                  flyer_wingmen::is_flip(trick) ? (s_mirror ? "; the formation now mirrored" : "; the formation back the right way round") : "");
   }
   s_leaderTrick = LeaderTrick{};
}

// A wingman holding course goes back to its slot once its copy has ended and
// settled, or straight away when its turn came and it did not trick.
void update_hold(Wingman& w, double now)
{
   if (!w.holding || w.trick >= 0) return;   // still waiting for its turn
   if (w.obj[kFly_Flags] & kFlagTrick) {
      w.holdUntil = now + flyer_wingmen::kTrickSettle;   // in its copy
      return;
   }
   if (now >= w.holdUntil) w.holding = false;
}

// A wingman's turn: the same trick the same way, free of energy.
void copy_trick(Wingman& w, double now)
{
   const int trick = w.trick;
   w.trick = -1;
   w.holdUntil = now + flyer_wingmen::kTrickSettle;   // if it does not trick, it settles from here
   uint8_t* const f = w.obj;
   if (handle_of(f) != w.handle || !alive(f) || !flying(f) || !in_formation(f) || (f[kFly_Flags] & kFlagTrick)) return;
   if (trick == kTrickSideRoll) {
      float* const side = (float*)(f + kFly_SideRoll);
      side[0] = w.trickSide < 0.0f ? -1.0f : 1.0f;
      side[1] = 0.0f;
      side[2] = 0.0f;
   }
   // Whatever DoTrick spends goes back, and a spent-out bar does not refuse it.
   float& energy = *(float*)(f + kFly_EnergyValue);
   uint32_t& energyFlags = *(uint32_t*)(f + kFly_EnergyFlags);
   const float keptEnergy = energy;
   const uint32_t keptFlags = energyFlags;
   energyFlags = keptFlags & ~1u;
   s_doTrick(f, nullptr, trick);   // the trampoline, past the stand-in that keeps wingmen from tricks
   energy = keptEnergy;
   energyFlags = keptFlags;
}

// ---- EntityFlyer::UpdateIndirect ----------------------------------------------------
bool __fastcall hooked_update(void* ctrl, void* edx, float dt)
{
   __try {
      const double now = seconds();
      if (now >= s_nextTick) {
         s_nextTick = now + kTickSeconds;
         tick(now);
      }
      squad_key(now);
      uint8_t* const obj = (uint8_t*)ctrl - kFly_Controllable;
      ripple_leader_trick(now);
      if (Wingman* w = find(obj)) {
         if (w->trick >= 0 && now >= w->trickAt) copy_trick(*w, now);
         update_hold(*w, now);
      }
      if (in_formation(obj)) {
         // A wingman that flies paths is held off them, so it flies by physics.
         flyer_real_flight::release(obj);
         if (obj[kFly_Flags] & kFlagPaths) {
            float& t = *(float*)(obj + kFly_FreeFlyTime);
            if (!(t >= kHoldFreeFly)) t = kHoldFreeFly;
         }
      } else {
         flyer_real_flight::on_update(obj, dt);   // its paths flown for real, where it may
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   return s_update(ctrl, edx, dt);
}

// ---- Speeds and tricks --------------------------------------------------------------
// A wingman's speeds stretch round both how fast its leader flies and the
// speed it is heading for: braking or boosting, the wingman may go there too.
float high_speed() { return s_leaderTarget > s_leaderSpeed ? s_leaderTarget : s_leaderSpeed; }
float low_speed() { return s_leaderTarget < s_leaderSpeed ? s_leaderTarget : s_leaderSpeed; }

// The getters touch only EAX, ECX and the FPU stack, and their callers know it:
// EntityFlyer::Update keeps the flyer in EDX across GetFlyerMaxSpeed and loads
// ECX from it for the next getter (modtools 0x004FED55). The stand-ins keep
// every register, the XMM ones too, and hand the speed back in ST0 as the
// getters do.
float __cdecl max_speed_for(uint8_t* flyer)
{
   const float v = s_maxSpeed(flyer, nullptr);
   __try {
      if (in_formation(flyer)) return flyer_wingmen::stretched_max(v, high_speed());
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   return v;
}

float __cdecl min_speed_for(uint8_t* flyer)
{
   const float v = s_minSpeed(flyer, nullptr);
   __try {
      if (in_formation(flyer)) return flyer_wingmen::stretched_min(v, low_speed());
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   return v;
}

__declspec(naked) void max_speed_mt()
{
   __asm {
      push   ebp
      mov    ebp, esp
      and    esp, 0xFFFFFFF0
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      push   ebx
      push   esi
      push   edi
      push   ecx
      push   edx
      push   ecx                    // the flyer
      call   max_speed_for          // the speed in ST0
      add    esp, 4
      pop    edx
      pop    ecx
      pop    edi
      pop    esi
      pop    ebx
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      mov    esp, ebp
      pop    ebp
      ret
   }
}

__declspec(naked) void min_speed_mt()
{
   __asm {
      push   ebp
      mov    ebp, esp
      and    esp, 0xFFFFFFF0
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      push   ebx
      push   esi
      push   edi
      push   ecx
      push   edx
      push   ecx                    // the flyer
      call   min_speed_for          // the speed in ST0
      add    esp, 4
      pop    edx
      pop    ecx
      pop    edi
      pop    esi
      pop    ebx
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      mov    esp, ebp
      pop    ebp
      ret
   }
}

// Rolls and flips of their own throw a wingman out of formation and into its
// leader; it copies its leader's instead (copy_trick, which calls past this). True
// to let the trick through. The leader's own is noted for its wingmen to copy.
bool __cdecl trick_allowed(uint8_t* flyer, int trick)
{
   __try {
      note_leader_trick(flyer, trick);
      return !in_formation(flyer);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return true;
   }
}

// thiscall(Trick), RET 4. Every register comes back as it was, whether the
// trick is skipped or the original (or camera shake's detour before it) runs.
__declspec(naked) void do_trick_mt()
{
   __asm {
      push   ebp
      mov    ebp, esp
      and    esp, 0xFFFFFFF0
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      pushad
      push   dword ptr [ebp + 8]    // the trick, above the return address
      push   ecx                    // the flyer
      call   trick_allowed
      add    esp, 8
      test   al, al
      popad                         // leaves the flags as TEST set them
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      mov    esp, ebp
      pop    ebp
      jz     skip
      jmp    dword ptr [s_doTrick]  // the trick, with the caller's stack as it was
   skip:
      ret    4
   }
}

// ---- EntityFlyer::CalculateDest, at UpdateIndirect's call ---------------------------
struct Steer {
   Wingman* w = nullptr;
   uint8_t* dest = nullptr;          // AILowLevel::mDest
   uint8_t  saved[kDestSize] = {};   // the AI's own
   V3       point;                   // where it is steered
   float    target = 0.0f;           // the speed asked for, a share of its top speed
   float    leaderSpeed = 0.0f;
   Frame    frame;
   V3       slot;
};

bool steer_before(uint8_t* flyer, float dt, Steer& st)
{
   if (!in_formation(flyer)) return false;
   Wingman* const w = find(flyer);
   if (handle_of(flyer) != w->handle || *(uint8_t* const*)(flyer + kFly_Ctrl) != w->uc) return false;
   if (handle_of(s_leader) != s_leaderHandle || !alive(s_leader)) return false;
   const float* lm = matrix_of(s_leader);
   const float* wm = matrix_of(flyer);
   if (!lm || !wm) return false;
   const V3 leaderVel = velocity_of(s_leader);
   st.frame = flyer_wingmen::level_frame(row(lm, 2), row(lm, 0));
   if (!st.frame.valid || !flyer_wingmen::finite(leaderVel)) return false;
   st.leaderSpeed = flyer_wingmen::length(leaderVel);
   s_leaderSpeed = st.leaderSpeed;
   // Braking or boosting, the leader's throttle says so before its speed does.
   const float aim = *(const float*)(s_leader + kFly_TargetSpeed);
   s_leaderTarget = std::isfinite(aim) && aim > 0.0f ? aim : st.leaderSpeed;

   const V3 me = row(wm, 3), leaderPos = row(lm, 3);
   st.slot = flyer_wingmen::to_world(
      st.frame, leaderPos,
      flyer_wingmen::mirrored(flyer_wingmen::slot_for(keys_of(s_leader), w->slot, g_flyerWingmenSpacing), s_mirror));
   const V3 slot = flyer_wingmen::in_frame(st.frame, me, st.slot);   // the slot, from the wingman
   if (!w->holding) flyer_wingmen::update_trim(w->trim, slot, dt);
   const V3 steer = flyer_wingmen::keep_clear(
      flyer_wingmen::steer_offset(slot, flyer_wingmen::lead_distance(st.leaderSpeed), w->trim.climb),
      flyer_wingmen::in_frame(st.frame, me, leaderPos));
   st.point = flyer_wingmen::to_world(st.frame, me, steer);
   V3 heading = st.frame.fwd;

   // Its speeds are the stretched ones, as its getters answer for it (above),
   // and the target the one the game's throttle turns into that speed.
   const float hi = flyer_wingmen::stretched_max(s_maxSpeed(flyer, nullptr), high_speed());
   const float lo = flyer_wingmen::stretched_min(s_minSpeed(flyer, nullptr), low_speed());
   float speed = flyer_wingmen::formation_speed(s_leaderTarget + w->trim.speed, slot.z, lo, hi);

   // Through the leader's trick and its own copy: straight off its nose, at the
   // leader's speed.
   V3 nose = row(wm, 2);
   if (w->holding && flyer_wingmen::normalise(nose)) {
      st.point = me + nose * flyer_wingmen::kHoldAhead;
      heading = nose;
      speed = flyer_wingmen::formation_speed(s_leaderTarget, 0.0f, lo, hi);
   }
   st.target = flyer_wingmen::target_for_speed(speed, lo, s_midSpeed(flyer, nullptr), hi);

   uint8_t* const dest = *(uint8_t**)(flyer + kFly_Ctrl) + kUC_LowLevel + kLL_Dest;
   std::memcpy(st.saved, dest, kDestSize);
   st.dest = dest;   // from here on the AI's own goes back, whatever happens
   st.dest[kDest_Has] = 1;
   std::memcpy(st.dest + kDest_Pos, &st.point, 12);
   std::memcpy(st.dest + kDest_Original, &st.point, 12);
   std::memcpy(st.dest + kDest_Dir, &heading, 12);
   *(int*)(st.dest + kDest_Safe) = kSafeHeightFull;
   *(float*)(st.dest + kDest_Taper) = 0.0f;
   *(float*)(st.dest + kDest_Speed) = st.target;
   st.w = w;
   return true;
}

void steer_after(uint8_t* flyer, Steer& st, bool steered, float* speed)
{
   std::memcpy(st.dest, st.saved, kDestSize);   // the AI's own destination back
   const float* wm = matrix_of(flyer);
   if (!wm) return;
   const V3 pos = row(wm, 3), fwd = row(wm, 2);
   if (steered && speed && !(flyer[kFly_Flags] & kFlagSliding)) {
      V3 to = st.point - pos;
      const float cosOff = flyer_wingmen::normalise(to) ? flyer_wingmen::dot(to, fwd) : 1.0f;
      *speed = flyer_wingmen::keep_speed(*speed, st.target, cosOff);
   }
   st.w->stats.add(flyer_wingmen::in_frame(st.frame, pos, st.slot), flyer_wingmen::length(velocity_of(flyer)),
                   st.leaderSpeed);
}

// thiscall(dt, dest, point, speed, myPos) on the entity, RET 0x14, as __fastcall
// with EDX unused.
bool __fastcall calc_dest_mt(uint8_t* flyer, void*, float dt, void* dest, float* point, float* speed, float* myPos)
{
   Steer st;
   flyer_real_flight::Steer onPath;   // a flyer on real flight, at its carrot
   bool steering = false, chasing = false;
   __try {
      steering = steer_before(flyer, dt, st);
      if (!steering) chasing = flyer_real_flight::steer_before(flyer, onPath);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      steering = chasing = false;
      if (st.dest) std::memcpy(st.dest, st.saved, kDestSize);
      flyer_real_flight::restore(onPath);
   }
   const bool r = ((fn_calc_dest_t)s_calcDest)(flyer, dt, dest, point, speed, myPos);
   if (steering) {
      __try {
         steer_after(flyer, st, r, speed);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
   } else if (chasing) {
      __try {
         flyer_real_flight::steer_after(flyer, onPath, r, point, speed);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         flyer_real_flight::restore(onPath);
      }
   }
   return r;
}

// ---- Install --------------------------------------------------------------------------
bool starts_with(uintptr_t base, uintptr_t va, const char* bytes, size_t len)
{
   return std::memcmp(resolve(base, va), bytes, len) == 0;
}

uintptr_t call_destination(const uint8_t* call)
{
   const uint8_t* dest = call + 5 + *(const int32_t*)(call + 1);
   if (dest[0] == 0xE9) dest = dest + 5 + *(const int32_t*)(dest + 1);
   return (uintptr_t)dest;
}

} // namespace

void flyer_wingmen_install(uintptr_t base)
{
   if (g_build != GameBuild::Modtools) {
      if (g_flyerWingmen) install_log("[Wingmen] NOT installed: the prototype runs on modtools only");
      if (g_flyerRealFlight) install_log("[RealFlight] NOT installed: the prototype runs on modtools only");
      return;
   }
   const auto* a = g_addr;
   if (!a->flyer_update_indirect || !a->flyer_calc_dest_call || !a->flyer_calc_dest || !a->flyer_max_speed ||
       !a->flyer_min_speed || !a->flyer_mid_speed || !a->flyer_do_trick || !a->ai_controller_list || !a->net_game_get_local_player) {
      install_log("[Wingmen] NOT installed: no address set for this build");
      return;
   }
   // Prologues up to `this` landing in its register, and the getters' test.
   uint8_t* const at = (uint8_t*)resolve(base, a->flyer_calc_dest_call);
   const bool ok =
      starts_with(base, a->flyer_update_indirect, "\x81\xEC\x84\x00\x00\x00\x53\x55\x56\x8B\xF1", 11) &&
      starts_with(base, a->flyer_calc_dest, "\x83\xEC\x28\x53\x55\x56\x8B\xE9\x8B\x85\x08\x03\x00\x00", 14) &&
      starts_with(base, a->flyer_max_speed, "\xD9\x05\x6C\xA0\xA2\x00\xD9\x81\xFC\x05\x00\x00", 12) &&
      starts_with(base, a->flyer_min_speed, "\xD9\x05\x6C\xA0\xA2\x00\xD9\x81\xFC\x05\x00\x00", 12) &&
      starts_with(base, a->flyer_mid_speed, "\xD9\x05\x6C\xA0\xA2\x00\xD9\x81\xFC\x05\x00\x00", 12) &&
      // Camera shake may have detoured DoTrick already (it checks the same
      // prologue first); the detours chain.
      (starts_with(base, a->flyer_do_trick, "\xD9\x05\x6C\xA0\xA2\x00\x56\x8B\xF1\x8B\x8E\x6C\x06\x00\x00", 15) ||
       *(const uint8_t*)resolve(base, a->flyer_do_trick) == 0xE9) &&
      at[0] == 0xE8 && call_destination(at) == (uintptr_t)resolve(base, a->flyer_calc_dest);
   if (!ok) {
      install_log("[Wingmen] NOT installed: EntityFlyer's update or its calls differ");
      return;
   }
   LARGE_INTEGER f;
   QueryPerformanceFrequency(&f);
   s_ticksPerSecond = (double)f.QuadPart;
   g_flyerWingmenCount = flyer_wingmen::count_for(nullptr, g_flyerWingmenCount);   // 0 to 4
   g_flyerWingmenSpacing = flyer_wingmen::clamp_spacing(g_flyerWingmenSpacing);
   s_calcDest = (uintptr_t)resolve(base, a->flyer_calc_dest);
   s_local = (fn_local_t)resolve(base, a->net_game_get_local_player);
   s_list = (uint8_t**)resolve(base, a->ai_controller_list);

   // Installed whatever the INI says: a class's keys decide for it, and with
   // neither every call goes on to the game unchanged.
   s_odf = odf_add_property_handler(on_property) && odf_add_derive_handler(on_derive);
   if (!s_odf) install_log("[Wingmen] ODF keys unavailable: no listener slot left; the INI decides");

   s_update = (fn_update_t)resolve(base, a->flyer_update_indirect);
   s_maxSpeed = (fn_speed_t)resolve(base, a->flyer_max_speed);
   s_minSpeed = (fn_speed_t)resolve(base, a->flyer_min_speed);
   s_midSpeed = (fn_speed_t)resolve(base, a->flyer_mid_speed);
   s_doTrick = (fn_trick_t)resolve(base, a->flyer_do_trick);
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)s_update, hooked_update);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_maxSpeed, max_speed_mt);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_minSpeed, min_speed_mt);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_doTrick, do_trick_mt);
   if (r != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
      if (r != NO_ERROR) DetourTransactionAbort();
      install_log("[Wingmen] NOT installed: detouring EntityFlyer failed (%ld)", (long)r);
      return;
   }
   s_detoured = true;

   // The call, during the installers' writable window.
   s_callSite.at = at;
   std::memcpy(s_callSite.orig, at, 5);
   *(int32_t*)(at + 1) = (int32_t)((intptr_t)&calc_dest_mt - (intptr_t)(at + 5));

   install_log("[Wingmen] installed (prototype): %s; up to %d friendly AI flyers fly a vee on yours, %.0f m apart, "
               "%s", g_flyerWingmen ? "on for every flyer" : "off but for flyer classes with Wingman keys",
               g_flyerWingmenCount, (double)g_flyerWingmenSpacing,
               g_flyerWingmenAuto ? "joining at takeoff" : "joining on the squad key");

   // Real flight on paths rides on these hooks (flyer_real_flight.hpp).
   flyer_real_flight::Borrowed borrowed;
   borrowed.maxSpeed = s_maxSpeed;
   borrowed.minSpeed = s_minSpeed;
   borrowed.midSpeed = s_midSpeed;
   borrowed.wingman = &in_formation;
   flyer_real_flight::install(base, borrowed);
}

void flyer_wingmen_uninstall()
{
   flyer_real_flight::uninstall();
   if (s_callSite.at) protected_write(s_callSite.at, s_callSite.orig, 5);
   s_callSite = Site{};
   if (s_detoured) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(&(PVOID&)s_update, hooked_update);
      DetourDetach(&(PVOID&)s_maxSpeed, max_speed_mt);
      DetourDetach(&(PVOID&)s_minSpeed, min_speed_mt);
      DetourDetach(&(PVOID&)s_doTrick, do_trick_mt);
      DetourTransactionCommit();
      s_detoured = false;
   }
   s_classes.clear();
}
