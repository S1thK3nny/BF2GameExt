#include "pch.h"
#include "flyer_wingmen.hpp"
#include "flyer_avoidance.hpp"
#include "flyer_orders_core.hpp"
#include "flyer_real_flight.hpp"
#include "flyer_real_flight_core.hpp"
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
#include <cstdio>
#include <cstring>
#include <unordered_map>

// See flyer_wingmen.hpp for the behaviour and the reads.

bool  g_flyerWingmen        = false;
int   g_flyerWingmenCount   = flyer_wingmen::kDefaultWingmen;
float g_flyerWingmenSpacing = flyer_wingmen::kDefaultSpacing;
bool  g_flyerWingmenAuto    = true;
bool  g_flyerWingmenLand    = true;
bool  g_flyerRealFlightSquadrons = true;

namespace {

using flyer_orders::Order;
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
constexpr int      kStateLanded      = 0;       // EntityFlyer::State: landed ...
constexpr int      kStateTakeoff     = 1;       // ... taking off ...
constexpr int      kStateFlying      = 2;       // ... flying ...
constexpr int      kStateLanding     = 3;       // ... landing
constexpr int      kGroundMask       = 0x190;   // what the flyer's own landing ray hits (EntityFlyer::Update 0x004FEA8D)
constexpr float    kGroundRay        = 200.0f;  // m down from the leader for its ground
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

// Orders: the player's targets, and what an AI's attack is aimed at.
constexpr uint32_t kCtl_Reticule       = 0x164;  // Controllable::mReticuleTarget, a handle per weapon channel (8 bytes each)
constexpr uint32_t kCtl_ReticuleHandle = 0x168;
constexpr int      kReticuleChannels   = 2;
constexpr uint32_t kUC_Agent           = 0x2C0;  // UnitController::mAgent ...
constexpr uint32_t kAgent_AttackTarget = 0x10;   // ... its AttackHelper's target ...
constexpr uint32_t kAgent_AttackHandle = 0x14;   // ... and that one's handle
constexpr int      kCommAttack         = 6;      // the comm keys' codes (Code::AttackPosition ...
constexpr int      kCommDefend         = 7;      // ... and Code::DefendPosition)
constexpr int      kSafeHeightTaper    = 1;      // SH_TAPER: a dive at the ground lets the safe height go
// A flyer's primary weapon and the ranges its AI fights at (its weapon ODF's).
constexpr uint32_t kFly_Weapons        = 0x6B8;  // EntityFlyer::mWeapon[4] ...
constexpr int      kFlyWeapons         = 4;
constexpr uint32_t kFly_WeaponIndex    = 0x6C8;  // ... and mWeaponIndex[channel], channel 0 first
constexpr uint32_t kWeapon_Class       = 0x64;   // Weapon::mClass
constexpr uint32_t kWClass_MinRange    = 0x100;  // WeaponClass: MinRange ...
constexpr uint32_t kWClass_OptimalRange = 0x104; // ... OptimalRange ...
constexpr uint32_t kWClass_MaxRange    = 0x108;  // ... MaxRange
// The fly agent (UnitFlyAgent) that flies an AI flyer: its controller.
constexpr uint32_t kAgent_Ctrl         = 0x0C;   // UnitAgent::mCtrl, the UnitController

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

// AI squadrons: the game's own squadron rule (EntityFlyer::UpdatePathFollower,
// EntityPathFollower::CreateSquadronFromClosestEntities) and its shapes.
constexpr uint32_t kFly_Class           = 0x66C;    // its EntityFlyerClass ...
constexpr uint32_t kClass_PathClass     = 0x6AC;    // ... whose EntityPathFollowerClass begins with its EntitySquadronClass:
                                                    //     the SquadronFormation names (PblHash), up to 10 ...
constexpr uint32_t kSquad_FormationCount = 0x28;    // ... and how many
constexpr uint32_t kPathClass_VsFlyer   = 0x78;     // PathFollowerFlyerVsFlyer
constexpr uint32_t kClass_SquadronCos   = 0x1104;   // mSquadronMaxAngle, a cosine (0: within 90 degrees)
constexpr uint32_t kClass_SquadronDist2 = 0x1108;   // mSquadronSquaredDist (0: from the model's bounding radius)
constexpr uint32_t kVt_ClassModel       = 0x20;     // the class's GetModel ...
constexpr uint32_t kModel_Red           = 0x20;     // ... its RedModel ...
constexpr uint32_t kRed_Radius          = 0x94;     // ... and that one's bounding radius
constexpr uint32_t kPath_MaxSquad       = 0x84;     // EntityPath::mMaxSquadronSize (~0: no limit)
constexpr uint32_t kFly_SquadronTime    = 0x1CE8;   // mSquadronFormationTime, counted down to a try
constexpr uint32_t kForm_Name           = 0x14;     // EntityFormation: its name's PblHash (RedPath::GetName) ...
constexpr uint32_t kForm_Nodes          = 0x18;     // ... its nodes (0x1C bytes, the position first) ...
constexpr uint32_t kForm_NodeCount      = 0x1C;     // ... how many ...
constexpr uint32_t kForm_Root           = 0x28;     // ... the root's ...
constexpr uint32_t kForm_Flags          = 0x2C;     // ... bit 0: the root is a slot
constexpr uint32_t kNodeSize            = 0x1C;
constexpr uint32_t kNode_Rotation       = 0x0C;     // a node's rotation: x, y, z, s
constexpr uint32_t kList_Next           = 0x04;     // the formations' list: a node's next ...
constexpr uint32_t kList_Payload        = 0x0C;     // ... and its formation (the head's null)
constexpr int      kMaxMapShapes        = 64;

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
constexpr uint32_t kRttiSoldier        = pbl_hash("EntitySoldier");
constexpr uint32_t kRttiUnitController = pbl_hash("UnitController");
// The localisation keys hash as the game's own squad command keys do (its
// EntitySoldier::Update looks this one up as 0x731FD493).
static_assert(pbl_hash("game.squadcommand.follow") == 0x731FD493u, "localisation keys are PblHashes");
constexpr uint32_t kHashClass = pbl_hash("WingmanClass");
constexpr uint32_t kHashCount = pbl_hash("WingmanCount");
constexpr uint32_t kHashAuto  = pbl_hash("WingmanAuto");
constexpr uint32_t kHashSlot  = pbl_hash("WingmanSlot");
constexpr uint32_t kHashFormation = pbl_hash("WingmanFormation");
constexpr uint32_t kHashExitOnLand = pbl_hash("WingmanExitOnLand");

constexpr double kTickSeconds   = 1.0;
constexpr double kReportSeconds = 5.0;
constexpr double kKeyRepeat     = 0.3;   // s: one press, seen by every flyer's update that frame
constexpr double kForceEvery    = 0.5;   // s between settings of a chasing wingman's attack target
constexpr int    kMaxReports    = 400;
constexpr int    kMaxSquadLogs  = 300;
constexpr int    kMaxOrderLogs  = 400;
constexpr int    kMaxCandidates = 32;
constexpr int    kMaxFoes       = 64;

using fn_is_rtti_t   = bool(__thiscall*)(void* self, uint32_t hash);
using fn_get_obj_t   = uint8_t*(__thiscall*)(void* self);
using fn_get_vec_t   = const float*(__thiscall*)(void* self);
using fn_local_t     = uint8_t*(__cdecl*)(uint32_t localIndex);
using fn_update_t    = bool(__fastcall*)(void* ctrl, void* edx, float dt);
using fn_speed_t     = float(__fastcall*)(uint8_t* flyer, void* edx);
using fn_trick_t     = void(__fastcall*)(uint8_t* flyer, void* edx, int trick);
using fn_calc_dest_t = bool(__thiscall*)(void* flyer, float dt, void* dest, float* point, float* speed,
                                         float* myPos);
using fn_find_shape_t = const uint8_t*(__cdecl*)(uint32_t name);
using fn_locked_t     = uint8_t*(__thiscall*)(void* controllable);
using fn_force_t      = void(__thiscall*)(void* uc, void* target);
using fn_affiliation_t = int(__thiscall*)(void* me, void* other);
using fn_message_t    = void(__cdecl*)(int player, const wchar_t* text, const uint8_t* color);
using fn_localize_t   = const wchar_t*(__cdecl*)(uint32_t hash, bool nullIfMissing);
using fn_add_chat_t   = uint32_t(__cdecl*)(uint32_t teamOnly, uint32_t localIndex, uint32_t lo, uint32_t hi, int code,
                                           const wchar_t* text, uint32_t mask);
using fn_ray_hit_t    = float(__cdecl*)(const float* start, const float* dir, float length, void** hit, float* normal,
                                        uint8_t** ignore, int ignoreCount, int typeMask, int ordnanceOnly);
using fn_flyer_t      = void(__thiscall*)(void* flyer);
using fn_land_hint_t  = bool(__thiscall*)(void* agent);
using fn_exit_t       = void(__thiscall*)(void* lowLevel, void* commandPost);

// Detoured: after the install each holds the original's trampoline.
fn_update_t s_update   = nullptr;
fn_speed_t  s_maxSpeed = nullptr;
fn_speed_t  s_minSpeed = nullptr;
fn_speed_t  s_midSpeed = nullptr;   // called, not detoured
fn_trick_t  s_doTrick  = nullptr;
uintptr_t   s_calcDest = 0;
fn_local_t  s_local    = nullptr;
uint8_t**   s_list     = nullptr;   // the global holding sAICtrls
fn_find_shape_t s_findShape = nullptr;   // EntityFormation::Find
uint8_t*    s_shapeList = nullptr;  // the map's formation shapes: the list's head node
bool        s_squadrons = false;    // the squadron rule's reads checked
bool        s_detoured = false;
bool        s_odf      = false;
double      s_ticksPerSecond = 0.0;

// Orders (null until their reads are checked).
fn_locked_t      s_lockedTarget = nullptr;   // Controllable::GetTargetLockedObj
fn_force_t       s_forceTarget  = nullptr;   // UnitController::ForceSetAttackHelperTarget
fn_affiliation_t s_affiliation  = nullptr;   // GameObject::GetAffiliationToMe
fn_message_t     s_message      = nullptr;   // the HUD's message line
const uint8_t*   s_messageColor = nullptr;   // the trooper squad commands' colour
fn_localize_t    s_localize     = nullptr;
fn_add_chat_t    s_addChat      = nullptr;   // NetGame::AddChatMessageToQue
int              s_pendingComm  = -1;        // a comm key pressed, for the next update ...
double           s_pendingAt    = 0.0;       // ... when
// Landing with the player (null until their reads are checked).
fn_ray_hit_t     s_rayHit  = nullptr;        // CollisionManager::RayHit
fn_flyer_t       s_land    = nullptr;        // EntityFlyer::Land
fn_flyer_t       s_takeOff = nullptr;        // EntityFlyer::TakeOff
fn_land_hint_t   s_landHint = nullptr;       // UnitFlyAgent::IsNearLandingPt (waiting aboard)
fn_exit_t        s_exitVehicle = nullptr;    // AILowLevel::SetExitVehicle

struct Site {
   uint8_t* at = nullptr;
   uint8_t  orig[5] = {};
};
Site s_callSite;
Site s_chatSite;
Site s_landHintSite;     // the fly agent's CALL to IsNearLandingPt ...
Site s_takeOffSites[2];  // ... and its two CALLs to EntityFlyer::TakeOff

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
   if (hash == kHashExitOnLand) {
      if (cls) flyer_wingmen::set_exit_on_land(s_classes[cls], value);
      return true;
   }
   if (hash == kHashSlot) {
      if (cls)
         flyer_wingmen::add_slot(s_classes[cls], value, [](const char* b, unsigned n) { return pbl_hash(b, n); });
      return true;
   }
   if (hash == kHashFormation) {
      if (cls) {
         ClassKeys& k = s_classes[cls];
         bool any = false;
         flyer_wingmen::for_each_name(value, [&](const char* b, unsigned len) {
            flyer_wingmen::add_formation(k, pbl_hash(b, len));
            any = true;
         });
         if (!any) flyer_wingmen::add_formation(k, 0);   // an empty list: no map shape, the vee
      }
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

// ---- Formations -------------------------------------------------------------------
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
   // The player's wingmen, under orders:
   uint8_t* target = nullptr;  // what it chases, or null for its slot
   uint32_t targetHandle = 0;
   bool     ground = false;    // the target is not in the air: passes at it
   flyer_orders::Run run;
   flyer_orders::RunShape shape;   // its passes and its gap behind a flyer, from its own weapon
   double   chasedFrom = 0.0;
   double   forcedAt = -1e9;   // when its AI's attack target was last set to its own
   bool     joining = false;   // flying in from afar (called up, or back from a chase)
   double   joinBy = 0.0;      // ... and let go if not in by then
   double   joinFrom = 0.0;
   // Landing with the player:
   int      land = 0;          // one of the kLand values below
   V3       spot;              // where it lands
};

// A wingman of the player's while the player lands and takes off.
enum : int {
   kLandNone    = 0,
   kLandComing  = 1,   // coming down beside its leader
   kLandCircle  = 2,   // circling overhead: no level ground beside its leader
   kLandDown    = 3,   // landing or landed
   kLandLeaving = 4,   // taking off with its leader
   kLandOut     = 5,   // landed, its pilot getting out (WingmanExitOnLand)
};

// The leader's DoTrick, seen before it runs; it went ahead if the leader is in a
// trick by the next update.
struct LeaderTrick {
   int    requested = -1;
   float  side = 0.0f;
   double at = 0.0;
};

// A leader and its wingmen: the player's (always the first), or an AI squadron.
struct Formation {
   uint8_t*    leader = nullptr;
   uint32_t    leaderHandle = 0;
   uint8_t*    leaderUc = nullptr;    // an AI squadron's: the UnitController flying its leader
   bool        ai = false;
   Wingman     wing[flyer_wingmen::kMaxWingmen];
   int         count = 0;
   Order       order = Order::FormUp; // the player's orders (an AI squadron's always form up) ...
   Order       resume = Order::FormUp;   // ... what an attack goes back to ...
   uint8_t*    attackTarget = nullptr;   // ... and an attack's target
   uint32_t    attackHandle = 0;
   bool        called = false;        // the player called wingmen: losses are made up even without WingmanAuto
   bool        landing = false;       // the player's flyer is landing or landed: its wingmen land too ...
   V3          pad;                   // ... beside where it came down ...
   Frame       padFrame;              // ... facing as it was ...
   float       padGround = 0.0f;      // ... the ground's height there
   bool        mirror = false;        // left for right, after an odd number of the leader's flips
   float       leaderSpeed = 0.0f;    // as last seen by a wingman's steering: how fast it flies ...
   float       leaderTarget = 0.0f;   // ... and the speed its throttle asks for
   LeaderTrick trick;
   V3          shape[flyer_wingmen::kMaxWingmen];   // an AI squadron's slots, from the map's formation shape
   int         shapeCount = 0;        // none: the leader class's WingmanSlot lines, else the vee
   uint32_t    shapeName = 0;
};

constexpr int kMaxFormations = 33;   // the player's, and up to 32 AI squadrons
Formation  s_forms[kMaxFormations];
Formation& s_player = s_forms[0];

double   s_nextTick = 0.0;
double   s_nextReport = 0.0;
double   s_nextSquadReport = 0.0;
double   s_lastPress = -1e9;
int      s_reports = 0;
int      s_trickLogs = 0;
constexpr int kMaxTrickLogs = 50;
int      s_squadLogs = 0;
int      s_orderLogs = 0;
int      s_formed = 0;      // AI squadrons formed since the last report ...
int      s_brokeUp = 0;     // ... and broken up
uint32_t s_random = 0x2545F491u;

double seconds()
{
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return (double)t.QuadPart / s_ticksPerSecond;
}

double random01()
{
   s_random = s_random * 1664525u + 1013904223u;
   return (double)(s_random >> 8) * (1.0 / 16777216.0);
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
int state_of(const uint8_t* flyer) { return *(const int*)(flyer + kFly_State); }
bool flying(const uint8_t* flyer) { return state_of(flyer) == kStateFlying; }
// Landed, taking off or landing.
bool on_or_near_ground(const uint8_t* flyer)
{
   const int s = state_of(flyer);
   return s == kStateLanded || s == kStateTakeoff || s == kStateLanding;
}
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

// The flyer the host's own player flies, while it flies (or, with its wingmen
// landing with it, while it lands, sits landed and takes off again).
uint8_t* player_flyer()
{
   uint8_t* const chr = s_local ? s_local(0) : nullptr;
   uint8_t* const vehicle = chr ? *(uint8_t**)(chr + layout::Character::kVehicle) : nullptr;
   if (!vehicle) return nullptr;
   uint8_t* const obj = object_of(vehicle);
   if (!obj || !is_rtti(obj, kRttiFlyer) || !alive(obj)) return nullptr;
   if (flying(obj) || (g_flyerWingmenLand && s_land && on_or_near_ground(obj))) return obj;
   return nullptr;
}

// Everything the AI controls, alive (a soldier, or what it drives, flies or
// mans), with its UnitController.
template <class F>
void for_each_ai_unit(F f)
{
   uint8_t* const list = s_list ? *s_list : nullptr;
   if (!list) return;
   uint8_t* node = *(uint8_t**)(list + kNodeNext);
   for (int i = 0; i < kMaxNodes && node; ++i) {
      uint8_t* const uc = *(uint8_t**)(node + kNodePayload);
      if (!uc) break;   // the list's end
      node = *(uint8_t**)(node + kNodeNext);
      if (!is_rtti(uc, kRttiUnitController)) continue;
      uint8_t* const owner = *(uint8_t**)(uc + kCtrl_Owner);
      if (!owner) continue;
      uint8_t* const obj = object_of(owner);
      if (!obj || !alive(obj)) continue;
      f(obj, uc);
   }
}

// Every AI flyer flying, flown by its own UnitController, with that controller.
template <class F>
void for_each_ai_flyer(F f)
{
   for_each_ai_unit([&](uint8_t* obj, uint8_t* uc) {
      if (!is_rtti(obj, kRttiFlyer) || !flying(obj)) return;
      if (*(uint8_t* const*)(obj + kFly_Ctrl) != uc) return;
      f(obj, uc);
   });
}

// The best few of a search, the highest key first.
struct Candidate {
   uint8_t* obj;
   uint8_t* uc;
   uint32_t cls;
   float    key;     // the higher, the sooner taken
   float    dist;
};

int add_candidate(Candidate* pool, int found, const Candidate& c)
{
   if (found == kMaxCandidates) {
      if (!(c.key > pool[kMaxCandidates - 1].key)) return found;
      pool[kMaxCandidates - 1] = c;
   } else {
      pool[found++] = c;
   }
   for (int k = found - 1; k > 0 && pool[k].key > pool[k - 1].key; --k) {
      const Candidate t = pool[k];
      pool[k] = pool[k - 1];
      pool[k - 1] = t;
   }
   return found;
}

// Whether this leader has wingmen at all: its class's keys decide where it has
// any, else the INI.
bool leads(const uint8_t* leader)
{
   const ClassKeys* k = keys_of(leader);
   return (flyer_wingmen::sets_any(k) || g_flyerWingmen) && flyer_wingmen::count_for(k, g_flyerWingmenCount) > 0;
}

Wingman* find(const uint8_t* obj, Formation** in = nullptr)
{
   for (Formation& f : s_forms) {
      if (!f.leader) continue;
      for (int i = 0; i < f.count; ++i)
         if (f.wing[i].obj == obj) {
            if (in) *in = &f;
            return &f.wing[i];
         }
   }
   return nullptr;
}

Formation* led_by(const uint8_t* obj)
{
   for (Formation& f : s_forms)
      if (f.leader && f.leader == obj) return &f;
   return nullptr;
}

// A wingman, the player's or an AI squadron's, in its slot or chasing: steered,
// and held off its paths.
bool held(const uint8_t* obj) { return find(obj) != nullptr; }

// A wingman flying its slot (not chasing): held to its leader's speeds, and
// copying its leader's tricks instead of pulling its own.
bool in_slot(const uint8_t* obj)
{
   const Wingman* w = find(obj);
   return w && !w->target;
}

const char* tag(const Formation& f) { return f.ai ? "[Squadrons]" : "[Wingmen]"; }

bool squad_log()
{
   if (s_squadLogs >= kMaxSquadLogs) return false;
   ++s_squadLogs;
   return true;
}

bool order_log()
{
   if (s_orderLogs >= kMaxOrderLogs) return false;
   ++s_orderLogs;
   return true;
}

void drop(Formation& f, int i, const char* why)
{
   if (!f.ai || squad_log())
      install_log("%s slot %d (#%u) let go: %s", tag(f), f.wing[i].slot + 1, f.wing[i].handle, why);
   f.wing[i] = f.wing[--f.count];
   f.wing[f.count] = Wingman{};
}

void release_all(Formation& f, const char* why)
{
   if (f.count && (!f.ai || squad_log())) install_log("%s %d let go: %s", tag(f), f.count, why);
   f.count = 0;
   for (auto& w : f.wing) w = Wingman{};
   f.mirror = false;   // a new formation starts the right way round
   f.trick = LeaderTrick{};
}

// An AI squadron's end. Its wingmen need nothing put back: no longer in
// formation, each goes back to real flight at its next update.
void dissolve(Formation& f, const char* why)
{
   if (squad_log()) install_log("[Squadrons] #%u's squadron of %d broke up: %s", f.leaderHandle, f.count, why);
   f = Formation{};
   ++s_brokeUp;
}

// Still a wingman? Why not, or null. One chasing (its chase has its own leash,
// assign_targets) or flying in from afar (with its own deadline) is not let go
// for its distance.
const char* lost(const Wingman& w, const Formation& f, double now)
{
   if (handle_of(w.obj) != w.handle || !alive(w.obj)) return "gone";
   if (*(uint8_t* const*)(w.obj + kFly_Ctrl) != w.uc) return "flown by someone else";
   // Landing with its leader, landed, or taking off with it, it is still one.
   const bool landing = f.landing || w.land != kLandNone;
   if (!flying(w.obj) && !(landing && on_or_near_ground(w.obj))) return "not flying";
   if (team_of(w.obj) != team_of(f.leader)) return "changed sides";
   if (w.target || landing) return nullptr;
   if (w.joining) return now > w.joinBy ? "did not reach its slot in time" : nullptr;
   const float* a = matrix_of(w.obj);
   const float* b = matrix_of(f.leader);
   if (!a || !b) return "no position";
   if (flyer_wingmen::length(row(a, 3) - row(b, 3)) > flyer_wingmen::kKeepRange) return "too far";
   return nullptr;
}

bool slot_taken(const Formation& f, int slot)
{
   for (int i = 0; i < f.count; ++i)
      if (f.wing[i].slot == slot) return true;
   return false;
}

// Slot `slot` of a formation, from its leader, mirrored after its leader's flips:
// its map shape's where it flies one, else its leader class's WingmanSlot lines,
// else the vee.
V3 slot_of(const Formation& f, int slot)
{
   const V3 s = slot >= 0 && slot < f.shapeCount ? f.shape[slot]
                                                 : flyer_wingmen::slot_for(keys_of(f.leader), slot, g_flyerWingmenSpacing);
   return flyer_wingmen::mirrored(s, f.mirror);
}

Wingman& add_wingman(Formation& f, uint8_t* obj, uint8_t* uc, int slot)
{
   Wingman& w = f.wing[f.count++];
   w = Wingman{};
   w.obj = obj;
   w.handle = handle_of(obj);
   w.uc = uc;
   w.slot = slot;
   return w;
}

// The player calls up an AI squadron's wingman: it leaves its squadron first.
void leave_squadron(const uint8_t* obj)
{
   Formation* f = nullptr;
   if (!find(obj, &f) || !f->ai) return;
   for (int i = 0; i < f->count; ++i)
      if (f->wing[i].obj == obj) {
         drop(*f, i, "called up by the player");
         break;
      }
   if (!f->count) dissolve(*f, "its last wingman was called up by the player");
}

// ---- The map's formation shapes ------------------------------------------------------
// The map's EntityFormation paths, flown as their authors drew them, whoever leads
// (flyer_wingmen_core.hpp, "Which way a map's formation shape faces"): their points
// with the game's z flip taken back out, and, on a map whose formations were placed
// turned (the BF3 maps'), turned back by their own rotation.
const char* shape_name(uint32_t name)
{
   static const char* const kStock[] = {"Tri-form",  "Line-Form", "Box-Form",    "five-form",  "eschleft-form",
                                        "eschrgt-form", "wall-form", "escort-form", "bomber-Form"};
   for (const char* n : kStock)
      if (pbl_hash(n) == name) return n;
   return nullptr;
}

struct Shape {
   uint32_t name = 0;
   V3       points[flyer_wingmen::kMaxShapePoints];   // as the game holds them: from the root, z negated
   int      count = 0;
   int      root = 0;
   bool     rootIsSlot = true;
   float    yaw = 0.0f;   // the root node's turn, in the editor's frame
   int      size = 0;     // flyers it holds, its leader among them
};

bool read_shape_at(const uint8_t* f, Shape& s)
{
   const uint8_t* const nodes = *(const uint8_t* const*)(f + kForm_Nodes);
   const int count = *(const int*)(f + kForm_NodeCount);
   const int root = *(const int*)(f + kForm_Root);
   if (!nodes || count < 2 || count > flyer_wingmen::kMaxShapePoints || root < 0 || root >= count) return false;
   for (int i = 0; i < count; ++i) std::memcpy(&s.points[i], nodes + i * kNodeSize, sizeof(V3));
   const float* q = (const float*)(nodes + root * kNodeSize + kNode_Rotation);
   s.name = *(const uint32_t*)(f + kForm_Name);
   s.count = count;
   s.root = root;
   s.rootIsSlot = (f[kForm_Flags] & 1) != 0;
   s.yaw = flyer_wingmen::drawn_yaw(q[0], q[1], q[2], q[3]);
   s.size = flyer_wingmen::shape_size(count, s.rootIsSlot);
   return true;
}

bool read_shape(uint32_t name, Shape& s)
{
   const uint8_t* const f = s_findShape && name ? s_findShape(name) : nullptr;
   return f && read_shape_at(f, s);
}

// Whether this map's formations are turned back by their rotation: when more of
// them are then led from the front (flyer_wingmen::unturn_shapes). Read afresh
// each time (a map has a dozen at most); logged when the answer's inputs change.
struct MapShapes {
   bool unturn = false;
   int  count = 0, asDrawn = 0, unturned = 0;
};
MapShapes s_lastMapShapes;
bool      s_mapShapesLogged = false;

MapShapes map_shapes()
{
   static Shape shapes[kMaxMapShapes];
   flyer_wingmen::ShapeView views[kMaxMapShapes];
   MapShapes m;
   if (!s_shapeList) return m;
   uint8_t* node = *(uint8_t**)(s_shapeList + kList_Next);
   for (int i = 0; i < 1024 && node && node != s_shapeList && m.count < kMaxMapShapes; ++i) {
      const uint8_t* const f = *(const uint8_t* const*)(node + kList_Payload);
      if (!f) break;   // the head: the list's end
      node = *(uint8_t**)(node + kList_Next);
      Shape& s = shapes[m.count];
      if (!read_shape_at(f, s)) continue;
      views[m.count] = {s.points, s.count, s.root, s.rootIsSlot, s.yaw};
      ++m.count;
   }
   m.unturn = flyer_wingmen::unturn_shapes(views, m.count, &m.asDrawn, &m.unturned);
   if (!s_mapShapesLogged || m.count != s_lastMapShapes.count || m.unturn != s_lastMapShapes.unturn ||
       m.asDrawn != s_lastMapShapes.asDrawn) {
      s_mapShapesLogged = true;
      install_log("[Wingmen] the map's %d formation shapes fly %s: %d led from the front as drawn, %d turned back by "
                  "their rotation",
                  m.count, m.unturn ? "turned back by their rotation" : "as drawn", m.asDrawn, m.unturned);
   }
   s_lastMapShapes = m;
   return m;
}

// The first `members` slots of a map shape, as drawn.
int shape_slots_for(const Shape& s, bool unturn, int members, V3* out)
{
   return flyer_wingmen::drawn_slots(s.points, s.count, s.root, s.rootIsSlot, s.yaw, unturn, members, out);
}

// ---- The player's wingmen ---------------------------------------------------------
// A player's flight flies its class's WingmanSlot lines where it has them; else
// one of the map's formation shapes: the first its WingmanFormation names that the
// map has, else the first of its SquadronFormation shapes that holds all its
// wingmen (else the largest); else the vee. Chosen when it starts leading.
void choose_player_shape(uint8_t* leader)
{
   Formation& f = s_player;
   f.shapeCount = 0;
   f.shapeName = 0;
   const ClassKeys* keys = keys_of(leader);
   if ((keys && keys->hasSlots) || !s_findShape) return;
   const int want = flyer_wingmen::count_for(keys, g_flyerWingmenCount);
   if (want <= 0) return;
   uint32_t names[flyer_wingmen::kMaxClassFormations];
   int named = 0;
   bool firstFound = false;   // WingmanFormation: the first the map has, whatever its size
   if (keys && keys->hasFormations) {
      for (int i = 0; i < keys->formationCount; ++i) names[named++] = keys->formations[i];
      firstFound = true;
   } else if (const uint8_t* cls = *(const uint8_t* const*)(leader + kFly_Class)) {
      const uint8_t* const pathClass = cls + kClass_PathClass;
      const uint32_t n = *(const uint32_t*)(pathClass + kSquad_FormationCount);
      for (uint32_t i = 0; i < n && named < flyer_wingmen::kMaxClassFormations; ++i)
         names[named++] = ((const uint32_t*)pathClass)[i];
   }
   Shape pick, s;
   bool have = false;
   for (int i = 0; i < named; ++i) {
      if (!read_shape(names[i], s)) continue;
      if (firstFound) {
         pick = s;
         have = true;
         break;
      }
      if (!have || (pick.size - 1 < want && s.size > pick.size)) {
         pick = s;
         have = true;
      }
      if (pick.size - 1 >= want) break;
   }
   if (!have) return;
   const MapShapes m = map_shapes();
   const int members = pick.size - 1 < want ? pick.size - 1 : want;
   f.shapeCount = shape_slots_for(pick, m.unturn, members, f.shape);
   f.shapeName = pick.name;
   const char* const nm = shape_name(pick.name);
   if (nm)
      install_log("[Wingmen] your flight flies the map's %s, %d slots%s", nm, f.shapeCount,
                  m.unturn ? ", turned back by its rotation" : "");
   else
      install_log("[Wingmen] your flight flies the map's formation 0x%08X, %d slots%s", pick.name, f.shapeCount,
                  m.unturn ? ", turned back by its rotation" : "");
}

// The leader's free slots, in order, each to the nearest friendly AI flyer its
// class lets follow and the slot lets in (a WingmanSlot line may keep it for some
// classes), however far off: one further than kJoinedRange flies in, and has
// join_seconds to reach its slot. An AI squadron's wingmen may be called away
// from it; its leaders not.
void fill(uint8_t* leader, double now)
{
   Formation& f = s_player;
   const ClassKeys* keys = keys_of(leader);
   int total = flyer_wingmen::count_for(keys, g_flyerWingmenCount);
   if (f.shapeCount > 0 && total > f.shapeCount) total = f.shapeCount;   // a map shape holds no more
   if (f.count >= total) return;
   const float* lm = matrix_of(leader);
   if (!lm) return;
   const V3 lp = row(lm, 3);
   const float leaderMax = s_maxSpeed(leader, nullptr);
   const int team = team_of(leader);

   // Every friendly AI flyer that may follow, the nearest first.
   Candidate pool[kMaxCandidates];
   int found = 0;
   for_each_ai_flyer([&](uint8_t* obj, uint8_t* uc) {
      if (obj == leader || team_of(obj) != team) return;
      Formation* in = nullptr;
      if (find(obj, &in) && in == &f) return;
      if (const Formation* led = led_by(obj); led && led->count) return;   // an AI squadron's leader
      const float* m = matrix_of(obj);
      if (!m) return;
      const float d = flyer_wingmen::length(row(m, 3) - lp);
      if (!std::isfinite(d)) return;
      const uint32_t cls = class_id(obj);
      if (!flyer_wingmen::accepts(keys, cls, flyer_wingmen::can_keep_up(s_maxSpeed(obj, nullptr), leaderMax))) return;
      found = add_candidate(pool, found, {obj, uc, cls, -d, d});
   });
   bool used[kMaxCandidates] = {};
   for (int slot = 0; slot < total && f.count < flyer_wingmen::kMaxWingmen; ++slot) {
      if (slot_taken(f, slot)) continue;
      for (int c = 0; c < found; ++c) {
         if (used[c] || !flyer_wingmen::slot_accepts(keys, slot, pool[c].cls)) continue;
         used[c] = true;
         leave_squadron(pool[c].obj);
         Wingman& w = add_wingman(f, pool[c].obj, pool[c].uc, slot);
         w.joining = pool[c].dist > flyer_orders::kJoinedRange;
         w.joinFrom = now;
         w.joinBy = now + flyer_orders::join_seconds(pool[c].dist);
         if (w.joining)
            install_log("[Wingmen] slot %d: #%u called up %.0f m off, %.0f s to reach its slot; top speed %.0f m/s "
                        "against the leader's %.0f",
                        slot + 1, w.handle, (double)pool[c].dist, w.joinBy - now, (double)s_maxSpeed(w.obj, nullptr),
                        (double)leaderMax);
         else
            install_log("[Wingmen] slot %d: #%u joined, %.0f m off, top speed %.0f m/s against the leader's %.0f",
                        slot + 1, w.handle, (double)pool[c].dist, (double)s_maxSpeed(w.obj, nullptr), (double)leaderMax);
         break;
      }
   }
}

// ---- The player's orders ---------------------------------------------------------------
// What the player sees: a line on the HUD's message line, in the trooper squad
// commands' colour, as theirs show. A map or mod may word it its own way under
// the key (its localisation); else the English here.
struct Line {
   uint32_t       key;    // PblHash of the localisation key
   const wchar_t* text;
};
constexpr Line kSayFormUp     = {pbl_hash("game.wingmen.formup"), L"Wingmen: form up"};
constexpr Line kSayEngage     = {pbl_hash("game.wingmen.engage"), L"Wingmen: engage"};
constexpr Line kSayAttack     = {pbl_hash("game.wingmen.attack"), L"Wingmen: attack my target"};
constexpr Line kSayCover      = {pbl_hash("game.wingmen.cover"), L"Wingmen: cover me"};
constexpr Line kSayNoTarget   = {pbl_hash("game.wingmen.notarget"), L"Wingmen: no target"};
constexpr Line kSayNone       = {pbl_hash("game.wingmen.none"), L"Wingmen: no one to call up"};
constexpr Line kSayTargetDown = {pbl_hash("game.wingmen.targetdown"), L"Wingmen: target down"};
constexpr Line kSayTargetLost = {pbl_hash("game.wingmen.targetlost"), L"Wingmen: target lost"};

void say(const Line& line)
{
   if (!s_message) return;
   const wchar_t* text = line.text;
   if (s_localize)
      if (const wchar_t* own = s_localize(line.key, true); own && *own) text = own;
   uint8_t color[4] = {0xFF, 0xFF, 0xFF, 0xFF};
   if (s_messageColor) std::memcpy(color, s_messageColor, sizeof color);
   s_message(0, text, color);
}

const char* order_name(Order o)
{
   switch (o) {
   case Order::FormUp: return "form up";
   case Order::Engage: return "engage";
   case Order::Attack: return "attack my target";
   case Order::Cover: return "cover me";
   }
   return "?";
}

V3 position_of(uint8_t* obj)
{
   const float* m = matrix_of(obj);
   return m ? row(m, 3) : V3{};
}

bool valid_target(const uint8_t* t, uint32_t handle) { return t && handle_of(t) == handle && alive(t); }

// In the air: a flyer taking off, flying or landing. Anything else is passed at.
bool airborne(uint8_t* t)
{
   if (!is_rtti(t, kRttiFlyer)) return false;
   const int state = *(const int*)(t + kFly_State);
   return state >= 1 && state <= 3;
}

bool enemy(uint8_t* leader, uint8_t* other)
{
   if (s_affiliation) return s_affiliation(leader, other) < 0;
   return team_of(other) != team_of(leader);
}

// What an AI's attack is aimed at: its agent's AttackHelper target, or null.
uint8_t* attack_target_of(uint8_t* uc)
{
   uint8_t* const agent = *(uint8_t**)(uc + kUC_Agent);
   if (!agent) return nullptr;
   uint8_t* const t = *(uint8_t**)(agent + kAgent_AttackTarget);
   if (!t || handle_of(t) != *(const uint32_t*)(agent + kAgent_AttackHandle)) return nullptr;
   return t;
}

// The player's target as the order is given: its lock, else an enemy under its
// reticule; null for none.
uint8_t* player_target(uint8_t* leader, const char** how)
{
   uint8_t* const ctl = leader + kFly_Controllable;
   if (s_lockedTarget)
      if (uint8_t* t = s_lockedTarget(ctl); t && alive(t) && enemy(leader, t)) {
         *how = "locked";
         return t;
      }
   for (int k = 0; k < kReticuleChannels; ++k) {
      uint8_t* const t = *(uint8_t**)(ctl + kCtl_Reticule + k * 8);
      if (!t || handle_of(t) != *(const uint32_t*)(ctl + kCtl_ReticuleHandle + k * 8)) continue;
      if (alive(t) && enemy(leader, t)) {
         *how = "under the reticule";
         return t;
      }
   }
   return nullptr;
}

// Back to its slot. From further than its leader keeps wingmen it flies in
// again, as a recruit does.
void clear_target(Wingman& w, uint8_t* leader, double now, const char* why)
{
   if (!w.target) return;
   if (why && order_log())
      install_log("[Wingmen] slot %d (#%u) back to its slot after %.0f s: %s", w.slot + 1, w.handle, now - w.chasedFrom,
                  why);
   w.target = nullptr;
   w.targetHandle = 0;
   w.ground = false;
   w.run = flyer_orders::Run{};
   const float d = flyer_wingmen::length(position_of(w.obj) - position_of(leader));
   if (!(d <= flyer_wingmen::kKeepRange)) {
      w.joining = true;
      w.joinFrom = now;
      w.joinBy = now + flyer_orders::join_seconds(d);
   }
}

// A flyer's primary weapon's ranges, as its weapon ODF gives them to the AI
// (MinRange, OptimalRange, MaxRange): channel 0's weapon (GetWeaponIndex(0),
// GetWeaponPtr), its class. False for none.
bool primary_ranges(const uint8_t* flyer, float& minRange, float& optimal, float& maxRange)
{
   const int index = *(const int*)(flyer + kFly_WeaponIndex);
   if (index < 0 || index >= kFlyWeapons) return false;
   const uint8_t* const weapon = *(const uint8_t* const*)(flyer + kFly_Weapons + index * 4);
   const uint8_t* const cls = weapon ? *(const uint8_t* const*)(weapon + kWeapon_Class) : nullptr;
   if (!cls) return false;
   minRange = *(const float*)(cls + kWClass_MinRange);
   optimal = *(const float*)(cls + kWClass_OptimalRange);
   maxRange = *(const float*)(cls + kWClass_MaxRange);
   return true;
}

void chase(Wingman& w, uint8_t* t, double now)
{
   if (w.target == t && w.targetHandle == handle_of(t)) return;
   w.target = t;
   w.targetHandle = handle_of(t);
   w.ground = !airborne(t);
   w.run = flyer_orders::Run{false, now};
   w.chasedFrom = now;
   w.forcedAt = -1e9;
   // Its passes and its gap, from its own weapon at its cruising speed.
   float minRange = 0.0f, optimal = 0.0f, maxRange = 0.0f;
   const bool armed = primary_ranges(w.obj, minRange, optimal, maxRange);
   w.shape = flyer_orders::run_shape(minRange, optimal, maxRange, s_midSpeed(w.obj, nullptr));
   if (order_log())
      install_log("[Wingmen] slot %d (#%u) takes on #%u (%s), %.0f m off; %s %.0f/%.0f/%.0f m: pulls out at %.0f m, "
                  "back in from %.0f m, %.0f m behind a flyer",
                  w.slot + 1, w.handle, w.targetHandle, w.ground ? "not in the air: passes at it" : "in the air",
                  (double)flyer_wingmen::length(position_of(t) - position_of(w.obj)),
                  armed ? "its weapon's ranges" : "no weapon read, ranges", (double)minRange, (double)optimal,
                  (double)maxRange, (double)w.shape.breakAt, (double)w.shape.extendTo, (double)w.shape.gap);
}

// Who each wingman takes on, by the order (flyer_orders_core.hpp): once a
// second, and as an order is given.
void assign_targets(Formation& f, uint8_t* leader, double now)
{
   const V3 lp = position_of(leader);
   if (f.order == Order::FormUp) {
      for (int i = 0; i < f.count; ++i) clear_target(f.wing[i], leader, now, "formed up");
      return;
   }
   if (f.order == Order::Attack) {
      uint8_t* const t = f.attackTarget;
      const bool down = !valid_target(t, f.attackHandle);
      if (!down && flyer_wingmen::length(position_of(t) - lp) <= flyer_orders::kAttackKeep) {
         for (int i = 0; i < f.count; ++i) chase(f.wing[i], t, now);
         return;
      }
      say(down ? kSayTargetDown : kSayTargetLost);
      if (order_log())
         install_log("[Wingmen] the attack's target is %s: back to %s", down ? "down" : "out of reach",
                     order_name(f.resume == Order::Attack ? Order::FormUp : f.resume));
      f.attackTarget = nullptr;
      f.attackHandle = 0;
      f.order = f.resume == Order::Attack ? Order::FormUp : f.resume;
      for (int i = 0; i < f.count; ++i) clear_target(f.wing[i], leader, now, nullptr);
      if (f.order == Order::FormUp) return;
   }
   // Engage or cover: the foes, then each wingman's.
   const bool cover = f.order == Order::Cover;
   const float range = cover ? flyer_orders::kCoverRange : flyer_orders::kEngageRange;
   const float keep = cover ? flyer_orders::kCoverKeep : flyer_orders::kEngageKeep;
   flyer_orders::Foe foes[kMaxFoes];
   uint8_t* foeObj[kMaxFoes];
   int n = 0;
   for_each_ai_unit([&](uint8_t* obj, uint8_t* uc) {
      if (n >= kMaxFoes || obj == leader) return;
      for (int k = 0; k < n; ++k)
         if (foeObj[k] == obj) return;   // a second seat of one already counted
      if (!enemy(leader, obj)) return;
      const V3 p = position_of(obj);
      if (!(flyer_wingmen::length(p - lp) <= range)) return;
      const bool air = airborne(obj);
      if (cover) {
         if (attack_target_of(uc) != leader) return;   // whoever has the leader as its target
      } else if (!air && is_rtti(obj, kRttiSoldier)) {
         return;   // troops on foot are left to the troops
      }
      foes[n] = {p, !air, 0};
      foeObj[n] = obj;
      ++n;
   });
   // A wingman keeps its foe while it is still one, within reach of the leader.
   for (int i = 0; i < f.count; ++i) {
      Wingman& w = f.wing[i];
      if (!w.target) continue;
      int k = 0;
      while (k < n && foeObj[k] != w.target) ++k;
      if (k < n) {
         ++foes[k].taken;
         continue;
      }
      const char* why = !valid_target(w.target, w.targetHandle) ? "its target is down"
                        : !(flyer_wingmen::length(position_of(w.target) - lp) <= keep) ? "its target got away"
                                                                                       : nullptr;
      if (why) clear_target(w, leader, now, why);
   }
   // The rest, in slot order, each to its cheapest foe; one still flying in waits.
   for (int slot = 0; slot < flyer_wingmen::kMaxWingmen; ++slot)
      for (int i = 0; i < f.count; ++i) {
         Wingman& w = f.wing[i];
         if (w.slot != slot || w.target || w.joining) continue;
         const int k = flyer_orders::pick_foe(position_of(w.obj), foes, n);
         if (k < 0) break;
         chase(w, foeObj[k], now);
         ++foes[k].taken;
      }
}

void set_order(Formation& f, Order o, double now)
{
   if (o != Order::Attack) {
      f.attackTarget = nullptr;
      f.attackHandle = 0;
   }
   f.order = o;
   if (order_log()) install_log("[Wingmen] order: %s, %d wingmen", order_name(o), f.count);
   if (f.leader) assign_targets(f, f.leader, now);
}

// A chasing wingman's AI aims and fires at its target: its AttackHelper is
// set to it (as UnitFlyAgent::IsNearATAT forces a walker on its crew), no
// more than every kForceEvery, and only while it aims at something else.
void force_target(Wingman& w, double now)
{
   if (!s_forceTarget || !w.target || now - w.forcedAt < kForceEvery) return;
   if (!valid_target(w.target, w.targetHandle) || attack_target_of(w.uc) == w.target) return;
   w.forcedAt = now;
   s_forceTarget(w.uc, w.target);
}

// Each wingman flying in counts as joined once near its slot; one still chasing
// whose leader is out of reach comes back.
void update_wingmen(Formation& f, uint8_t* leader, double now)
{
   const float* lm = matrix_of(leader);
   if (!lm) return;
   const Frame frame = flyer_wingmen::level_frame(row(lm, 2), row(lm, 0));
   for (int i = 0; i < f.count; ++i) {
      Wingman& w = f.wing[i];
      const V3 me = position_of(w.obj);
      if (w.target && !(flyer_wingmen::length(me - row(lm, 3)) <= flyer_orders::kChaseLeash))
         clear_target(w, leader, now, "too far from its leader");
      if (!w.joining || !frame.valid) continue;
      const V3 slot = flyer_wingmen::to_world(frame, row(lm, 3), slot_of(f, w.slot));
      if (flyer_wingmen::length(me - slot) <= flyer_orders::kJoinedRange) {
         w.joining = false;
         if (order_log())
            install_log("[Wingmen] slot %d (#%u) in its slot after %.0f s", w.slot + 1, w.handle, now - w.joinFrom);
      }
   }
}

void report(double now)
{
   if (now < s_nextReport || s_reports >= kMaxReports) return;
   s_nextReport = now + kReportSeconds;
   if (s_player.count) {
      int joining = 0, chasing = 0;
      for (int i = 0; i < s_player.count; ++i) {
         joining += s_player.wing[i].joining && !s_player.wing[i].target;
         chasing += s_player.wing[i].target != nullptr;
      }
      ++s_reports;
      install_log("[Wingmen] %s: %d wingmen, %d in their slots, %d flying in, %d chasing", order_name(s_player.order),
                  s_player.count, s_player.count - joining - chasing, joining, chasing);
   }
   for (int i = 0; i < s_player.count; ++i) {
      Wingman& w = s_player.wing[i];
      Stats& s = w.stats;
      if (!s.samples) continue;
      const double n = s.samples;
      ++s_reports;
      install_log("[Wingmen] slot %d: off its slot by %.1f m on average, %.1f m at worst (slot %.1f m ahead, %.1f "
                  "right, %.1f up); %.0f m/s against the leader's %.0f; trim %.1f m up, %.1f m/s",
                  w.slot + 1, s.distance / n, (double)s.worst, s.ahead / n, s.right / n, s.up / n, s.speed / n,
                  s.leaderSpeed / n, (double)w.trim.climb, (double)w.trim.speed);
      s = Stats{};
   }
}

// ---- AI squadrons -----------------------------------------------------------------
// A flyer on real flight never joins the game's squadrons (they ride splines). Its
// leaders gather squadrons of their own by the game's rule, flown as the
// player's wingmen are: every 5.5 to 8.5 s a leader not in one takes the nearest
// same-side flyers on real flight heading its way, within its class's reach, up
// to its path's MaxSquadSize, in one of its class's SquadronFormation shapes
// (the map's EntityFormation paths, as drawn), or its WingmanSlot lines.
float model_radius(uint8_t* cls)
{
   uint8_t* const model = ((fn_get_obj_t)(*(void***)cls)[kVt_ClassModel / 4])(cls);
   const uint8_t* const red = model ? *(const uint8_t* const*)(model + kModel_Red) : nullptr;
   return red ? *(const float*)(red + kRed_Radius) : 0.0f;
}

Formation* free_formation()
{
   for (int k = 1; k < kMaxFormations; ++k)
      if (!s_forms[k].leader) return &s_forms[k];
   return nullptr;
}

void try_squadron(uint8_t* leader, const uint8_t* path)
{
   uint8_t* const cls = *(uint8_t**)(leader + kFly_Class);
   if (!cls) return;
   const uint8_t* const pathClass = cls + kClass_PathClass;
   if (!pathClass[kPathClass_VsFlyer]) return;   // the game gathers FlyerVsFlyer classes only
   const ClassKeys* keys = keys_of(leader);
   Shape shapes[flyer_wingmen::kMaxClassFormations];
   int nShapes = 0;
   const uint32_t named = *(const uint32_t*)(pathClass + kSquad_FormationCount);
   for (uint32_t k = 0; k < named && k < (uint32_t)flyer_wingmen::kMaxClassFormations; ++k)
      if (read_shape(((const uint32_t*)pathClass)[k], shapes[nShapes])) ++nShapes;
   const bool ownSlots = keys && keys->hasSlots && keys->slotCount > 0;
   if (!nShapes && !ownSlots) return;   // no shape to fly: none, as the game
   Formation* const f = free_formation();
   if (!f) return;

   const float* lm = matrix_of(leader);
   if (!lm) return;
   const V3 lp = row(lm, 3);
   V3 lf = row(lm, 2);
   if (!flyer_wingmen::normalise(lf)) return;
   const float reach2 = flyer_wingmen::squadron_reach2(*(const float*)(cls + kClass_SquadronDist2), model_radius(cls));
   const float cosMax = *(const float*)(cls + kClass_SquadronCos);
   const float leaderMax = s_maxSpeed(leader, nullptr);
   const int team = team_of(leader);

   Candidate pool[kMaxCandidates];
   int found = 0;
   for_each_ai_flyer([&](uint8_t* obj, uint8_t* uc) {
      if (obj == leader || team_of(obj) != team || find(obj) || led_by(obj) || (obj[kFly_Flags] & kFlagTrick)) return;
      if (!flyer_real_flight::squadron_path(obj)) return;   // on real flight, on a path, not chasing
      const float* m = matrix_of(obj);
      if (!m) return;
      V3 fwd = row(m, 2);
      if (!flyer_wingmen::normalise(fwd)) return;
      const V3 d = row(m, 3) - lp;
      float score = 0.0f;
      if (!flyer_wingmen::squadron_candidate(flyer_wingmen::dot(d, d), reach2, flyer_wingmen::dot(fwd, lf), cosMax, score))
         return;
      const uint32_t id = class_id(obj);
      if (!flyer_wingmen::accepts(keys, id, flyer_wingmen::can_keep_up(s_maxSpeed(obj, nullptr), leaderMax))) return;
      found = add_candidate(pool, found, {obj, uc, id, score, flyer_wingmen::length(d)});
   });
   if (!found) return;

   // How many: up to the path's MaxSquadSize, as many as one of its shapes holds.
   const uint32_t maxSquad = *(const uint32_t*)(path + kPath_MaxSquad);
   int n = found < flyer_wingmen::kMaxWingmen ? found : flyer_wingmen::kMaxWingmen;
   if (maxSquad < (uint32_t)n) n = (int)maxSquad;
   const Shape* shape = nullptr;
   int members = 0;
   if (nShapes) {
      members = flyer_wingmen::squadron_members(n, [&](int size) {
         for (int k = 0; k < nShapes; ++k)
            if (shapes[k].size == size) return true;
         return false;
      });
      if (!members) return;
      int fits = 0;
      for (int k = 0; k < nShapes; ++k) fits += shapes[k].size == members + 1;
      int pick = (int)(random01() * fits);
      for (int k = 0; k < nShapes && !shape; ++k)
         if (shapes[k].size == members + 1 && pick-- == 0) shape = &shapes[k];
      if (!shape) return;
   } else {
      const int own = flyer_wingmen::count_for(keys, g_flyerWingmenCount);
      members = n < own ? n : own;
      if (members <= 0) return;
   }

   *f = Formation{};
   f->ai = true;
   f->leader = leader;
   f->leaderHandle = handle_of(leader);
   f->leaderUc = *(uint8_t**)(leader + kFly_Ctrl);
   if (shape) {
      f->shapeCount = shape_slots_for(*shape, map_shapes().unturn, members, f->shape);
      f->shapeName = shape->name;
      if (f->shapeCount < members) members = f->shapeCount;
   }
   // Its slots in order, each to the best-placed candidate the slot takes.
   bool used[kMaxCandidates] = {};
   for (int slot = 0; slot < members; ++slot)
      for (int c = 0; c < found; ++c) {
         if (used[c] || (!shape && !flyer_wingmen::slot_accepts(keys, slot, pool[c].cls))) continue;
         used[c] = true;
         add_wingman(*f, pool[c].obj, pool[c].uc, slot);
         break;
      }
   if (!f->count) {
      *f = Formation{};
      return;
   }
   ++s_formed;
   if (squad_log()) {
      char who[160] = {};
      int at = 0;
      for (int i = 0; i < f->count && at < (int)sizeof who - 24; ++i)
         for (int c = 0; c < found; ++c)
            if (pool[c].obj == f->wing[i].obj) {
               at += sprintf_s(who + at, sizeof who - at, "%s#%u %.0f m", i ? ", " : "", f->wing[i].handle,
                               (double)pool[c].dist);
               break;
            }
      const char* const named_ = shape ? shape_name(shape->name) : nullptr;
      if (shape && named_)
         install_log("[Squadrons] #%u leads %d in %s: %s", f->leaderHandle, f->count, named_, who);
      else if (shape)
         install_log("[Squadrons] #%u leads %d in formation 0x%08X: %s", f->leaderHandle, f->count, shape->name, who);
      else
         install_log("[Squadrons] #%u leads %d in its WingmanSlot lines: %s", f->leaderHandle, f->count, who);
   }
}

// A free AI flyer on real flight counts its squadron time down, as the game
// does a placed one's (+0x1CE8), and tries when it runs out.
void maybe_squadron(uint8_t* obj, float dt)
{
   if (!s_squadrons || !g_flyerRealFlightSquadrons || find(obj) || led_by(obj)) return;
   const uint8_t* const path = flyer_real_flight::squadron_path(obj);
   if (!path) return;
   float& timer = *(float*)(obj + kFly_SquadronTime);
   if (!std::isfinite(timer)) timer = 0.0f;
   if (std::isfinite(dt) && dt > 0.0f) timer -= dt;
   if (timer > 0.0f) return;
   timer = (float)flyer_wingmen::squadron_wait(random01());
   try_squadron(obj, path);
}

const char* leader_lost(const Formation& f)
{
   uint8_t* const l = f.leader;
   if (handle_of(l) != f.leaderHandle || !alive(l)) return "its leader is gone";
   if (*(uint8_t* const*)(l + kFly_Ctrl) != f.leaderUc) return "its leader is flown by someone else";
   if (!flying(l)) return "its leader landed";
   if (find(l)) return "its leader flies as a wingman";
   if (!flyer_real_flight::on_real_flight(l)) return "its leader left real flight";
   return nullptr;
}

void report_squadrons(double now)
{
   if (now < s_nextSquadReport || s_reports >= kMaxReports) return;
   s_nextSquadReport = now + kReportSeconds;
   int squads = 0, wingmen = 0, samples = 0;
   double distance = 0.0;
   float worst = 0.0f;
   for (int k = 1; k < kMaxFormations; ++k) {
      Formation& f = s_forms[k];
      if (!f.leader) continue;
      ++squads;
      wingmen += f.count;
      for (int i = 0; i < f.count; ++i) {
         Stats& s = f.wing[i].stats;
         samples += s.samples;
         distance += s.distance;
         if (s.worst > worst) worst = s.worst;
         s = Stats{};
      }
   }
   if (squads || s_formed || s_brokeUp) {
      ++s_reports;
      install_log("[Squadrons] %d AI squadrons flying, %d wingmen in them, off their slots by %.1f m on average, %.1f "
                  "m at worst; %d formed and %d broke up in %.0f s",
                  squads, wingmen, samples ? distance / samples : 0.0, (double)worst, s_formed, s_brokeUp,
                  kReportSeconds);
   }
   s_formed = 0;
   s_brokeUp = 0;
}

void tick_squadrons(double now)
{
   for (int k = 1; k < kMaxFormations; ++k) {
      Formation& f = s_forms[k];
      if (!f.leader) continue;
      if (const char* why = leader_lost(f)) {
         dissolve(f, why);
         continue;
      }
      for (int i = f.count - 1; i >= 0; --i)
         if (const char* why = lost(f.wing[i], f, now)) drop(f, i, why);
      if (!f.count) dissolve(f, "no wingmen left");
   }
   report_squadrons(now);
}

// ---- Landing with the player (FlyerWingmenLand) ----------------------------------------
// The ground under a point, straight down, as the flyer's own landing ray finds
// it: its height and how far up its surface faces; false for none in reach.
bool ground_at(V3 from, float length, uint8_t* ignoreA, uint8_t* ignoreB, float& y, float& up)
{
   if (!s_rayHit) return false;
   const float start[3] = {from.x, from.y, from.z};
   const float down[3] = {0.0f, -1.0f, 0.0f};
   void* hit = nullptr;
   float normal[3] = {0.0f, 1.0f, 0.0f};
   uint8_t* ignore[2] = {ignoreA, ignoreB};
   const float fraction = s_rayHit(start, down, length, &hit, normal, ignore, ignoreB ? 2 : 1, kGroundMask, 0);
   if (!(fraction >= 0.0f && fraction < 1.0f)) return false;
   y = from.y - fraction * length;
   up = normal[1];
   return true;
}

// The leader comes down: each wingman gets a spot beside it on level ground,
// else the sky over it (flyer_orders_core.hpp).
void start_landing(Formation& f, uint8_t* leader, double now)
{
   f.landing = true;
   const float* lm = matrix_of(leader);
   if (!lm) return;
   f.pad = row(lm, 3);
   f.padFrame = flyer_wingmen::level_frame(row(lm, 2), row(lm, 0));
   float y = f.pad.y, up = 1.0f;
   f.padGround = ground_at(f.pad + V3{0.0f, 5.0f, 0.0f}, kGroundRay, leader, nullptr, y, up) ? y : f.pad.y;
   int beside = 0, over = 0;
   for (int i = 0; i < f.count; ++i) {
      Wingman& w = f.wing[i];
      clear_target(w, leader, now, nullptr);
      w.joining = false;
      w.holding = false;
      w.trick = -1;
      const V3 offset = flyer_orders::land_offset(slot_of(f, w.slot));
      V3 spot = f.padFrame.valid ? flyer_wingmen::to_world(f.padFrame, f.pad, offset) : f.pad + offset;
      float hy = 0.0f, hup = 0.0f;
      const bool level =
         ground_at(V3{spot.x, f.padGround + flyer_orders::kLandProbe, spot.z}, 2.0f * flyer_orders::kLandProbe, leader,
                   w.obj, hy, hup) &&
         flyer_orders::level_spot(hy, hup, f.padGround);
      spot.y = level ? hy : f.padGround;
      w.spot = spot;
      w.land = level ? kLandComing : kLandCircle;
      ++(level ? beside : over);
   }
   if (order_log())
      install_log("[Wingmen] the leader comes down: %d land beside it, %d circle overhead (no level ground beside it)",
                  beside, over);
}

// The leader takes off again (or did not land after all): so do they.
void end_landing(Formation& f)
{
   f.landing = false;
   int leaving = 0;
   for (int i = 0; i < f.count; ++i) {
      Wingman& w = f.wing[i];
      const int s = state_of(w.obj);
      if (s == kStateLanded || s == kStateLanding) {
         s_takeOff(w.obj);
         w.land = kLandLeaving;
         ++leaving;
      } else {
         w.land = kLandNone;
      }
   }
   if (order_log()) install_log("[Wingmen] the leader takes off: %d of its wingmen take off with it", leaving);
}

// Once a second: the leader's state, and each wingman's.
void update_landing(Formation& f, uint8_t* leader)
{
   const int ls = state_of(leader);
   const bool down = ls == kStateLanding || ls == kStateLanded;
   if (down && !f.landing) start_landing(f, leader, seconds());
   else if (!down && f.landing) end_landing(f);
   for (int i = 0; i < f.count; ++i) {
      Wingman& w = f.wing[i];
      if (w.land == kLandLeaving && flying(w.obj)) w.land = kLandNone;   // up again: to its slot
      // Up again before its leader (it should wait aboard): it circles.
      if (w.land == kLandDown && flying(w.obj) && f.landing) w.land = kLandCircle;
      // Down, one whose class says so gets out (AILowLevel::SetExitVehicle, with
      // no command post, as the AI's own end-of-game exit asks); the rest wait.
      if (w.land == kLandDown && state_of(w.obj) == kStateLanded && s_exitVehicle &&
          flyer_wingmen::exits_on_land(keys_of(w.obj))) {
         s_exitVehicle(w.uc + kUC_LowLevel, nullptr);
         w.land = kLandOut;
         if (order_log()) install_log("[Wingmen] slot %d (#%u) landed: its pilot gets out (WingmanExitOnLand)",
                                      w.slot + 1, w.handle);
      }
   }
}

// Every update: one coming down near enough to its spot lands, as a node's
// LandOnArrival lands a flyer.
void maybe_land(Wingman& w)
{
   if (w.land != kLandComing || !flying(w.obj) || !flyer_orders::may_land(position_of(w.obj), w.spot)) return;
   s_land(w.obj);
   w.land = kLandDown;
   if (order_log())
      install_log("[Wingmen] slot %d (#%u) lands %.0f m from its spot", w.slot + 1, w.handle,
                  (double)flyer_orders::across(position_of(w.obj), w.spot));
}

// The player's flight, back to its first state: forming up, no attack, called by
// no one, not landing.
void reset_orders(Formation& f)
{
   f.order = Order::FormUp;
   f.resume = Order::FormUp;
   f.attackTarget = nullptr;
   f.attackHandle = 0;
   f.called = false;
   f.landing = false;
}

void tick(double now)
{
   uint8_t* const leader = player_flyer();
   if (!leader || !leads(leader)) {
      release_all(s_player, leader ? "the player's flyer leads none" : "the player is not flying");
      s_player.leader = nullptr;
      s_player.leaderHandle = 0;
      s_player.shapeCount = 0;
      s_player.shapeName = 0;
      reset_orders(s_player);
   } else {
      if (leader != s_player.leader || handle_of(leader) != s_player.leaderHandle) {
         release_all(s_player, "a new leader");
         s_player.leader = leader;
         s_player.leaderHandle = handle_of(leader);
         reset_orders(s_player);
         choose_player_shape(leader);
      }
      if (g_flyerWingmenLand && s_land) update_landing(s_player, leader);
      for (int i = s_player.count - 1; i >= 0; --i)
         if (const char* why = lost(s_player.wing[i], s_player, now)) drop(s_player, i, why);
      if (flying(leader) && !s_player.landing) {
         update_wingmen(s_player, leader, now);
         assign_targets(s_player, leader, now);
         // Losses are made up whenever they happen, whatever the order: once the
         // player has called wingmen, or from takeoff with WingmanAuto.
         const ClassKeys* keys = keys_of(leader);
         const int want = flyer_wingmen::count_for(keys, g_flyerWingmenCount);
         if ((s_player.called || flyer_wingmen::auto_for(keys, g_flyerWingmenAuto)) && s_player.count < want) {
            const int before = s_player.count;
            fill(leader, now);
            if (s_player.count > before && s_player.order != Order::FormUp) assign_targets(s_player, leader, now);
         }
      }
      report(now);
   }
   tick_squadrons(now);
}

// The player leads a flight, in the air.
bool player_leads_now()
{
   return s_player.leader && handle_of(s_player.leader) == s_player.leaderHandle && alive(s_player.leader) &&
          flying(s_player.leader);
}

// The squad key, in the air: calls wingmen up when it leads none, else
// switches between forming up and engaging (from an attack or cover, forming
// up).
void squad_key(double now)
{
   if (!player_leads_now()) return;
   const uint32_t trigger = *(const uint32_t*)(s_player.leader + kFly_Controllable + kCtl_Squad);
   if ((trigger & kSquadPressed) != kSquadPressed || now - s_lastPress < kKeyRepeat) return;
   s_lastPress = now;
   if (!s_player.count) {
      s_player.called = true;
      fill(s_player.leader, now);
      set_order(s_player, Order::FormUp, now);
      say(s_player.count ? kSayFormUp : kSayNone);
      install_log("[Wingmen] squad key: %s", s_player.count ? "called up, form up" : "no one to call up");
      return;
   }
   set_order(s_player, flyer_orders::after_squad_key(s_player.order), now);
   say(s_player.order == Order::FormUp ? kSayFormUp : kSayEngage);
   install_log("[Wingmen] squad key: %s", order_name(s_player.order));
}

// The comm keys' attack and defend, in the air with wingmen: attack my target
// (the player's lock, else an enemy under its reticule, as the key is pressed),
// and cover me. Their team chat goes out as ever.
void comm_key(double now)
{
   const int code = s_pendingComm;
   s_pendingComm = -1;
   if (code != kCommAttack && code != kCommDefend) return;
   if (now - s_pendingAt > kKeyRepeat) return;   // pressed before any flyer was there to hear it
   if (!player_leads_now()) return;
   if (!s_player.count) {
      say(kSayNone);
      install_log("[Wingmen] comm key %d: no wingmen to order", code);
      return;
   }
   if (code == kCommDefend) {
      set_order(s_player, Order::Cover, now);
      say(kSayCover);
      install_log("[Wingmen] comm key: cover me");
      return;
   }
   const char* how = "";
   uint8_t* const t = player_target(s_player.leader, &how);
   if (!t) {
      say(kSayNoTarget);
      install_log("[Wingmen] comm key: attack my target, but nothing is locked or under the reticule");
      return;
   }
   s_player.resume = flyer_orders::resume_after_attack(s_player.order, s_player.resume);
   s_player.attackTarget = t;
   s_player.attackHandle = handle_of(t);
   say(kSayAttack);
   install_log("[Wingmen] comm key: attack my target, #%u (%s, %s), %.0f m off", handle_of(t), how,
               airborne(t) ? "in the air" : "not in the air",
               (double)flyer_wingmen::length(position_of(t) - position_of(s_player.leader)));
   set_order(s_player, Order::Attack, now);
}

// ---- The leader's tricks, copied down the formation ----------------------------------
const char* trick_name(int trick, float side)
{
   static const char* const kNames[kTrickCount] = {"flip up", "flip down", "flip up right", "flip down right",
                                                    "flip up left", "flip down left", "side roll"};
   if (trick == kTrickSideRoll) return side < 0.0f ? "side roll left" : "side roll right";
   return trick >= 0 && trick < kTrickCount ? kNames[trick] : "trick";
}

// From DoTrick's stand-in, before a leader's trick runs: what was asked, and
// which way a side roll goes (the game sets mSideRoll.x from the strafe key
// first). A leader already in a trick is refused, so that call is passed over.
void note_leader_trick(const uint8_t* flyer, int trick)
{
   Formation* const f = led_by(flyer);
   if (!f || !f->count || (flyer[kFly_Flags] & kFlagTrick)) return;
   if (trick < 0 || trick >= kTrickCount) return;
   f->trick.requested = trick;
   f->trick.side = *(const float*)(flyer + kFly_SideRoll);
   f->trick.at = seconds();
}

// Once the leader is in its trick, each wingman in its slot gets its turn (one
// chasing or still flying in has its own business).
void ripple_leader_trick(Formation& f, double now)
{
   if (f.trick.requested < 0) return;
   if (!f.leader || handle_of(f.leader) != f.leaderHandle) {
      f.trick = LeaderTrick{};
      return;
   }
   if (!(f.leader[kFly_Flags] & kFlagTrick)) {
      if (now - f.trick.at > flyer_wingmen::kTrickWait) f.trick = LeaderTrick{};   // refused
      return;
   }
   // The trick it did: its class may have turned a flip into a side roll.
   const int done = *(const int*)(f.leader + kFly_LastTrick);
   const int trick = done >= 0 && done < kTrickCount ? done : f.trick.requested;
   int slots[flyer_wingmen::kMaxWingmen];
   int copying = 0;
   for (int i = 0; i < f.count; ++i)
      if (!f.wing[i].target && !f.wing[i].joining) slots[copying++] = f.wing[i].slot;
   for (int i = 0; i < f.count; ++i) {
      Wingman& w = f.wing[i];
      if (w.target || w.joining) continue;
      w.trick = trick;
      w.trickSide = f.trick.side;
      w.trickAt = now + flyer_wingmen::ripple_delay(flyer_wingmen::ripple_rank(w.slot, slots, copying));
      w.holding = true;   // straight on until its copy is done
   }
   // A flip turns the leader round: its wingmen take the slots on the side they
   // come out on.
   if (flyer_wingmen::is_flip(trick)) f.mirror = !f.mirror;
   if (s_trickLogs < kMaxTrickLogs) {
      ++s_trickLogs;
      install_log("%s #%u's %s: %d wingmen copy it, %.2f s apart%s", tag(f), f.leaderHandle,
                  trick_name(trick, f.trick.side), copying, flyer_wingmen::kTrickRipple,
                  flyer_wingmen::is_flip(trick) ? (f.mirror ? "; the formation now mirrored"
                                                            : "; the formation back the right way round")
                                                : "");
   }
   f.trick = LeaderTrick{};
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

// A wingman's turn: the same trick the same way, free of energy (not once it
// has gone after a target meanwhile).
void copy_trick(Wingman& w, double now)
{
   const int trick = w.trick;
   w.trick = -1;
   w.holdUntil = now + flyer_wingmen::kTrickSettle;   // if it does not trick, it settles from here
   uint8_t* const fl = w.obj;
   if (handle_of(fl) != w.handle || !alive(fl) || !flying(fl) || w.target || (fl[kFly_Flags] & kFlagTrick)) return;
   if (trick == kTrickSideRoll) {
      float* const side = (float*)(fl + kFly_SideRoll);
      side[0] = w.trickSide < 0.0f ? -1.0f : 1.0f;
      side[1] = 0.0f;
      side[2] = 0.0f;
   }
   // Whatever DoTrick spends goes back, and a spent-out bar does not refuse it.
   float& energy = *(float*)(fl + kFly_EnergyValue);
   uint32_t& energyFlags = *(uint32_t*)(fl + kFly_EnergyFlags);
   const float keptEnergy = energy;
   const uint32_t keptFlags = energyFlags;
   energyFlags = keptFlags & ~1u;
   s_doTrick(fl, nullptr, trick);   // the trampoline, past the stand-in that keeps wingmen from tricks
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
      comm_key(now);
      uint8_t* const obj = (uint8_t*)ctrl - kFly_Controllable;
      for (Formation& f : s_forms)
         if (f.leader && f.trick.requested >= 0) ripple_leader_trick(f, now);
      Formation* f = nullptr;
      if (Wingman* w = find(obj, &f)) {
         if (w->trick >= 0 && now >= w->trickAt) copy_trick(*w, now);
         update_hold(*w, now);
         // Turned on by an attacker: an AI squadron's leader takes it on, the
         // whole squadron with it; the player's wingmen go by their orders.
         uint8_t* const leader = f->ai && handle_of(f->leader) == f->leaderHandle && alive(f->leader) ? f->leader
                                                                                                      : nullptr;
         flyer_real_flight::pass_attacker(obj, leader);
         if (w->target) force_target(*w, now);
         if (w->land == kLandComing && s_land) maybe_land(*w);
      }
      if (held(obj)) {
         // A wingman that flies paths is held off them, so it flies by physics.
         flyer_real_flight::release(obj);
         if (obj[kFly_Flags] & kFlagPaths) {
            float& t = *(float*)(obj + kFly_FreeFlyTime);
            if (!(t >= kHoldFreeFly)) t = kHoldFreeFly;
         }
      } else {
         flyer_real_flight::on_update(obj, dt);   // its paths flown for real, where it may
         maybe_squadron(obj, dt);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   return s_update(ctrl, edx, dt);
}

// ---- Speeds and tricks --------------------------------------------------------------
// A wingman's speeds stretch round both how fast its leader flies and the
// speed it is heading for: braking or boosting, the wingman may go there too.
float high_speed(const Formation& f) { return f.leaderTarget > f.leaderSpeed ? f.leaderTarget : f.leaderSpeed; }
float low_speed(const Formation& f) { return f.leaderTarget < f.leaderSpeed ? f.leaderTarget : f.leaderSpeed; }

// The getters touch only EAX, ECX and the FPU stack, and their callers know it:
// EntityFlyer::Update keeps the flyer in EDX across GetFlyerMaxSpeed and loads
// ECX from it for the next getter (modtools 0x004FED55). The stand-ins keep
// every register, the XMM ones too, and hand the speed back in ST0 as the
// getters do.
float __cdecl max_speed_for(uint8_t* flyer)
{
   const float v = s_maxSpeed(flyer, nullptr);
   __try {
      Formation* f = nullptr;
      if (const Wingman* w = find(flyer, &f); w && !w->target) return flyer_wingmen::stretched_max(v, high_speed(*f));
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   return v;
}

float __cdecl min_speed_for(uint8_t* flyer)
{
   const float v = s_minSpeed(flyer, nullptr);
   __try {
      Formation* f = nullptr;
      if (const Wingman* w = find(flyer, &f); w && !w->target) return flyer_wingmen::stretched_min(v, low_speed(*f));
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
// to let the trick through. A leader's own is noted for its wingmen to copy.
bool __cdecl trick_allowed(uint8_t* flyer, int trick)
{
   __try {
      note_leader_trick(flyer, trick);
      return !in_slot(flyer);
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
   Formation* f = nullptr;
   Wingman*   w = nullptr;
   uint8_t*   dest = nullptr;          // AILowLevel::mDest
   uint8_t    saved[kDestSize] = {};   // the AI's own
   V3         point;                   // where it is steered
   float      target = 0.0f;           // the speed asked for, a share of its top speed
   float      keepCos = flyer_wingmen::kKeepSpeedCos;   // that speed held until the point is this far off its nose
   bool       chasing = false;
   float      leaderSpeed = 0.0f;
   Frame      frame;
   V3         slot;
};

// The AI's destination swapped for ours; what it replaced is kept for after.
void write_dest(uint8_t* flyer, Steer& st, V3 heading, int safeHeight)
{
   uint8_t* const dest = *(uint8_t**)(flyer + kFly_Ctrl) + kUC_LowLevel + kLL_Dest;
   std::memcpy(st.saved, dest, kDestSize);
   st.dest = dest;   // from here on the AI's own goes back, whatever happens
   st.dest[kDest_Has] = 1;
   std::memcpy(st.dest + kDest_Pos, &st.point, 12);
   std::memcpy(st.dest + kDest_Original, &st.point, 12);
   std::memcpy(st.dest + kDest_Dir, &heading, 12);
   *(int*)(st.dest + kDest_Safe) = safeHeight;
   *(float*)(st.dest + kDest_Taper) = 0.0f;
   *(float*)(st.dest + kDest_Speed) = st.target;
}

// After its target: behind a flyer in the air, at where it will be (as real
// flight's chase, flyer_real_flight_core.hpp), flat out to close and at its
// speed once its own gap behind it; at anything else, passes
// (flyer_orders_core.hpp): a dive at it with the safe height let go near it,
// then out and up, as far as its own weapon makes them; at its own speeds.
bool steer_chase(uint8_t* flyer, Wingman& w, Steer& st)
{
   uint8_t* const t = w.target;
   if (!valid_target(t, w.targetHandle)) return false;
   const float* tm = matrix_of(t);
   const float* wm = matrix_of(flyer);
   if (!tm || !wm) return false;
   const V3 me = row(wm, 3), target = row(tm, 3), targetVel = velocity_of(t);
   V3 nose = row(wm, 2), to = target - me;
   const float d = flyer_wingmen::length(to);
   if (!flyer_wingmen::normalise(nose) || !flyer_wingmen::normalise(to)) return false;
   const float aheadCos = flyer_wingmen::dot(nose, to);
   const float hi = s_maxSpeed(flyer, nullptr), lo = s_minSpeed(flyer, nullptr), mid = s_midSpeed(flyer, nullptr);
   const float mySpeed = flyer_wingmen::length(velocity_of(flyer));
   const double now = seconds();
   if (const bool ground = !airborne(t); ground != w.ground) {   // its target landed, or took off
      w.ground = ground;
      w.run = flyer_orders::Run{false, now};
   }
   int safe = kSafeHeightFull;
   float speed;
   if (!w.ground) {
      st.point = flyer_real_flight::pursuit_point(me, mySpeed, target, targetVel);
      speed = flyer_real_flight::pursuit_speed(d, aheadCos, flyer_wingmen::length(targetVel), lo, hi, w.shape.gap);
   } else {
      flyer_orders::update_run(w.run, w.shape, d, aheadCos, now);
      if (w.run.extending) {
         st.point = flyer_orders::extend_point(me, nose);
         speed = hi;
      } else {
         st.point = flyer_real_flight::pursuit_point(me, mySpeed, target, targetVel);
         speed = mid;
         safe = kSafeHeightTaper;
      }
   }
   V3 heading = st.point - me;
   if (!flyer_wingmen::finite(st.point) || !flyer_wingmen::normalise(heading)) return false;
   st.target = flyer_wingmen::target_for_speed(speed, lo, mid, hi);
   st.keepCos = flyer_real_flight::kPursuitKeepCos;
   st.chasing = true;
   write_dest(flyer, st, heading, safe);
   return true;
}

// While its leader lands: down toward a point over its spot, at its slowest from
// kLandSlowFrom in, the safe height let go near it; else (no level ground, or up
// again before its leader) round the sky over its leader.
bool steer_land(uint8_t* flyer, Formation& f, Wingman& w, Steer& st)
{
   const float* wm = matrix_of(flyer);
   if (!wm || !f.leader) return false;
   const V3 me = row(wm, 3);
   const float leaderSpeed = flyer_wingmen::length(velocity_of(f.leader));
   if (std::isfinite(leaderSpeed)) f.leaderSpeed = f.leaderTarget = leaderSpeed;
   const float hi = flyer_wingmen::stretched_max(s_maxSpeed(flyer, nullptr), high_speed(f));
   const float lo = flyer_wingmen::stretched_min(s_minSpeed(flyer, nullptr), low_speed(f));
   const float mid = s_midSpeed(flyer, nullptr);
   int safe = kSafeHeightFull;
   float speed = mid;
   if (w.land == kLandComing) {
      st.point = w.spot + V3{0.0f, flyer_orders::kLandHover, 0.0f};
      if (flyer_orders::across(me, w.spot) < flyer_orders::kLandSlowFrom) speed = lo;
      safe = kSafeHeightTaper;
   } else {
      st.point = flyer_orders::loiter_point(f.pad + V3{0.0f, flyer_orders::kLoiterHeight, 0.0f}, me);
   }
   V3 heading = st.point - me;
   if (!flyer_wingmen::finite(st.point) || !flyer_wingmen::normalise(heading)) return false;
   st.target = flyer_wingmen::target_for_speed(speed, lo, mid, hi);
   st.keepCos = flyer_wingmen::kKeepSpeedCos;
   st.chasing = true;   // not at its slot: no slot statistics
   write_dest(flyer, st, heading, safe);
   return true;
}

bool steer_before(uint8_t* flyer, float dt, Steer& st)
{
   Formation* f = nullptr;
   Wingman* const w = find(flyer, &f);
   if (!w || !flying(flyer)) return false;   // landing, landed or taking off: the game's own
   if (handle_of(flyer) != w->handle || *(uint8_t* const*)(flyer + kFly_Ctrl) != w->uc) return false;
   if (f->landing && steer_land(flyer, *f, *w, st)) {
      st.f = f;
      st.w = w;
      return true;
   }
   if (w->target && steer_chase(flyer, *w, st)) {
      st.f = f;
      st.w = w;
      return true;
   }
   // At its slot (a chase with nothing to steer at this update included).
   uint8_t* const leader = f->leader;
   if (handle_of(leader) != f->leaderHandle || !alive(leader)) return false;
   const float* lm = matrix_of(leader);
   const float* wm = matrix_of(flyer);
   if (!lm || !wm) return false;
   const V3 leaderVel = velocity_of(leader);
   st.frame = flyer_wingmen::level_frame(row(lm, 2), row(lm, 0));
   if (!st.frame.valid || !flyer_wingmen::finite(leaderVel)) return false;
   st.leaderSpeed = flyer_wingmen::length(leaderVel);
   f->leaderSpeed = st.leaderSpeed;
   // Braking or boosting, the leader's throttle says so before its speed does.
   const float aim = *(const float*)(leader + kFly_TargetSpeed);
   f->leaderTarget = std::isfinite(aim) && aim > 0.0f ? aim : st.leaderSpeed;

   const V3 me = row(wm, 3), leaderPos = row(lm, 3);
   st.slot = flyer_wingmen::to_world(st.frame, leaderPos, slot_of(*f, w->slot));
   const V3 slot = flyer_wingmen::in_frame(st.frame, me, st.slot);   // the slot, from the wingman
   if (!w->holding) flyer_wingmen::update_trim(w->trim, slot, dt);
   const V3 steer = flyer_wingmen::keep_clear(
      flyer_wingmen::steer_offset(slot, flyer_wingmen::lead_distance(st.leaderSpeed), w->trim.climb),
      flyer_wingmen::in_frame(st.frame, me, leaderPos));
   st.point = flyer_wingmen::to_world(st.frame, me, steer);
   V3 heading = st.frame.fwd;

   // Its speeds are the stretched ones, as its getters answer for it (above),
   // and the target the one the game's throttle turns into that speed.
   const float hi = flyer_wingmen::stretched_max(s_maxSpeed(flyer, nullptr), high_speed(*f));
   const float lo = flyer_wingmen::stretched_min(s_minSpeed(flyer, nullptr), low_speed(*f));
   float speed = flyer_wingmen::formation_speed(f->leaderTarget + w->trim.speed, slot.z, lo, hi);

   // Through the leader's trick and its own copy: straight off its nose, at the
   // leader's speed.
   V3 nose = row(wm, 2);
   if (w->holding && flyer_wingmen::normalise(nose)) {
      st.point = me + nose * flyer_wingmen::kHoldAhead;
      heading = nose;
      speed = flyer_wingmen::formation_speed(f->leaderTarget, 0.0f, lo, hi);
   }
   st.target = flyer_wingmen::target_for_speed(speed, lo, s_midSpeed(flyer, nullptr), hi);
   write_dest(flyer, st, heading, kSafeHeightFull);
   st.f = f;
   st.w = w;
   return true;
}

// A chase keeps the stock speed where CalculateDest lifted its point (over
// terrain, or something in the way, which it slows for), as real flight does.
constexpr float kRaisedBy = 1.0f;   // m

void steer_after(uint8_t* flyer, Steer& st, bool steered, const float* point, float* speed)
{
   std::memcpy(st.dest, st.saved, kDestSize);   // the AI's own destination back
   const float* wm = matrix_of(flyer);
   if (!wm) return;
   const V3 pos = row(wm, 3), fwd = row(wm, 2);
   const bool lifted = st.chasing && point && point[1] > st.point.y + kRaisedBy;
   if (steered && speed && !lifted && !(flyer[kFly_Flags] & kFlagSliding)) {
      V3 to = st.point - pos;
      const float cosOff = flyer_wingmen::normalise(to) ? flyer_wingmen::dot(to, fwd) : 1.0f;
      *speed = flyer_wingmen::keep_speed(*speed, st.target, cosOff, st.keepCos);
   }
   if (!st.chasing)
      st.w->stats.add(flyer_wingmen::in_frame(st.frame, pos, st.slot), flyer_wingmen::length(velocity_of(flyer)),
                      st.leaderSpeed);
}

// thiscall(dt, dest, point, speed, myPos) on the entity, RET 0x14, as __fastcall
// with EDX unused.
bool __fastcall calc_dest_mt(uint8_t* flyer, void*, float dt, void* dest, float* point, float* speed, float* myPos)
{
   Steer st;
   flyer_real_flight::Steer onPath;   // a flyer on real flight, at its carrot or its target
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
         steer_after(flyer, st, r, point, speed);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
   } else if (chasing) {
      __try {
         flyer_real_flight::steer_after(flyer, onPath, r, point, speed);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         flyer_real_flight::restore(onPath);
      }
   }
   // Every AI flyer, however steered, clear of the flyers around it.
   if (r && flyer_avoidance::active()) {
      __try {
         flyer_avoidance::adjust(flyer, (const uint8_t*)dest, point, speed);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
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

// The squadron rule's reads, as the game's own code makes them: EntityFormation::Find
// walking its list; UpdatePathFollower counting the squadron time (+0x1CE8) down,
// then reading the class's squadron distance (+0x1108), else its model's bounding
// radius (GetModel at the class's vptr +0x20, +0x20, +0x94) times 30, and its angle
// (+0x1104).
bool install_squadrons(uintptr_t base)
{
   const auto* a = g_addr;
   if (!s_findShape || !a->flyer_update_path_follower) return false;
   const uint8_t* const upf = (const uint8_t*)resolve(base, a->flyer_update_path_follower);
   return std::memcmp(upf + 0x134, "\xD9\x86\xE8\x1C\x00\x00", 6) == 0 &&
          std::memcmp(upf + 0x153, "\x8B\x8E\x6C\x06\x00\x00\xD9\x05\x6C\xA0\xA2\x00\xD9\x81\x08\x11\x00\x00", 18) == 0 &&
          std::memcmp(upf + 0x17A, "\x8B\x11\xFF\x52\x20\x8B\x40\x20\xD9\x80\x94\x00\x00\x00\xD8\x0D\x18\x7E\xA3\x00", 20) == 0 &&
          std::memcmp(upf + 0x1E6, "\x8B\x91\x04\x11\x00\x00", 6) == 0;
}

// The map's formation shapes: EntityFormation::Find, which walks their list from
// its head's next (PUSH ESI; MOV ESI, [head + 4]; MOV ECX, [ESI + 0xC]; TEST ECX, ECX).
bool install_shapes(uintptr_t base)
{
   const auto* a = g_addr;
   if (!a->entity_formation_find || !a->entity_formation_list) return false;
   uint8_t* const list = (uint8_t*)resolve(base, a->entity_formation_list);
   uint8_t find[12] = {0x56, 0x8B, 0x35, 0, 0, 0, 0, 0x8B, 0x4E, 0x0C, 0x85, 0xC9};
   const uint32_t first = (uint32_t)(uintptr_t)(list + kList_Next);
   std::memcpy(find + 3, &first, 4);
   if (std::memcmp(resolve(base, a->entity_formation_find), find, sizeof find) != 0) return false;
   s_findShape = (fn_find_shape_t)resolve(base, a->entity_formation_find);
   s_shapeList = list;
   return true;
}

// ---- The comm keys --------------------------------------------------------------------
// TeamOrderInputProcess's CALL to NetGame::AddChatMessageToQue, made once for a
// comm key pressed, its code (0-7) the fifth argument: passed on as it was, and
// the code kept for the next flyer update (comm_key). The caller pops the
// arguments and keeps nothing in a register across the call.
uint32_t __cdecl team_order_mt(uint32_t teamOnly, uint32_t localIndex, uint32_t lo, uint32_t hi, int code,
                               const wchar_t* text, uint32_t mask)
{
   s_pendingComm = code;
   s_pendingAt = seconds();
   return s_addChat(teamOnly, localIndex, lo, hi, code, text, mask);
}

// The orders' reads and calls, as the game's own code makes them.
bool install_orders(uintptr_t base)
{
   const auto* a = g_addr;
   if (!a->controllable_locked_target || !a->uc_force_attack_target || !a->object_affiliation || !a->hud_message ||
       !a->hud_message_color || !a->localize_find || !a->team_order_input || !a->team_order_chat_call ||
       !a->add_chat_message)
      return false;
   uint8_t* const call = (uint8_t*)resolve(base, a->team_order_chat_call);
   const bool ok =
      // GetTargetLockedObj: the player id (+0xD4) first, the lock kept at +0x138.
      starts_with(base, a->controllable_locked_target, "\x56\x8B\xF1\x57\x8B\xBE\xD4\x00\x00\x00", 10) &&
      starts_with(base, a->controllable_locked_target + 0x7B, "\x8B\x86\x38\x01\x00\x00", 6) &&
      // ForceSetAttackHelperTarget: the agent (+0x2C0), its AttackHelper (+0x10).
      starts_with(base, a->uc_force_attack_target, "\x8B\x81\xC0\x02\x00\x00\x85\xC0\x74\x08\x8D\x48\x10\xE9", 14) &&
      // GetAffiliationToMe: its own team (+0x234) into the table.
      starts_with(base, a->object_affiliation, "\x53\x55\x56\x8B\xF1\x8B\x86\x34\x02\x00\x00", 11) &&
      // The message line: the player's events, 0x328 apart.
      starts_with(base, a->hud_message, "\x8B\x44\x24\x0C\x83\xEC\x10\x56\x8B\x74\x24\x18\x69\xF6\x28\x03\x00\x00", 18) &&
      starts_with(base, a->localize_find, "\x8B\x44\x24\x04\x50\x68\x00\x80\x00\x00", 10) &&
      // TeamOrderInputProcess: the control mode's row (0x102 bytes), then, for the
      // key pressed, the text, the code, the 64-bit zero, the local index and false
      // pushed, the CALL, and the caller popping all seven.
      starts_with(base, a->team_order_input, "\x83\xEC\x20\x8B\x41\x08\x55\x56\x57\x33\xFF\x69\xC0\x02\x01\x00\x00",
                  17) &&
      std::memcmp(call - 0x13, "\x50\x57\x6A\x00\x6A\x00\x6A\x00", 8) == 0 &&
      std::memcmp(call - 3, "\x50\x6A\x00", 3) == 0 && call[0] == 0xE8 &&
      call_destination(call) == (uintptr_t)resolve(base, a->add_chat_message) &&
      std::memcmp(call + 5, "\x83\xC4\x1C", 3) == 0 &&
      starts_with(base, a->add_chat_message, "\x55\x8B\xEC\x83\xEC\x20\x0F\xB6\x45\x08", 10);
   if (!ok) return false;
   s_lockedTarget = (fn_locked_t)resolve(base, a->controllable_locked_target);
   s_forceTarget = (fn_force_t)resolve(base, a->uc_force_attack_target);
   s_affiliation = (fn_affiliation_t)resolve(base, a->object_affiliation);
   s_message = (fn_message_t)resolve(base, a->hud_message);
   s_messageColor = (const uint8_t*)resolve(base, a->hud_message_color);
   s_localize = (fn_localize_t)resolve(base, a->localize_find);
   s_addChat = (fn_add_chat_t)resolve(base, a->add_chat_message);
   // The call, during the installers' writable window.
   s_chatSite.at = call;
   std::memcpy(s_chatSite.orig, call, 5);
   *(int32_t*)(call + 1) = (int32_t)((intptr_t)&team_order_mt - (intptr_t)(call + 5));
   return true;
}

// Landing with the player: EntityFlyer::Land and TakeOff, thiscall() on the
// entity (Land perhaps detoured by the LandOnArrival fix already, which only
// adds to it), and CollisionManager::RayHit for the ground beside the leader.
bool install_landing(uintptr_t base)
{
   const auto* a = g_addr;
   if (!a->entity_flyer_land || !a->entity_flyer_take_off || !a->collision_ray_hit) return false;
   const bool ok =
      (starts_with(base, a->entity_flyer_land, "\x56\x8B\xF1\x8B\x86\xA4\x05\x00\x00", 9) ||
       *(const uint8_t*)resolve(base, a->entity_flyer_land) == 0xE9) &&
      starts_with(base, a->entity_flyer_take_off, "\x51\x56\x8B\xF1\x8B\x86\xA4\x05\x00\x00\x85\xC0\x57", 13) &&
      starts_with(base, a->collision_ray_hit, "\xD9\x05\x90\xA2\xA2\x00\x8B\x54\x24\x10", 10);
   if (!ok) return false;
   s_land = (fn_flyer_t)resolve(base, a->entity_flyer_land);
   s_takeOff = (fn_flyer_t)resolve(base, a->entity_flyer_take_off);
   s_rayHit = (fn_ray_hit_t)resolve(base, a->collision_ray_hit);
   return true;
}

// ---- Waiting aboard ---------------------------------------------------------------------
// The fly agent's FLY state asks IsNearLandingPt: near one of the map's land hints
// it lands there, and once down its DROPOFF state gets the pilot out
// (AILowLevel::SetExitVehicle). A wingman lands only with its leader, so for one
// the answer is no (and no hint is reserved). thiscall() on the agent, the answer
// in AL; the caller keeps only ESI past the call.
bool __fastcall land_hint_mt(uint8_t* agent, void*)
{
   bool wingman = false;
   __try {
      uint8_t* const uc = *(uint8_t**)(agent + kAgent_Ctrl);
      uint8_t* const owner = uc ? *(uint8_t**)(uc + kCtrl_Owner) : nullptr;
      uint8_t* const obj = owner ? object_of(owner) : nullptr;
      wingman = obj && held(obj);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      wingman = false;
   }
   return wingman ? false : s_landHint(agent);
}

// The fly agent's TAKEOFF and FLY states take a landed flyer up
// (EntityFlyer::TakeOff, thiscall() on the flyer). A wingman landed beside its
// leader waits for it instead: they take off together (end_landing calls
// TakeOff itself). Each caller returns straight after the call.
void __fastcall agent_take_off_mt(uint8_t* flyer, void*)
{
   bool wait = false;
   __try {
      Formation* f = nullptr;
      const Wingman* w = find(flyer, &f);
      wait = w && f->landing && w->land == kLandDown;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      wait = false;
   }
   if (!wait) s_takeOff(flyer);
}

// Waiting aboard: the agent's CALL to IsNearLandingPt (MOV ECX, ESI before it,
// TEST AL, AL then the LAND state after it) and its two CALLs to TakeOff (the
// flyer in ECX, its state tested for flying first, the agent's return after);
// AILowLevel::SetExitVehicle for WingmanExitOnLand.
bool install_waiting(uintptr_t base)
{
   const auto* a = g_addr;
   if (!s_takeOff || !a->fly_agent_land_hint_call || !a->fly_agent_land_hint || !a->fly_agent_take_off_call ||
       !a->fly_agent_take_off_call2 || !a->ai_set_exit_vehicle)
      return false;
   uint8_t* const hint = (uint8_t*)resolve(base, a->fly_agent_land_hint_call);
   uint8_t* const up1 = (uint8_t*)resolve(base, a->fly_agent_take_off_call);
   uint8_t* const up2 = (uint8_t*)resolve(base, a->fly_agent_take_off_call2);
   const uintptr_t takeOff = (uintptr_t)resolve(base, a->entity_flyer_take_off);
   const bool ok =
      std::memcmp(hint - 2, "\x8B\xCE", 2) == 0 && hint[0] == 0xE8 &&
      call_destination(hint) == (uintptr_t)resolve(base, a->fly_agent_land_hint) &&
      std::memcmp(hint + 5, "\x84\xC0\x74\x10\x8B\x16\x6A\x05", 8) == 0 &&
      starts_with(base, a->fly_agent_land_hint, "\x51\x56\x8B\xF1\xE8", 5) &&
      std::memcmp(up1 - 0xF, "\x83\xB8\xA4\x05\x00\x00\x02", 7) == 0 && std::memcmp(up1 - 2, "\x8B\xC8", 2) == 0 &&
      up1[0] == 0xE8 && call_destination(up1) == takeOff && std::memcmp(up1 + 5, "\x5F\x5E\x5B\x8B\xE5\x5D\xC3", 7) == 0 &&
      std::memcmp(up2 - 0x16, "\x83\xB8\xA4\x05\x00\x00\x02", 7) == 0 && std::memcmp(up2 - 2, "\x8B\xC8", 2) == 0 &&
      up2[0] == 0xE8 && call_destination(up2) == takeOff && std::memcmp(up2 + 5, "\x5F\x5E\x5B\x8B\xE5\x5D\xC3", 7) == 0 &&
      starts_with(base, a->ai_set_exit_vehicle, "\x80\x49\x74\x08\x8B\x44\x24\x04\x89\x41\x78\xC2\x04\x00", 14);
   if (!ok) return false;
   s_landHint = (fn_land_hint_t)resolve(base, a->fly_agent_land_hint);
   s_exitVehicle = (fn_exit_t)resolve(base, a->ai_set_exit_vehicle);
   // The calls, during the installers' writable window.
   auto redirect = [](Site& site, uint8_t* at, const void* to) {
      site.at = at;
      std::memcpy(site.orig, at, 5);
      *(int32_t*)(at + 1) = (int32_t)((intptr_t)to - (intptr_t)(at + 5));
   };
   redirect(s_landHintSite, hint, (const void*)&land_hint_mt);
   redirect(s_takeOffSites[0], up1, (const void*)&agent_take_off_mt);
   redirect(s_takeOffSites[1], up2, (const void*)&agent_take_off_mt);
   return true;
}

} // namespace

void flyer_wingmen_install(uintptr_t base)
{
   if (g_build != GameBuild::Modtools) {
      if (g_flyerWingmen) install_log("[Wingmen] NOT installed: the prototype runs on modtools only");
      if (g_flyerRealFlight) install_log("[RealFlight] NOT installed: the prototype runs on modtools only");
      if (g_flyerAvoidance) install_log("[Avoidance] NOT installed: the prototype runs on modtools only");
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
   s_random ^= (uint32_t)f.QuadPart;
   g_flyerWingmenCount = flyer_wingmen::count_for(nullptr, g_flyerWingmenCount);   // 0 to 8
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
               g_flyerWingmenAuto ? "called up at takeoff" : "called up on the squad key");
   if (install_orders(base))
      install_log("[Wingmen] orders installed: the squad key forms up or engages, the comm keys' attack and defend "
                  "order attack my target and cover me");
   else
      install_log("[Wingmen] orders NOT installed: their reads differ; the squad key forms up or engages, with no "
                  "message");
   if (!g_flyerWingmenLand)
      install_log("[Wingmen] landing with you off ([AI] FlyerWingmenLand=0): wingmen are let go when you land");
   else if (install_landing(base))
      install_log("[Wingmen] landing with you installed: wingmen land beside you, or circle overhead, and take off "
                  "with you; %s",
                  install_waiting(base) ? "they wait aboard (WingmanExitOnLand = 1 on a class gets its pilots out)"
                                        : "NOT kept aboard (the fly agent's calls differ): near a land hint their "
                                          "AI may land and get out");
   else
      install_log("[Wingmen] landing with you NOT installed: EntityFlyer's Land or TakeOff differ; wingmen are let go "
                  "when you land");

   // Real flight on paths rides on these hooks (flyer_real_flight.hpp).
   flyer_real_flight::Borrowed borrowed;
   borrowed.maxSpeed = s_maxSpeed;
   borrowed.minSpeed = s_minSpeed;
   borrowed.midSpeed = s_midSpeed;
   borrowed.wingman = &held;
   const bool realFlight = flyer_real_flight::install(base, borrowed);

   // AI squadrons need real flight (they gather flyers on it).
   // The map's formation shapes, for the player's flight and the AI squadrons.
   if (!install_shapes(base)) install_log("[Wingmen] the map's formation shapes unavailable: EntityFormation::Find differs");

   // AI squadrons need real flight (they gather flyers on it).
   s_squadrons = realFlight && install_squadrons(base);
   if (realFlight)
      install_log("[Squadrons] %s", !s_squadrons ? "NOT installed: the squadron rule's reads differ"
                                    : g_flyerRealFlightSquadrons
                                       ? "installed (prototype): AI flyers on real flight gather squadrons by the "
                                         "game's rule, in its formation shapes"
                                       : "off ([AI] FlyerRealFlightSquadrons=0)");

   // Avoidance runs in the CalculateDest stand-in above.
   flyer_avoidance::install(base);
}

void flyer_wingmen_uninstall()
{
   flyer_avoidance::uninstall();
   flyer_real_flight::uninstall();
   if (s_callSite.at) protected_write(s_callSite.at, s_callSite.orig, 5);
   s_callSite = Site{};
   if (s_chatSite.at) protected_write(s_chatSite.at, s_chatSite.orig, 5);
   s_chatSite = Site{};
   for (Site* s : {&s_landHintSite, &s_takeOffSites[0], &s_takeOffSites[1]}) {
      if (s->at) protected_write(s->at, s->orig, 5);
      *s = Site{};
   }
   s_landHint = nullptr;
   s_exitVehicle = nullptr;
   s_lockedTarget = nullptr;
   s_forceTarget = nullptr;
   s_affiliation = nullptr;
   s_message = nullptr;
   s_messageColor = nullptr;
   s_localize = nullptr;
   s_addChat = nullptr;
   s_pendingComm = -1;
   s_land = nullptr;
   s_takeOff = nullptr;
   s_rayHit = nullptr;
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
   for (Formation& f : s_forms) f = Formation{};
   s_squadrons = false;
   s_findShape = nullptr;
   s_shapeList = nullptr;
   s_mapShapesLogged = false;
   s_classes.clear();
}
