#include "pch.h"
#include "flyer_wingmen.hpp"
#include "render/hud_wingmen.hpp"
#include "flyer_avoidance.hpp"
#include "flyer_orders_core.hpp"
#include "flyer_real_flight.hpp"
#include "flyer_real_flight_core.hpp"
#include "flyer_trail_core.hpp"
#include "flyer_wingmen_core.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "entity/odf_gameext_props.hpp"
#include "game/Battlefront2/Source/Character.h"
#include "lua/lua_events.hpp"
#include "util/install_log.hpp"

#include <detours.h>

#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>
#include <unordered_map>

// See flyer_wingmen.hpp for the behaviour and the reads.

bool  g_flyerWingmen        = false;
int   g_flyerWingmenCount   = flyer_wingmen::kDefaultWingmen;
float g_flyerWingmenSpacing = flyer_wingmen::kDefaultSpacing;
bool  g_flyerWingmenAuto    = false;   // the user: none until the squad key calls them
bool  g_flyerWingmenLand    = true;
bool  g_flyerWingmenMarkers = true;
bool  g_flyerWingmenTrail   = true;
bool  g_flyerRealFlightSquadrons = true;

namespace {

using flyer_orders::Order;
using flyer_wingmen_lua::WingmanRef;
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
constexpr uint32_t kSquadDown        = 1;       // down (the trooper squad key's hold tests it)
constexpr int      kStateLanded      = 0;       // EntityFlyer::State: landed ...
constexpr int      kStateTakeoff     = 1;       // ... taking off ...
constexpr int      kStateFlying      = 2;       // ... flying ...
constexpr int      kStateLanding     = 3;       // ... landing
constexpr int      kGroundMask       = 0x190;   // what the flyer's own landing ray hits (EntityFlyer::Update 0x004FEA8D):
                                                 // COLL_STATIC, COLL_TERRAIN, COLL_WATER; no vehicles (COLL_RIGID) or soldiers
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
constexpr uint32_t kAgent_AttackTarget = 0x10;   // ... its AttackHelper (its target first) ...
constexpr uint32_t kAgent_AttackHandle = 0x14;   // ... and that one's handle
// The comm keys, read where the player's controller reads its other inputs:
// PlayerController::mJoystick, its ProcessedInputs, whose +0x10 has bit n set
// while a key bound to digital input n is held, for a pad's button the frame it
// goes down (ProcessedInputs::GetDigital).
constexpr uint32_t kPC_Inputs          = 0x88;
constexpr uint32_t kInputs_Bits        = 0x10;
constexpr int      kInputCommAttack    = 26;     // ePROCESSEDINPUT_Comm_Chat_6: the comm key Attack (F11 by default) ...
constexpr int      kInputCommDefend    = 27;     // ... Comm_Chat_7, Defend (F12)
constexpr int      kInputCommFormation = 20;     // Comm_Chat_0, Spotted (F5): in a flyer, the formation key
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
constexpr int      kSafeHeightTaperFactor = 3;   // SH_TAPER_FACTOR: with mTaperFactor 0, the point is not lifted at all

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
constexpr uint32_t kRttiPlayerController = pbl_hash("PlayerController");
// The localisation keys hash as the game's own squad command keys do (its
// EntitySoldier::Update looks this one up as 0x731FD493).
static_assert(pbl_hash("game.squadcommand.follow") == 0x731FD493u, "localisation keys are PblHashes");
constexpr uint32_t kHashClass = pbl_hash("WingmanClass");
constexpr uint32_t kHashCount = pbl_hash("WingmanCount");
constexpr uint32_t kHashAuto  = pbl_hash("WingmanAuto");
constexpr uint32_t kHashSlot  = pbl_hash("WingmanSlot");
constexpr uint32_t kHashFormation = pbl_hash("WingmanFormation");
constexpr uint32_t kHashExitOnLand = pbl_hash("WingmanExitOnLand");
constexpr uint32_t kHashLand = pbl_hash("WingmanLand");
constexpr uint32_t kHashWingman = pbl_hash("Wingman");
constexpr uint32_t kHashRole = pbl_hash("WingmanRole");
constexpr uint32_t kHashStartOrder = pbl_hash("WingmanStartOrder");
constexpr uint32_t kHashEngageRange = pbl_hash("WingmanEngageRange");
constexpr uint32_t kRttiGameObject = pbl_hash("GameObject");
constexpr int      kIdMapSlots = 0x800;   // EntityEx::mIdMap, the table every Lua name lookup searches

constexpr double kTickSeconds   = 1.0;
constexpr double kReportSeconds = 5.0;
constexpr double kKeyRepeat     = 0.3;   // s: one press, seen by every flyer's update that frame
constexpr double kForceEvery    = 0.5;   // s between settings of a chasing wingman's attack target
constexpr double kSightEvery    = 0.25;  // s between looks along its line to its target
constexpr double kOpenEvery     = 2.0;   // s between workings-out of its target's open side
constexpr float  kViaReached    = 100.0f;   // m: the way round to the open side, passed
constexpr double kExitAgain     = 2.0;   // s: a landed wingman's pilot asked to get out again ...
constexpr int    kExitTries     = 4;     // ... this many times in all
constexpr double kSettleFor     = 20.0;  // s a landed wingman of a leader gone has to get its pilot out
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
using fn_attack_reset_t = void(__thiscall*)(void* attackHelper);
using fn_affiliation_t = int(__thiscall*)(void* me, void* other);
using fn_message_t    = void(__cdecl*)(int player, const wchar_t* text, const uint8_t* color);
using fn_localize_t   = const wchar_t*(__cdecl*)(uint32_t hash, bool nullIfMissing);
using fn_ray_hit_t    = float(__cdecl*)(const float* start, const float* dir, float length, void** hit, float* normal,
                                        uint8_t** ignore, int ignoreCount, int typeMask, int ordnanceOnly);
using fn_add_marker_t = void(__cdecl*)(void* obj, void* region, uint32_t icon, float scale, uint32_t teams,
                                       uint32_t color, uint32_t hudTarget, uint32_t flash, uint32_t pulse,
                                       float pulseFrom);
using fn_remove_marker_t = void(__cdecl*)(void* obj, void* region, uint32_t teams);
using fn_flyer_t      = void(__thiscall*)(void* flyer);
using fn_land_hint_t  = bool(__thiscall*)(void* agent);
using fn_exit_t       = void(__thiscall*)(void* lowLevel, void* commandPost);
using fn_find_name_t  = uint8_t*(__cdecl*)(void* table, int slots, uint32_t hash);

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
fn_attack_reset_t s_attackReset = nullptr;   // AttackHelper::Reset: its target let go (holding fire)
fn_affiliation_t s_affiliation  = nullptr;   // GameObject::GetAffiliationToMe
fn_message_t     s_message      = nullptr;   // the HUD's message line
const uint8_t*   s_messageColor = nullptr;   // the trooper squad commands' colour
fn_localize_t    s_localize     = nullptr;
bool             s_commKeys     = false;     // the comm keys' reads checked
uint32_t         s_commHeld     = 0;         // the comm keys' bits held at the last look
flyer_orders::SquadHold s_squadHold;         // the squad key held down: breaking the flight off
// The player's wingmen's markers (null until their reads are checked).
fn_add_marker_t    s_addMarker    = nullptr; // TargetManager::AddMarker
fn_remove_marker_t s_removeMarker = nullptr; // TargetManager::RemoveMarker
// Landing with the player (null until their reads are checked).
fn_ray_hit_t     s_rayHit  = nullptr;        // CollisionManager::RayHit
fn_flyer_t       s_land    = nullptr;        // EntityFlyer::Land
fn_flyer_t       s_takeOff = nullptr;        // EntityFlyer::TakeOff
fn_land_hint_t   s_landHint = nullptr;       // UnitFlyAgent::IsNearLandingPt (waiting aboard)
fn_exit_t        s_exitVehicle = nullptr;    // AILowLevel::SetExitVehicle
// A mission script's names for world objects (null until their reads are checked).
fn_find_name_t   s_findName = nullptr;       // PblHashTableCode::_Find ...
void*            s_idMap = nullptr;          // ... on EntityEx::mIdMap

struct Site {
   uint8_t* at = nullptr;
   uint8_t  orig[5] = {};
};
Site s_callSite;
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
   if (hash == kHashLand) {
      if (cls) flyer_wingmen::set_land(s_classes[cls], value);
      return true;
   }
   if (hash == kHashWingman) {
      if (cls) flyer_wingmen::set_wingman(s_classes[cls], value);
      return true;
   }
   if (hash == kHashRole) {
      if (cls) flyer_orders::set_role(s_classes[cls], value);
      return true;
   }
   if (hash == kHashStartOrder) {
      if (cls) flyer_orders::set_start_order(s_classes[cls], value);
      return true;
   }
   if (hash == kHashEngageRange) {
      if (cls) flyer_orders::set_engage_range(s_classes[cls], value);
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
   bool     touchedDown = false;   // landed beside its leader (OnWingmanLanded)
   double   landAt = 0.0;      // told to land (EntityFlyer::Land) ...
   int      landTries = 0;     // ... its landings interrupted so far
   V3       circleAt;          // with no place yet under a roof, the low circle it flies: round here ...
   float    circleR = 0.0f;    // ... this far (0: the open sky's, over its leader) ...
   double   lookAgainAt = 0.0; // ... looking for a free place again from then (kLandLookAgain)
   double   outFrom = 0.0;     // let go under a roof, it flies back out along the trail first (head_out): since ...
   int      outLooks = 0;      // ... roof looks in a row with none over it ...
   const char* outCode = nullptr;   // ... and the reason a script hears once it is out
   double   exitAt = 0.0;      // its pilot last asked to get out ...
   int      exitTries = 0;     // ... how many times
   int      pilot = -1;        // the character flying it as it joined (a script's events), or -1
   uint32_t markTeams = 0;     // the player's wingman: the teams its marker shows to, while it has one
   // Its target's line of sight and open side (flyer_orders_core.hpp):
   bool     sees = true;       // nothing solid between it and its target, as last looked ...
   double   seenAt = -1e9;     // ... when
   V3       open{0.0f, 1.0f, 0.0f};   // the side its passes come in along (up: open sky) ...
   bool     sky = true;        // ... its target is under open sky: passes as at the ground ...
   double   openAt = -1e9;     // ... as worked out then ...
   bool     hasVia = false;    // ... and the way round to that side where the straight line there is blocked
   V3       via;
   // Its leader's trail (flyer_trail_core.hpp), the player's wingmen: a wall
   // between it and where it is going, it flies the path the leader flew.
   bool     onTrail = false;
   int      trailAt = -1;      // the trail's point it has got to ...
   int      trailTo = -1;      // ... and the one it steers at
   double   trailLookAt = -1e9;   // its lines last looked along
   int      clearLooks = 0;    // looks in a row with its way clear
   double   gainedAt = 0.0;    // when it last got to a further point
   double   waitUntil = 0.0;   // stuck on it: left to its own AI until then
   bool     shut = false;      // something solid on its line to where it is going, as last looked ...
   double   shutAt = -1e9;     // ... when (follow_trail)
   bool     roofed = false;    // under a roof itself, as last looked (update_trail): the game's lift off (no_lift)
};

// A wingman of the player's while the player lands and takes off.
enum : int {
   kLandNone    = 0,
   kLandComing  = 1,   // coming down beside its leader
   kLandCircle  = 2,   // circling overhead: no level ground beside its leader
   kLandDown    = 3,   // landing or landed
   kLandLeaving = 4,   // taking off with its leader
   kLandOut     = 5,   // landed, its pilot getting out (unless WingmanExitOnLand = 0)
   kLandSeek    = 6,   // its landing interrupted: taking off again, for a clear place
   kLandOutward = 7,   // let go under a roof: flying back out along the leader's trail first (head_out)
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
   bool        quiet = false;         // the order is a mission script's: no line on the HUD
   bool        called = false;        // the player called wingmen: losses are made up even without WingmanAuto
   bool        brokenOff = false;     // the player broke the flight off: none joins on its own until called again
   bool        handPicked = false;    // the player picked its wingmen one by one: losses are not made up
   bool        landing = false;       // the player's flyer is landing or landed: its wingmen land too ...
   V3          pad;                   // ... beside where it came down ...
   Frame       padFrame;              // ... facing as it was ...
   float       padGround = 0.0f;      // ... the ground's height there ...
   float       padRoof = flyer_orders::kNoRoof;   // ... a roof's over it (a hangar's), else kNoRoof ...
   int         padTeam = 0;           // ... the leader's team as it came down ...
   double      outAt = 0.0;           // ... and when the player got out of it, its flight still landing (0: aboard)
   bool        mirror = false;        // left for right, after an odd number of the leader's flips
   float       leaderSpeed = 0.0f;    // as last seen by a wingman's steering: how fast it flies ...
   float       leaderTarget = 0.0f;   // ... and the speed its throttle asks for
   LeaderTrick trick;
   V3          shape[flyer_wingmen::kMaxWingmen];   // an AI squadron's slots, from the map's formation shape
   int         shapeCount = 0;        // none: the leader class's WingmanSlot lines, else the vee
   uint32_t    shapeName = 0;
   bool        vee = false;           // a script's SetWingmenFormation("vee"): the vee whatever the class says
   bool        roofed = false;        // the player's flight: its leader under a roof, as last looked ...
   double      roofAt = -1e9;         // ... when ...
   float       squeeze = 1.0f;        // ... and its slots' share of their spacing (flyer_trail_core.hpp)
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

// ---- What a mission script says (Lua, flyer_wingmen.hpp) ------------------------------
// The player's flight as a script shapes it, for the rest of the mission
// (flyer_wingmen_mission_end clears it): the flyers it names, which join first
// (the roster), how many are called up on their own and when, the standing
// order, the formation, the landing.
struct RosterEntry {
   uint32_t name = 0;        // a world object's name (PblHash): whatever carries it now ...
   uint8_t* obj = nullptr;   // ... or this flyer, while its handle holds
   uint32_t handle = 0;
};
constexpr int kMaxExcluded = 32;
RosterEntry s_roster[flyer_wingmen::kMaxWingmen];
int         s_rosterCount = 0;
uint32_t    s_excluded[kMaxExcluded] = {};   // flyers RemoveWingman let go: never called up on their own again
int         s_excludedCount = 0;
bool        s_scripted = false;        // CallWingmen or AddWingman: the player's flyer leads whatever the INI says
bool        s_locked = false;          // SetWingmenLocked
int         s_scriptSize = -1;         // CallWingmen(n): wingmen called up on their own
int         s_scriptAuto = -1;         // over WingmanAuto: 1 after CallWingmen, 0 after DismissWingmen
int         s_scriptOrder = -1;        // SetWingmenOrder's standing order (an Order), over WingmanStartOrder
int         s_scriptLand = -1;         // SetWingmenLand, over WingmanLand
bool        s_scriptShapeSet = false;  // SetWingmenFormation: ...
bool        s_scriptVee = false;       // ... the vee ...
uint32_t    s_scriptShape = 0;         // ... or this map shape

// ---- The script's events ------------------------------------------------------------
// Queued, and fired once the flyer's own update is done or the script's call
// has finished, never halfway through changing a flight, so a callback may
// change it.
struct PendingEvent {
   LuaEventId  id;
   int         chr;      // the character it is about
   uint8_t*    obj;      // its flyer, or the order's target; may be null
   const char* reason;   // OnWingmanLost's
};
constexpr int kMaxEvents = 64;
PendingEvent  s_events[kMaxEvents];
int           s_eventCount = 0;
const char*   s_lostReason = nullptr;   // GetWingmanLostReason, during OnWingmanLost

void queue_event(LuaEventId id, int chr, uint8_t* obj, const char* reason = nullptr)
{
   if (chr < 0 || s_eventCount >= kMaxEvents) return;
   s_events[s_eventCount++] = {id, chr, obj, reason};
}

void flush_events()
{
   static bool flushing = false;
   if (flushing) return;   // fired from a callback: the loop below takes what it queues
   flushing = true;
   for (int fired = 0; s_eventCount > 0 && fired < 2 * kMaxEvents; ++fired) {
      const PendingEvent e = s_events[0];
      --s_eventCount;
      std::memmove(s_events, s_events + 1, (size_t)s_eventCount * sizeof(PendingEvent));
      void* const chr = lua_event_character_from_index(e.chr);
      if (!chr) continue;
      s_lostReason = e.reason;
      lua_event_fire(e.id, chr, e.obj);
      s_lostReason = nullptr;
   }
   s_eventCount = 0;
   flushing = false;
}

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

// Whether the player's wingmen land with it: a script's SetWingmenLand, else its
// class's WingmanLand, else the INI's FlyerWingmenLand.
bool lands(const uint8_t* leader)
{
   return s_scriptLand >= 0 ? s_scriptLand != 0 : flyer_wingmen::lands_with(keys_of(leader), g_flyerWingmenLand);
}

// The flyer the host's own player flies, while it flies (or, with its wingmen
// landing with it, while it lands, sits landed and takes off again; else they
// break off as it lands).
uint8_t* player_flyer()
{
   uint8_t* const chr = s_local ? s_local(0) : nullptr;
   uint8_t* const vehicle = chr ? *(uint8_t**)(chr + layout::Character::kVehicle) : nullptr;
   if (!vehicle) return nullptr;
   uint8_t* const obj = object_of(vehicle);
   if (!obj || !is_rtti(obj, kRttiFlyer) || !alive(obj)) return nullptr;
   if (flying(obj)) return obj;
   if (s_land && on_or_near_ground(obj) && lands(obj)) return obj;
   return nullptr;
}

// The characters (Character::sCharacters): one's index from its address, and
// the one flying a flyer (its pilot, whose vehicle is the flyer's own
// Controllable), each -1 for none.
const uint8_t* characters(int& count)
{
   count = 0;
   if (!g_addr->char_array_base || !g_addr->max_chars) return nullptr;
   const uint8_t* const chars = *(const uint8_t* const*)resolve(g_addr->char_array_base);
   count = chars ? *(const int*)resolve(g_addr->max_chars) : 0;
   if (count < 0 || count > 4096) count = 0;
   return chars;
}

int character_index(const uint8_t* chr)
{
   int count = 0;
   const uint8_t* const chars = characters(count);
   if (!chars || !chr || chr < chars) return -1;
   const size_t offset = (size_t)(chr - chars);
   const int index = (int)(offset / layout::Character::kSize);
   return offset % layout::Character::kSize == 0 && index < count ? index : -1;
}

int pilot_of(const uint8_t* flyer)
{
   int count = 0;
   const uint8_t* const chars = characters(count);
   const uint8_t* const ctl = flyer + kFly_Controllable;
   for (int i = 0; chars && i < count; ++i)
      if (*(const uint8_t* const*)(chars + (size_t)i * layout::Character::kSize + layout::Character::kVehicle) == ctl)
         return i;
   return -1;
}

int player_index() { return s_local ? character_index(s_local(0)) : -1; }

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
// any, else the INI; a mission script calling or naming wingmen gives it some
// whatever either says.
bool leads(const uint8_t* leader)
{
   const ClassKeys* k = keys_of(leader);
   if (!flyer_wingmen::sets_any(k) && !g_flyerWingmen && !s_scripted) return false;
   return flyer_wingmen::flight_size(s_scriptSize, k, g_flyerWingmenCount) > 0 || s_rosterCount > 0;
}

// Wingmen are called up on their own at takeoff: after a script's CallWingmen
// (or not after its DismissWingmen), else its class's WingmanAuto, else the INI.
bool auto_now(const ClassKeys* keys)
{
   return s_scriptAuto >= 0 ? s_scriptAuto != 0 : flyer_wingmen::auto_for(keys, g_flyerWingmenAuto);
}

bool excluded(uint32_t handle)
{
   for (int i = 0; i < s_excludedCount; ++i)
      if (s_excluded[i] == handle) return true;
   return false;
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

// A fault one of the guards here caught: what it was doing is skipped and the
// game carries on (BF2GameExt_crash.log has where it faulted, the first few
// times). Said the 1st, 10th, 100th ... time for each place, so one that keeps
// happening shows without filling the log; two in the first hangar landings
// showed nowhere but the crash log (start_landing; drop_gone).
void caught(const char* where)
{
   struct Place {
      const char* where;
      uint32_t    count;
   };
   static Place places[24];
   static int   used = 0;
   Place* p = nullptr;
   for (int i = 0; i < used && !p; ++i)
      if (places[i].where == where) p = &places[i];
   if (!p) {
      if (used == 24) return;
      places[used] = Place{where, 0};
      p = &places[used++];
   }
   const uint32_t n = ++p->count;
   uint32_t say = 1;
   while (say < n && say <= 100000000u) say *= 10;
   if (say == n) install_log("[Wingmen] a fault in %s was caught (%u so far): that step was skipped", where, n);
}

// One of the player's wingmen leaves its flight: the script hears it
// (OnWingmanLost, with `code` for GetWingmanLostReason), with its flyer while
// that is still the one it was.
void lost_event(const Formation& f, const Wingman& w, const char* code)
{
   if (&f != &s_forms[0] || !code) return;
   queue_event(LuaEventId::WingmanLost, w.pilot, handle_of(w.obj) == w.handle ? w.obj : nullptr, code);
}

// ---- The player's wingmen's markers (FlyerWingmenMarkers) -------------------------------
// The game's own target markers (TargetManager, as objectives', Lua's
// MapAddEntityMarker's and the comm keys' requests' are): a steady cyan circle
// over each wingman on the map, and, as a "common" HUD target, its bracket on
// screen in the same colour; for the player's team, from the moment a wingman
// joins until it leaves. There are 32 between all markers (AddMarker takes no
// more and says nothing). Not where a .hud draws its own (hud_wingmen.hpp: the
// user, "If the hud entry isn't found, resort to the current implementation").
constexpr uint32_t kMarkerIcon  = pbl_hash("hud_objective_icon_circle");
constexpr uint32_t kMarkerColor = pbl_hash("cyan");
constexpr float    kMarkerScale = 3.0f;

void mark(Wingman& w, const uint8_t* leader)
{
   if (!s_addMarker || !g_flyerWingmenMarkers || w.markTeams || !leader || hud_wingmen_bound()) return;
   const int team = team_of(leader);
   if (team < 0) return;
   const uint32_t teams = 1u << team;
   s_addMarker(w.obj, nullptr, kMarkerIcon, kMarkerScale, teams, kMarkerColor, 1, 0, 0, 0.0f);
   w.markTeams = teams;
}

// Taken down while its flyer is still the one it was (a wingman shot down is let
// go while its wreck is). Once its handle has moved on the game shows the marker
// no more, but keeps its slot taken until the mission ends.
void unmark(Wingman& w)
{
   if (!w.markTeams) return;
   if (s_removeMarker && handle_of(w.obj) == w.handle) s_removeMarker(w.obj, nullptr, w.markTeams);
   w.markTeams = 0;
}

// The flight's attack target's marker, while it attacks one and no .hud draws its
// own: the same circle, red, on the map and bracketed on screen.
constexpr uint32_t kTargetMarkColor = pbl_hash("red");
struct TargetMark {
   uint8_t* obj = nullptr;
   uint32_t handle = 0;
   uint32_t teams = 0;
};
TargetMark s_targetMark;

void unmark_target()
{
   if (s_targetMark.obj && s_removeMarker && handle_of(s_targetMark.obj) == s_targetMark.handle)
      s_removeMarker(s_targetMark.obj, nullptr, s_targetMark.teams);
   s_targetMark = TargetMark{};
}

void drop(Formation& f, int i, const char* why, const char* code = nullptr)
{
   if (!f.ai || squad_log())
      install_log("%s slot %d (#%u) let go: %s", tag(f), f.wing[i].slot + 1, f.wing[i].handle, why);
   lost_event(f, f.wing[i], code);
   unmark(f.wing[i]);
   f.wing[i] = f.wing[--f.count];
   f.wing[f.count] = Wingman{};
}

void release_all(Formation& f, const char* why, const char* code = nullptr)
{
   if (f.count && (!f.ai || squad_log())) install_log("%s %d let go: %s", tag(f), f.count, why);
   for (int i = 0; i < f.count; ++i) {
      lost_event(f, f.wing[i], code);
      unmark(f.wing[i]);
   }
   f.count = 0;
   for (auto& w : f.wing) w = Wingman{};
   f.mirror = false;   // a new formation starts the right way round
   f.trick = LeaderTrick{};
}

// Those shot down since the last tick, first, before anything asks one where it
// is: a destroyed flyer is soon freed (modtools fills it with 0xDD), and its
// matrix is then read through freed memory (the crash log's two faults of
// 2026-10-15, both in start_landing, each on a wingman just shot down).
void drop_gone(Formation& f)
{
   for (int i = f.count - 1; i >= 0; --i)
      if (handle_of(f.wing[i].obj) != f.wing[i].handle || !alive(f.wing[i].obj)) drop(f, i, "gone", "killed");
}

// An AI squadron's end. Its wingmen need nothing put back: no longer in
// formation, each goes back to real flight at its next update.
void dissolve(Formation& f, const char* why)
{
   if (squad_log()) install_log("[Squadrons] #%u's squadron of %d broke up: %s", f.leaderHandle, f.count, why);
   f = Formation{};
   ++s_brokeUp;
}

// Still a wingman? Why not, or null; `code` the reason as a script reads it
// (GetWingmanLostReason). One chasing (its chase has its own leash,
// assign_targets) or flying in from afar (with its own deadline) is not let go
// for its distance, nor one of a script's locked flight for either.
const char* lost(const Wingman& w, const Formation& f, double now, const char** code)
{
   *code = "killed";
   if (handle_of(w.obj) != w.handle || !alive(w.obj)) return "gone";
   *code = w.land == kLandOut ? "landed" : "pilot";
   if (*(uint8_t* const*)(w.obj + kFly_Ctrl) != w.uc) return "flown by someone else";
   // Landing with its leader, landed, or taking off with it, it is still one.
   const bool landing = f.landing || w.land != kLandNone;
   *code = "landed";
   if (!flying(w.obj) && !(landing && on_or_near_ground(w.obj))) return "not flying";
   *code = "team";
   // The player out of its flyer: the side it came down on (an empty flyer's may not be).
   if (team_of(w.obj) != (f.outAt > 0.0 ? f.padTeam : team_of(f.leader))) return "changed sides";
   if (w.target || landing) return nullptr;
   const bool keep = &f == &s_forms[0] && s_locked;
   *code = "late";
   if (w.joining) return now > w.joinBy && !keep ? "did not reach its slot in time" : nullptr;
   const float* a = matrix_of(w.obj);
   const float* b = matrix_of(f.leader);
   *code = "killed";
   if (!a || !b) return "no position";
   *code = "far";
   if (!keep && flyer_wingmen::length(row(a, 3) - row(b, 3)) > flyer_wingmen::kKeepRange) return "too far";
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
// else the vee (or the vee a script asked for).
V3 slot_of(const Formation& f, int slot)
{
   const V3 s = slot >= 0 && slot < f.shapeCount ? f.shape[slot]
                : f.vee ? flyer_wingmen::slot_offset(slot, g_flyerWingmenSpacing)
                        : flyer_wingmen::slot_for(keys_of(f.leader), slot, g_flyerWingmenSpacing);
   return flyer_wingmen::mirrored(s, f.mirror);
}

// The leader class's keys where its own WingmanSlot lines decide who takes
// which slot: not while the flight flies a map shape or a script's vee.
const ClassKeys* slot_keys(const Formation& f, const ClassKeys* keys)
{
   return f.shapeCount > 0 || f.vee ? nullptr : keys;
}

// How many a flight holds: its map shape's slots, else its leader class's
// WingmanSlot lines, else as many as may fly.
int capacity(const Formation& f, const ClassKeys* keys)
{
   if (f.shapeCount > 0) return f.shapeCount;
   if (!f.vee && keys && keys->hasSlots) return keys->slotCount;
   return flyer_wingmen::kMaxWingmen;
}

// The first free slot of the player's flight that takes a flyer of class `cls`
// (a WingmanSlot line may keep one for some classes), else, for a flyer a
// script named, the first free one; -1 for none.
int free_slot(const Formation& f, const ClassKeys* keys, uint32_t cls, bool named)
{
   const int cap = capacity(f, keys);
   const ClassKeys* const own = slot_keys(f, keys);
   for (int pass = 0; pass < (named ? 2 : 1); ++pass)
      for (int slot = 0; slot < cap; ++slot)
         if (!slot_taken(f, slot) && (pass || flyer_wingmen::slot_accepts(own, slot, cls))) return slot;
   return -1;
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
// A player's flight flies the formation a mission script set (SetWingmenFormation:
// a map shape, or the vee) where the map has it; else its class's WingmanSlot
// lines where it has them; else one of the map's formation shapes: the first its
// WingmanFormation names that the map has, else the first of its
// SquadronFormation shapes that holds all its wingmen (else the largest); else
// the vee. Chosen when it starts leading, or as a script sets one. A map shape
// takes all its slots, so a flyer a script names may fly in one past those
// called up on their own.
void log_player_shape(const Formation& f, bool unturn, const char* by)
{
   const char* const nm = shape_name(f.shapeName);
   if (nm)
      install_log("[Wingmen] your flight flies the map's %s%s, %d slots%s", nm, by, f.shapeCount,
                  unturn ? ", turned back by its rotation" : "");
   else
      install_log("[Wingmen] your flight flies the map's formation 0x%08X%s, %d slots%s", f.shapeName, by, f.shapeCount,
                  unturn ? ", turned back by its rotation" : "");
}

void choose_player_shape(uint8_t* leader)
{
   Formation& f = s_player;
   f.shapeCount = 0;
   f.shapeName = 0;
   f.vee = false;
   if (s_scriptShapeSet) {
      Shape s;
      if (s_scriptVee) {
         f.vee = true;
         install_log("[Wingmen] your flight flies the vee (the mission's SetWingmenFormation)");
         return;
      }
      if (read_shape(s_scriptShape, s)) {
         const MapShapes m = map_shapes();
         f.shapeCount = shape_slots_for(s, m.unturn, s.size - 1, f.shape);
         f.shapeName = s.name;
         log_player_shape(f, m.unturn, " (the mission's SetWingmenFormation)");
         return;
      }
   }
   const ClassKeys* keys = keys_of(leader);
   if ((keys && keys->hasSlots) || !s_findShape) return;
   int want = flyer_wingmen::flight_size(s_scriptSize, keys, g_flyerWingmenCount);
   if (want < s_rosterCount) want = s_rosterCount;
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
   f.shapeCount = shape_slots_for(pick, m.unturn, pick.size - 1, f.shape);
   f.shapeName = pick.name;
   log_player_shape(f, m.unturn, "");
}

// A flyer joins the player's flight in `slot`: one further than kJoinedRange
// flies in, and has join_seconds to reach its slot; the script hears
// (OnWingmanJoin). `named`: one a script named (AddWingman).
void enlist(uint8_t* obj, uint8_t* uc, int slot, float dist, float leaderMax, double now, bool named)
{
   leave_squadron(obj);
   Wingman& w = add_wingman(s_player, obj, uc, slot);
   w.joining = dist > flyer_orders::kJoinedRange;
   w.joinFrom = now;
   w.joinBy = now + flyer_orders::join_seconds(dist);
   w.pilot = pilot_of(obj);
   if (w.joining)
      install_log("[Wingmen] slot %d: #%u %s %.0f m off, %.0f s to reach its slot; top speed %.0f m/s against the "
                  "leader's %.0f",
                  slot + 1, w.handle, named ? "named by the mission, flies in from" : "called up", (double)dist,
                  w.joinBy - now, (double)s_maxSpeed(w.obj, nullptr), (double)leaderMax);
   else
      install_log("[Wingmen] slot %d: #%u joined%s, %.0f m off, top speed %.0f m/s against the leader's %.0f", slot + 1,
                  w.handle, named ? " (named by the mission)" : "", (double)dist, (double)s_maxSpeed(w.obj, nullptr),
                  (double)leaderMax);
   mark(w, s_player.leader);
   queue_event(LuaEventId::WingmanJoin, w.pilot, obj);
}

// The leader's free slots, in order, each to the nearest friendly AI flyer its
// class lets follow and the slot lets in (a WingmanSlot line may keep it for some
// classes), however far off, until it has as many as it calls up on its own. An
// AI squadron's wingmen may be called away from it; its leaders not; nor a
// flyer whose class says Wingman = 0, nor one a script let go.
void fill(uint8_t* leader, double now)
{
   Formation& f = s_player;
   const ClassKeys* keys = keys_of(leader);
   const int cap = capacity(f, keys);
   int total = flyer_wingmen::flight_size(s_scriptSize, keys, g_flyerWingmenCount);
   if (total > cap) total = cap;   // a map shape or WingmanSlot lines hold no more
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
      if (!flyer_wingmen::may_follow(keys_of(obj)) || excluded(handle_of(obj))) return;
      const float* m = matrix_of(obj);
      if (!m) return;
      const float d = flyer_wingmen::length(row(m, 3) - lp);
      if (!std::isfinite(d)) return;
      const uint32_t cls = class_id(obj);
      if (!flyer_wingmen::accepts(keys, cls, flyer_wingmen::can_keep_up(s_maxSpeed(obj, nullptr), leaderMax))) return;
      found = add_candidate(pool, found, {obj, uc, cls, -d, d});
   });
   bool used[kMaxCandidates] = {};
   const ClassKeys* const own = slot_keys(f, keys);
   for (int slot = 0; slot < cap && f.count < total; ++slot) {
      if (slot_taken(f, slot)) continue;
      for (int c = 0; c < found; ++c) {
         if (used[c] || !flyer_wingmen::slot_accepts(own, slot, pool[c].cls)) continue;
         used[c] = true;
         enlist(pool[c].obj, pool[c].uc, slot, pool[c].dist, leaderMax, now, false);
         break;
      }
   }
}

// ---- The flyers a mission script names (AddWingman) --------------------------------------
// A world object by its name, as the game's own Lua functions find one
// (EntityEx::mIdMap; a spawner's flyer carries the spawner's name), or null.
uint8_t* object_named(uint32_t name)
{
   if (!s_findName || !s_idMap || !name) return nullptr;
   uint8_t* const obj = s_findName(s_idMap, kIdMapSlots, name);
   return obj && is_rtti(obj, kRttiGameObject) ? obj : nullptr;
}

// What a roster entry names now: whatever carries its name, or its flyer while
// that is still the one it was; null for none, or no flyer.
uint8_t* roster_object(const RosterEntry& e)
{
   uint8_t* const obj = e.name ? object_named(e.name) : e.obj && handle_of(e.obj) == e.handle ? e.obj : nullptr;
   return obj && is_rtti(obj, kRttiFlyer) ? obj : nullptr;
}

// The flyers a script named join the player's flight before any called up on
// their own, whatever their classes' keys say: each, once it is a friendly AI
// flyer in the air and not in it, in the first free slot that takes its class
// (else any), flying in from wherever it is. One named as a flyer is dropped from
// the roster once that flyer is gone; one named by name waits for whatever
// carries the name next.
void join_roster(uint8_t* leader, double now)
{
   Formation& f = s_player;
   uint8_t* want[flyer_wingmen::kMaxWingmen] = {};
   int wanted = 0;
   for (int r = s_rosterCount - 1; r >= 0; --r) {
      const RosterEntry& e = s_roster[r];
      if (!e.name && !(e.obj && handle_of(e.obj) == e.handle)) {
         s_roster[r] = s_roster[--s_rosterCount];   // its flyer is gone
         s_roster[s_rosterCount] = RosterEntry{};
         continue;
      }
      uint8_t* const obj = roster_object(e);
      Formation* in = nullptr;
      if (obj && obj != leader && !(find(obj, &in) && in == &f)) want[wanted++] = obj;
   }
   if (!wanted) return;
   const ClassKeys* keys = keys_of(leader);
   const float* lm = matrix_of(leader);
   if (!lm) return;
   const V3 lp = row(lm, 3);
   const float leaderMax = s_maxSpeed(leader, nullptr);
   const int team = team_of(leader);
   for_each_ai_flyer([&](uint8_t* obj, uint8_t* uc) {
      int k = 0;
      while (k < wanted && want[k] != obj) ++k;
      if (k == wanted || team_of(obj) != team) return;
      const int slot = free_slot(f, keys, class_id(obj), true);
      if (slot < 0) return;
      if (Formation* led = led_by(obj)) dissolve(*led, "its leader was named to the player's flight");
      const float* m = matrix_of(obj);
      const float d = m ? flyer_wingmen::length(row(m, 3) - lp) : 0.0f;
      enlist(obj, uc, slot, std::isfinite(d) ? d : 0.0f, leaderMax, now, true);
   });
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
constexpr Line kSayBreakOff   = {pbl_hash("game.wingmen.breakoff"), L"Wingmen: break off"};
constexpr Line kSayJoin       = {pbl_hash("game.wingmen.join"), L"Wingmen: one joins you"};

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
   w.sees = true;
   w.seenAt = -1e9;
   w.sky = true;
   w.open = V3{0.0f, 1.0f, 0.0f};
   w.openAt = -1e9;
   w.hasVia = false;
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
      if (!f.quiet) say(down ? kSayTargetDown : kSayTargetLost);
      if (order_log())
         install_log("[Wingmen] the attack's target is %s: back to %s", down ? "down" : "out of reach",
                     order_name(f.resume == Order::Attack ? Order::FormUp : f.resume));
      f.attackTarget = nullptr;
      f.attackHandle = 0;
      f.order = f.resume == Order::Attack ? Order::FormUp : f.resume;
      queue_event(LuaEventId::WingmenOrder, player_index(), nullptr);
      for (int i = 0; i < f.count; ++i) clear_target(f.wing[i], leader, now, nullptr);
      if (f.order == Order::FormUp) return;
   }
   // Engage or cover: the foes, then each wingman's. How far engaging reaches is
   // the leader class's WingmanEngageRange where it gives one.
   const bool cover = f.order == Order::Cover;
   const float engage = flyer_orders::engage_range(keys_of(leader));
   const float range = cover ? flyer_orders::kCoverRange : engage;
   const float keep = cover ? flyer_orders::kCoverKeep : flyer_orders::engage_keep(engage);
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
   // The rest, in slot order, each to its cheapest foe by its class's
   // WingmanRole; one still flying in waits.
   for (int slot = 0; slot < flyer_wingmen::kMaxWingmen; ++slot)
      for (int i = 0; i < f.count; ++i) {
         Wingman& w = f.wing[i];
         if (w.slot != slot || w.target || w.joining) continue;
         const int k = flyer_orders::pick_foe(position_of(w.obj), foes, n, flyer_orders::role_of(keys_of(w.obj)));
         if (k < 0) break;
         chase(w, foeObj[k], now);
         ++foes[k].taken;
      }
}

// The flight's order, from a key (the caller puts its line on the HUD) or a
// mission script (`quiet`: no line, nor one when its attack ends). The script
// hears either (OnWingmenOrder, with an attack's target).
void set_order(Formation& f, Order o, double now, bool quiet = false)
{
   if (o != Order::Attack) {
      f.attackTarget = nullptr;
      f.attackHandle = 0;
   }
   f.order = o;
   f.quiet = quiet;
   if (order_log())
      install_log("[Wingmen] order: %s, %d wingmen%s", order_name(o), f.count, quiet ? " (the mission's)" : "");
   queue_event(LuaEventId::WingmenOrder, player_index(), o == Order::Attack ? f.attackTarget : nullptr);
   // Landing (or on the ground) it waits for the next tick in the air.
   if (f.leader && flying(f.leader) && !f.landing) assign_targets(f, f.leader, now);
}

// Whether anything is in the way from one point to another (the same ray, the
// same types); how far along the first thing is.
bool blocked(V3 from, V3 to, uint8_t* ignoreA, uint8_t* ignoreB, float* along = nullptr, int types = kGroundMask)
{
   if (!s_rayHit) return false;
   V3 dir = to - from;
   const float length = flyer_wingmen::length(dir);
   if (!(length > 0.01f) || !flyer_wingmen::normalise(dir)) return false;
   const float start[3] = {from.x, from.y, from.z};
   const float d[3] = {dir.x, dir.y, dir.z};
   void* hit = nullptr;
   float normal[3] = {0.0f, 1.0f, 0.0f};
   uint8_t* ignore[2] = {ignoreA, ignoreB};
   const float fraction = s_rayHit(start, d, length, &hit, normal, ignore, ignoreB ? 2 : 1, types, 0);
   if (!(fraction >= 0.0f && fraction < 1.0f)) return false;
   if (along) *along = fraction * length;
   return true;
}

// Whether a wingman sees its target: nothing solid (the landing ray's types:
// buildings, props, terrain, water; not vehicles) on the line to it, looked
// along to kSightShort short of it.
bool sees_target(uint8_t* flyer, uint8_t* t)
{
   const V3 me = position_of(flyer), at = position_of(t);
   V3 dir = at - me;
   const float d = flyer_wingmen::length(dir);
   if (!(d > flyer_orders::kSightShort) || !flyer_wingmen::normalise(dir)) return true;
   return !blocked(me, at - dir * flyer_orders::kSightShort, flyer, t);
}

// A chasing wingman's AI aims and fires at its target: its AttackHelper is
// set to it (as UnitFlyAgent::IsNearATAT forces a walker on its crew), no
// more than every kForceEvery, and only while it aims at something else; but
// only while it sees it (sees_target, looked at every kSightEvery). Without the
// line, the AttackHelper's target is let go (AttackHelper::Reset, as it is
// before every new target: ForceSetAttackHelperTarget cannot take none, it reads
// the target's position), so it holds its fire at that one and takes on what
// its own sight finds.
void force_target(Wingman& w, double now)
{
   if (!w.target || !valid_target(w.target, w.targetHandle)) return;
   if (now - w.seenAt >= kSightEvery) {
      w.sees = sees_target(w.obj, w.target);
      w.seenAt = now;
   }
   if (!w.sees) {
      if (s_attackReset && attack_target_of(w.uc) == w.target)
         if (uint8_t* const agent = *(uint8_t**)(w.uc + kUC_Agent)) s_attackReset(agent + kAgent_AttackTarget);
      return;
   }
   if (!s_forceTarget || now - w.forcedAt < kForceEvery || attack_target_of(w.uc) == w.target) return;
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
      if (!flyer_wingmen::may_follow(keys_of(obj))) return;   // Wingman = 0
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
      for (int i = f.count - 1; i >= 0; --i) {
         const char* code = nullptr;
         if (const char* why = lost(f.wing[i], f, now, &code)) drop(f, i, why);
      }
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

// What is under a wingman, kLandReach down at most (flyer_orders_core.hpp,
// land_here): how far the ground is (the landing ray's types; infinity for
// none), its height and how far up it faces; and how far a flyer or vehicle is
// (COLL_RIGID, as EntityFlyer and the other vehicles give GameObject; the
// landing ray goes through them; infinity for none).
constexpr int kVehicleTypes = 0x8;

void look_down(uint8_t* self, float& below, float& hitY, float& normalY, float& onto)
{
   const V3 at = position_of(self);
   below = onto = std::numeric_limits<float>::infinity();
   hitY = at.y;
   normalY = 1.0f;
   float y = 0.0f, up = 0.0f;
   if (ground_at(at, flyer_orders::kLandReach, self, nullptr, y, up)) {
      below = at.y - y;
      hitY = y;
      normalY = up;
   }
   float along = 0.0f;
   if (blocked(at, at - V3{0.0f, flyer_orders::kLandReach, 0.0f}, self, nullptr, &along, kVehicleTypes)) onto = along;
}

// Whether something is over a point within kLandProbe: a roof (a hangar's).
bool roofed(V3 at, uint8_t* ignoreA, uint8_t* ignoreB, float* height = nullptr)
{
   float along = 0.0f;
   if (!blocked(at, at + V3{0.0f, flyer_orders::kLandProbe, 0.0f}, ignoreA, ignoreB, &along)) return false;
   if (height) *height = along;
   return true;
}

// Whether a flyer or vehicle stands on a place (a ship parked in a hangar, the
// player's own): rays down from `top` onto it at vehicles alone (the ground's
// rays go through them), at its centre and a third of kLandSpacing to each side.
bool occupied(V3 spot, float top, uint8_t* self)
{
   const float d = flyer_orders::kLandSpacing / 3.0f;
   const V3 offsets[5] = {V3{0.0f, 0.0f, 0.0f}, V3{d, 0.0f, 0.0f}, V3{-d, 0.0f, 0.0f}, V3{0.0f, 0.0f, d},
                          V3{0.0f, 0.0f, -d}};
   for (const V3& o : offsets) {
      const V3 at = spot + o;
      if (blocked(V3{at.x, top, at.z}, V3{at.x, spot.y - 1.0f, at.z}, self, nullptr, nullptr, kVehicleTypes))
         return true;
   }
   return false;
}

// ---- The leader's trail (flyer_trail_core.hpp, FlyerWingmenTrail) --------------------
// The player's flyer's path, recorded from every flyer's update (the leader's own
// need not come through UpdateIndirect), with the roof over it looked for every
// kRoofEvery and its flight's slots closed up beneath one (Formation::squeeze).
constexpr double kRoofEvery    = 0.5;     // s
constexpr float  kTrailReached = 10.0f;   // m: the point it steers at, passed
flyer_trail::Trail s_trail;
uint32_t s_trailOf = 0;      // the leader whose path it is (its handle)
double   s_squeezedAt = 0.0;

bool trail_live(const Formation& f)
{
   return &f == &s_player && g_flyerWingmenTrail && f.leader && s_trailOf == f.leaderHandle && s_trail.count > 0;
}

void update_trail(double now)
{
   uint8_t* const leader = s_player.leader;
   if (!g_flyerWingmenTrail || !leader || handle_of(leader) != s_player.leaderHandle || !alive(leader)) return;
   if (s_trailOf != s_player.leaderHandle) {
      flyer_trail::clear(s_trail);
      s_trailOf = s_player.leaderHandle;
      s_player.roofed = false;
      s_player.roofAt = -1e9;
      s_player.squeeze = 1.0f;
   }
   const V3 at = position_of(leader);
   flyer_trail::record(s_trail, at);
   if (now - s_player.roofAt >= kRoofEvery) {
      s_player.roofAt = now;
      const bool roof = roofed(at, leader, nullptr);
      if (roof != s_player.roofed && s_player.count > 0 && order_log())
         install_log("[Wingmen] you fly %s a roof: your wingmen's slots %s", roof ? "under" : "out from under",
                     roof ? "close up" : "open out again");
      s_player.roofed = roof;
      // And over each of its wingmen (no_lift), one on its way out counting the
      // looks it has been out from under it (out_clear).
      for (int i = 0; i < s_player.count; ++i) {
         Wingman& w = s_player.wing[i];
         w.roofed = handle_of(w.obj) == w.handle && alive(w.obj) && roofed(position_of(w.obj), w.obj, nullptr);
         if (w.land == kLandOutward) flyer_trail::out_clear(w.outLooks, w.roofed);
      }
   }
   s_player.squeeze = flyer_trail::squeeze(s_player.squeeze, s_player.roofed, (float)(now - s_squeezedAt));
   s_squeezedAt = now;
}

// A wingman's place in its flight's line, 0 first: those in slots before its own.
int rank_of(const Formation& f, const Wingman& w)
{
   int rank = 0;
   for (int j = 0; j < f.count; ++j)
      if (f.wing[j].slot < w.slot) ++rank;
   return rank;
}

// Whether a wingman could get to its leader along the trail: on it, or with a
// point within kNear in a clear line.
bool reaches_by_trail(const Formation& f, const Wingman& w)
{
   if (!trail_live(f)) return false;
   if (w.onTrail && flyer_trail::has(s_trail, w.trailAt)) return true;
   const V3 me = position_of(w.obj);
   float d = 0.0f;
   const int at = flyer_trail::nearest(s_trail, me, -1, &d);
   return at >= 0 && flyer_trail::takes(true, d) && !blocked(me, flyer_trail::point(s_trail, at), w.obj, f.leader);
}

// A wingman of the player's with a wall between it and `goal` (its slot, or the
// place it lands at) flies its leader's trail, looked at every kLookEvery: true
// while it does, with the point it steers at in `out` and how far behind its
// place in the line it is in `behind`. Stuck on it, it is left to its own AI for
// kWaitFor (steer_before, Wingman::waitUntil).
bool follow_trail(uint8_t* flyer, const Formation& f, Wingman& w, V3 goal, double now, V3& out, float& behind)
{
   if (!trail_live(f)) {
      w.onTrail = false;
      return false;
   }
   uint8_t* const leader = f.leader;
   const V3 me = position_of(flyer);
   if (now - w.trailLookAt >= flyer_trail::kLookEvery) {
      w.trailLookAt = now;
      const bool shut = blocked(me, goal, flyer, leader);
      w.shut = shut;
      w.shutAt = now;
      if (w.onTrail && !flyer_trail::has(s_trail, w.trailAt)) w.onTrail = false;   // the trail started again
      if (w.onTrail) {
         if (flyer_trail::leaves(w.clearLooks, !shut)) {
            w.onTrail = false;
            if (order_log())
               install_log("[Wingmen] slot %d (#%u): its way clear again, off your trail", w.slot + 1, w.handle);
         }
      } else if (shut) {
         float d = 0.0f;
         const int at = flyer_trail::nearest(s_trail, me, -1, &d);
         if (at >= 0 && flyer_trail::takes(true, d) && !blocked(me, flyer_trail::point(s_trail, at), flyer, leader)) {
            w.onTrail = true;
            w.trailAt = at;
            w.trailTo = flyer_trail::next_of(s_trail, at);
            w.clearLooks = 0;
            w.gainedAt = now;
            if (order_log())
               install_log("[Wingmen] slot %d (#%u): a wall between it and %s: it flies your trail, from %.0f m off",
                           w.slot + 1, w.handle, f.landing ? "its place" : "its slot", (double)d);
         }
      }
      if (w.onTrail) {
         // How far along it has got, and the furthest point ahead it sees.
         const int at = flyer_trail::nearest(s_trail, me, w.trailAt);
         if (at > w.trailAt) {
            w.trailAt = at;
            w.gainedAt = now;
         }
         int cuts[flyer_trail::kMaxCuts];
         const int n = flyer_trail::cut_candidates(s_trail, w.trailAt, cuts);
         w.trailTo = flyer_trail::next_of(s_trail, w.trailAt);
         for (int k = 0; k < n; ++k)
            if (!blocked(me, flyer_trail::point(s_trail, cuts[k]), flyer, leader)) {
               w.trailTo = cuts[k];
               break;
            }
      }
   }
   if (!w.onTrail) return false;
   const float gap = flyer_wingmen::clamp_spacing(g_flyerWingmenSpacing);
   behind = flyer_trail::behind_place(s_trail, flyer_trail::leader_along(s_trail, position_of(leader)), rank_of(f, w),
                                      gap, w.trailAt);
   if (flyer_trail::stuck(now - w.gainedAt, behind, gap)) {
      w.onTrail = false;
      w.waitUntil = now + flyer_trail::kWaitFor;
      if (order_log())
         install_log("[Wingmen] slot %d (#%u): no way along your trail for %.0f s: its own AI flies it for %.0f s",
                     w.slot + 1, w.handle, flyer_trail::kStuckFor, flyer_trail::kWaitFor);
      return false;
   }
   // Past the point it steers at between looks: on to the next, which the leader
   // flew to from it.
   const float* wm = matrix_of(flyer);
   const V3 nose = wm ? row(wm, 2) : V3{0.0f, 0.0f, 1.0f};
   if (!flyer_trail::has(s_trail, w.trailTo) || w.trailTo <= w.trailAt) w.trailTo = flyer_trail::next_of(s_trail, w.trailAt);
   for (int k = 0; k < 8 && w.trailTo < flyer_trail::newest(s_trail); ++k) {
      const V3 to = flyer_trail::point(s_trail, w.trailTo) - me;
      if (flyer_wingmen::length(to) > kTrailReached && flyer_wingmen::dot(to, nose) > 0.0f) break;
      w.trailTo = flyer_trail::next_of(s_trail, w.trailTo);
   }
   out = flyer_trail::point(s_trail, w.trailTo);
   return true;
}

// One let go under a roof (kLandOutward, head_out) flies back out the way the
// leader flew in: along its trail backwards (flyer_trail_core.hpp, the way out),
// at the furthest of the points kCuts back it has a clear line to, looked at
// every kLookEvery. True while it can, with the point it steers at in `out`.
bool follow_trail_out(uint8_t* flyer, const Formation& f, Wingman& w, double now, V3& out)
{
   if (!trail_live(f)) return false;
   uint8_t* const leader = f.leader;
   const V3 me = position_of(flyer);
   if (now - w.trailLookAt >= flyer_trail::kLookEvery) {
      w.trailLookAt = now;
      float d = 0.0f;
      const int at = flyer_trail::nearest_back(s_trail, me, w.onTrail ? w.trailAt : -1, &d);
      if (!w.onTrail) {
         if (at < 0 || !flyer_trail::takes(true, d) || blocked(me, flyer_trail::point(s_trail, at), flyer, leader))
            return false;
         w.onTrail = true;
         w.trailAt = at;
         w.gainedAt = now;
      } else if (at >= 0 && at < w.trailAt) {
         w.trailAt = at;
         w.gainedAt = now;
      }
      int cuts[flyer_trail::kMaxCuts];
      const int n = flyer_trail::back_candidates(s_trail, w.trailAt, cuts);
      w.trailTo = flyer_trail::prev_of(s_trail, w.trailAt);
      for (int k = 0; k < n; ++k)
         if (!blocked(me, flyer_trail::point(s_trail, cuts[k]), flyer, leader)) {
            w.trailTo = cuts[k];
            break;
         }
   }
   if (!w.onTrail || !flyer_trail::has(s_trail, w.trailAt)) return false;
   // Past the point it steers at between looks: on to the one before it, which
   // the leader flew from to it.
   const float* wm = matrix_of(flyer);
   const V3 nose = wm ? row(wm, 2) : V3{0.0f, 0.0f, 1.0f};
   if (!flyer_trail::has(s_trail, w.trailTo) || w.trailTo >= w.trailAt) w.trailTo = flyer_trail::prev_of(s_trail, w.trailAt);
   for (int k = 0; k < 8 && w.trailTo > s_trail.first; ++k) {
      const V3 to = flyer_trail::point(s_trail, w.trailTo) - me;
      if (flyer_wingmen::length(to) > kTrailReached && flyer_wingmen::dot(to, nose) > 0.0f) break;
      w.trailTo = flyer_trail::prev_of(s_trail, w.trailTo);
   }
   out = flyer_trail::point(s_trail, w.trailTo);
   return true;
}

// The game's lift off for one of the player's wingmen, as on the trail
// (flyer_trail_core.hpp, lift_off): under a roof itself, or with its leader under
// one and its line clear as follow_trail last looked. FlyerWingmenTrail's, with
// the roofs it looks for.
bool no_lift(const Formation& f, const Wingman& w, double now)
{
   return &f == &s_player && g_flyerWingmenTrail && flyer_trail::lift_off(w.roofed, f.roofed, w.shut, now - w.shutAt);
}

// A wingman's place beside the landing leader (flyer_orders_core.hpp,
// land_tries), else on ahead the way the leader faces (forward_tries), none past
// the first wall: the first that is spaced from the places already `taken`, on
// ground level with the leader's and flat, probed from under any roof over the
// leader, with no flyer or vehicle on it (occupied), and, under a roof, in a clear
// line from the leader and on a clear way from the wingman, straight or along the
// leader's trail (reaches_by_trail). Which try it was (from kLandTries on, ahead),
// or -1 for none.
int land_place(const Formation& f, uint8_t* leader, const Wingman& w, int rank, const V3* taken, int nTaken, V3& out)
{
   V3 tries[flyer_orders::kLandTries + flyer_orders::kLandAhead];
   const V3 slot = slot_of(f, w.slot);
   const int beside = flyer_orders::land_tries(slot, rank, tries);
   const int n = beside + flyer_orders::forward_tries(slot, rank, tries + beside);
   const bool roof = f.padRoof < flyer_orders::kNoRoof;
   const float top = f.padGround + flyer_orders::probe_height(f.padRoof);
   const V3 low{f.pad.x, f.padGround + flyer_orders::kLandClear, f.pad.z};
   V3 ahead = low;   // on ahead: as far as the line has gone, from the leader's
   for (int k = 0; k < n; ++k) {
      V3 spot = f.padFrame.valid ? flyer_wingmen::to_world(f.padFrame, f.pad, tries[k]) : f.pad + tries[k];
      spot.y = f.padGround;
      if (k >= beside) {   // on ahead: none past a wall
         const V3 next{spot.x, low.y, spot.z};
         if (blocked(ahead, next, leader, w.obj)) break;
         ahead = next;
      }
      if (!flyer_orders::spaced(spot, taken, nTaken)) continue;
      if (roof && blocked(low, V3{spot.x, low.y, spot.z}, leader, w.obj)) continue;   // a wall between
      float hy = 0.0f, hup = 0.0f;
      if (!ground_at(V3{spot.x, top, spot.z}, top - f.padGround + 2.0f * flyer_orders::kLandLevel, leader, w.obj, hy,
                     hup) ||
          !flyer_orders::level_spot(hy, hup, f.padGround))
         continue;
      spot.y = hy;
      if (occupied(spot, top, w.obj)) continue;   // a ship parked on it
      if (roof &&
          blocked(position_of(w.obj), spot + V3{0.0f, flyer_orders::hover_height(f.padRoof), 0.0f}, leader, w.obj) &&
          !reaches_by_trail(f, w))
         continue;   // no clear way to it, nor along the leader's trail
      out = spot;
      return k;
   }
   return -1;
}

// Where a place is, for the log: beside the leader or on ahead.
const char* place_said(int k) { return k < flyer_orders::kLandTries ? "beside you" : "on ahead"; }

// Under a roof, where one with no place circles (flyer_orders_core.hpp,
// roof_loiter_radius): low, at its hover height, round the point with most room
// every way, of the one over the leader and those on ahead along its line before
// a wall. False with no room anywhere.
bool roof_circle(const Formation& f, const Wingman& w, V3& center, float& radius)
{
   constexpr int kTypes = kGroundMask | kVehicleTypes;
   constexpr float kDiag = 0.70710678f;
   const float reach = flyer_orders::kRoofLoiterMax + flyer_orders::kRoofLoiterRoom;
   const V3 fwd = f.padFrame.valid ? f.padFrame.fwd : V3{0.0f, 0.0f, 1.0f};
   const V3 right = f.padFrame.valid ? f.padFrame.right : V3{1.0f, 0.0f, 0.0f};
   const V3 ways[8] = {fwd, fwd * -1.0f, right, right * -1.0f, (fwd + right) * kDiag, (fwd - right) * kDiag,
                       (right - fwd) * kDiag, (fwd + right) * -kDiag};
   V3 at{f.pad.x, f.padGround + flyer_orders::hover_height(f.padRoof), f.pad.z};
   float best = -1.0f;
   for (int k = 0; k <= flyer_orders::kLandAhead; ++k) {
      if (k > 0) {
         const V3 next = at + fwd * flyer_orders::kLandSpacing;
         if (blocked(at, next, w.obj, nullptr, nullptr, kTypes)) break;   // a wall ahead
         at = next;
      }
      float room = reach;
      for (const V3& d : ways) {
         float along = 0.0f;
         if (blocked(at, at + d * reach, w.obj, nullptr, &along, kTypes) && along < room) room = along;
      }
      if (room > best) {
         best = room;
         center = at;
      }
      if (best >= reach) break;   // all the room a circle wants
   }
   radius = flyer_orders::roof_loiter_radius(best);
   return radius > 0.0f;
}

// With no place free: it circles till one is, looking again from kLandLookAgain
// on (look_again); under a roof low, round `center`, `radius` round (roof_circle),
// in the open over its leader (radius 0).
void circle_till_free(Wingman& w, V3 center, float radius, double now)
{
   w.land = kLandCircle;
   w.circleAt = center;
   w.circleR = radius;
   w.lookAgainAt = now + flyer_orders::kLandLookAgain;
}

// The places the others hold, for wingman `i`'s next (land_place): the leader's
// own, those the others come down at and where those down are; and its turn
// among the flight in `rank` (those in slots before its own).
int taken_places(const Formation& f, int i, V3* taken, int& rank)
{
   int n = 0;
   rank = 0;
   taken[n++] = f.pad;
   for (int j = 0; j < f.count; ++j) {
      if (j == i) continue;
      const Wingman& o = f.wing[j];
      if (o.slot < f.wing[i].slot) ++rank;
      if (o.land == kLandComing) taken[n++] = o.spot;
      else if ((o.land == kLandDown || o.land == kLandOut) && handle_of(o.obj) == o.handle && alive(o.obj))
         taken[n++] = position_of(o.obj);
   }
   return n;
}

// One let go under a roof (no place and no room to circle, its landings failing,
// or its time with the player out over), where the game's own AI would most
// likely fly it into a wall: it flies back out along the leader's trail first
// (kLandOutward, follow_trail_out) and is let go once out from under the roof
// (flyer_trail_core.hpp, out_clear), or where it is when stuck or past kOutMax.
// The user: "can it fly back out the way it flew in, probably by [following] my
// path backwards? Lots of hangars are single entry/exit, so if they disband it's
// highly likely they'll crash unless they do this". With no trail it can take,
// or not flying, it is let go now.
void head_out(Formation& f, int i, double now, const char* why, const char* code)
{
   Wingman& w = f.wing[i];
   float d = 0.0f;
   const int at = trail_live(f) && handle_of(w.obj) == w.handle && alive(w.obj) && flying(w.obj)
                     ? flyer_trail::nearest_back(s_trail, position_of(w.obj), -1, &d)
                     : -1;
   if (at < 0 || !flyer_trail::takes(true, d)) {
      drop(f, i, why, code);
      return;
   }
   w.land = kLandOutward;
   w.outFrom = now;
   w.outLooks = 0;
   w.outCode = code;
   w.onTrail = false;
   w.clearLooks = 0;
   w.trailLookAt = -1e9;
   w.circleR = 0.0f;
   if (order_log())
      install_log("[Wingmen] slot %d (#%u): %s: it flies back out along your trail before it is let go", w.slot + 1,
                  w.handle, why);
}

// The leader comes down: each wingman, in slot order, gets a place beside it or
// on ahead (land_place). One with none circles till one is free: overhead, or
// under a roof low where there is room (roof_circle); with no room, it flies back
// out along the trail and is let go (head_out).
void start_landing(Formation& f, uint8_t* leader, double now)
{
   f.landing = true;
   f.padTeam = team_of(leader);
   f.outAt = 0.0;
   const float* lm = matrix_of(leader);
   if (!lm) return;
   f.pad = row(lm, 3);
   f.padFrame = flyer_wingmen::level_frame(row(lm, 2), row(lm, 0));
   float y = f.pad.y, up = 1.0f;
   f.padGround = ground_at(f.pad + V3{0.0f, 5.0f, 0.0f}, kGroundRay, leader, nullptr, y, up) ? y : f.pad.y;
   float roof = flyer_orders::kLandProbe;
   f.padRoof = roofed(V3{f.pad.x, f.padGround + 1.0f, f.pad.z}, leader, nullptr, &roof) ? roof + 1.0f
                                                                                       : flyer_orders::kNoRoof;
   int here = 0, beside = 0, over = 0, gone = 0;
   V3 taken[flyer_wingmen::kMaxWingmen];
   int nTaken = 0;
   bool letGo[flyer_wingmen::kMaxWingmen] = {};
   bool placed[flyer_wingmen::kMaxWingmen] = {};
   for (int rank = 0; rank < f.count; ++rank) {
      // The next in slot order.
      int i = -1;
      for (int j = 0; j < f.count; ++j)
         if (!placed[j] && (i < 0 || f.wing[j].slot < f.wing[i].slot)) i = j;
      placed[i] = true;
      Wingman& w = f.wing[i];
      clear_target(w, leader, now, nullptr);
      w.joining = false;
      w.holding = false;
      w.trick = -1;
      w.landTries = 0;
      // Near, over level ground with nothing on it: straight down where it is.
      const V3 at = position_of(w.obj);
      float below = 0.0f, hitY = 0.0f, normalY = 0.0f, onto = 0.0f;
      look_down(w.obj, below, hitY, normalY, onto);
      const bool nearLeader = flyer_wingmen::length(at - f.pad) <= flyer_orders::kLandNowRange;
      if (s_land && flyer_orders::land_here(nearLeader, below, hitY, normalY, f.padGround, onto)) {
         s_land(w.obj);
         w.land = kLandDown;
         w.spot = V3{at.x, hitY, at.z};
         w.landAt = now;
         taken[nTaken++] = w.spot;
         ++here;
         if (order_log())
            install_log("[Wingmen] slot %d (#%u) lands where it is, %.0f m from you, %.0f m up", w.slot + 1, w.handle,
                        (double)flyer_orders::across(at, f.pad), (double)below);
         continue;
      }
      V3 spot = f.pad;
      const int k = land_place(f, leader, w, rank, taken, nTaken, spot);
      if (k >= 0) {
         w.spot = spot;
         w.land = kLandComing;
         taken[nTaken++] = spot;
         ++beside;
         if (order_log())
            install_log("[Wingmen] slot %d (#%u) lands %s %.0f m off (place %d)", w.slot + 1, w.handle,
                        place_said(k), (double)flyer_orders::across(spot, f.pad), k + 1);
      } else if (f.padRoof < flyer_orders::kNoRoof) {
         // Under a roof, where circling overhead would take it into the roof: low,
         // where there is room, till a place is free; with no room, let go.
         V3 c;
         float r = 0.0f;
         if (roof_circle(f, w, c, r)) {
            circle_till_free(w, c, r, now);
            ++over;
            if (order_log())
               install_log("[Wingmen] slot %d (#%u): no clear place free: it circles %.0f m round, low under the roof, "
                           "till one is", w.slot + 1, w.handle, (double)r);
         } else {
            letGo[i] = true;
            ++gone;
         }
      } else {
         w.spot = f.pad;
         circle_till_free(w, V3{}, 0.0f, now);
         ++over;
      }
   }
   for (int i = f.count - 1; i >= 0; --i)
      if (letGo[i]) head_out(f, i, now, "no clear place, and no room to circle under the roof", "noroom");
   if (order_log())
      install_log("[Wingmen] the leader comes down%s: %d land where they are, %d fly to a place, %d circle till one "
                  "is free, %d let go (no room under the roof)",
                  f.padRoof < flyer_orders::kNoRoof ? " under a roof" : "", here, beside, over, gone);
}

// A wingman whose landing was interrupted, up again: a clear place beside its
// leader or on ahead (land_place, away from the others' places and from where
// they are), else it circles till one is free (circle_till_free; under a roof
// low, where there is room, or with none it flies back out and is let go,
// head_out). One interrupted past kLandRetries lands no more: in the open it
// circles, under a roof it flies back out and is let go (the user: "If they
// cannot land reliably and don't crash, they should just find a suitable spot or
// disband").
void re_place(Formation& f, uint8_t* leader, int i, double now)
{
   Wingman& w = f.wing[i];
   V3 taken[flyer_wingmen::kMaxWingmen + 1];
   int rank = 0;
   const int nTaken = taken_places(f, i, taken, rank);
   const bool roof = f.padRoof < flyer_orders::kNoRoof;
   const bool done = w.landTries > flyer_orders::kLandRetries;
   V3 spot = f.pad;
   const int k = done ? -1 : land_place(f, leader, w, rank, taken, nTaken, spot);
   V3 c;
   float r = 0.0f;
   if (k >= 0) {
      w.spot = spot;
      w.land = kLandComing;
      w.circleR = 0.0f;
      if (order_log())
         install_log("[Wingmen] slot %d (#%u) lands at a clear place %s, %.0f m from you (place %d)", w.slot + 1,
                     w.handle, place_said(k), (double)flyer_orders::across(spot, f.pad), k + 1);
   } else if (roof && !done && roof_circle(f, w, c, r)) {
      circle_till_free(w, c, r, now);
      if (order_log())
         install_log("[Wingmen] slot %d (#%u): no clear place free: it circles %.0f m round, low under the roof, till "
                     "one is", w.slot + 1, w.handle, (double)r);
   } else if (roof) {
      head_out(f, i, now, done ? "it could not land under the roof" : "no clear place, and no room to circle under the roof",
               "noroom");
   } else {
      w.spot = f.pad;
      circle_till_free(w, V3{}, 0.0f, now);
      if (order_log())
         install_log("[Wingmen] slot %d (#%u): %s; it circles", w.slot + 1, w.handle,
                     done ? "it lands no more" : "nowhere clear to land");
   }
}

// One circling with no place (kLandCircle), every kLandLookAgain: a place free
// now, it comes down there (the user: "circle until a free spot appears"). Not
// one interrupted past kLandRetries: it lands no more.
void look_again(Formation& f, uint8_t* leader, int i, double now)
{
   Wingman& w = f.wing[i];
   w.lookAgainAt = now + flyer_orders::kLandLookAgain;
   if (w.landTries > flyer_orders::kLandRetries) return;
   V3 taken[flyer_wingmen::kMaxWingmen + 1];
   int rank = 0;
   const int nTaken = taken_places(f, i, taken, rank);
   V3 spot = f.pad;
   const int k = land_place(f, leader, w, rank, taken, nTaken, spot);
   if (k < 0) return;
   w.spot = spot;
   w.land = kLandComing;
   w.circleR = 0.0f;
   if (order_log())
      install_log("[Wingmen] slot %d (#%u): a place is free now %s, %.0f m from you (place %d): it comes down there",
                  w.slot + 1, w.handle, place_said(k), (double)flyer_orders::across(spot, f.pad), k + 1);
}

// The leader takes off again (or did not land after all): so do they. The
// player's trail starts again where it took off, so the way out is the trail's,
// not the way in.
void end_landing(Formation& f)
{
   f.landing = false;
   if (&f == &s_player) flyer_trail::clear(s_trail);
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
      w.touchedDown = false;
      w.circleR = 0.0f;
   }
   if (order_log()) install_log("[Wingmen] the leader takes off: %d of its wingmen take off with it", leaving);
}

// Once a second: the leader's state, and each wingman's.
void update_landing(Formation& f, uint8_t* leader)
{
   const double now = seconds();
   const int ls = state_of(leader);
   const bool down = ls == kStateLanding || ls == kStateLanded;
   if (down && !f.landing) start_landing(f, leader, seconds());
   else if (!down && f.landing) end_landing(f);
   if (f.landing) {
      // Interrupted (flyer_orders_core.hpp, landing_interrupted): coming down onto a
      // flyer or vehicle, still not down, or down on top of one. It takes off again
      // and, up, is given a clear place (re_place).
      for (int i = 0; i < f.count; ++i) {
         Wingman& w = f.wing[i];
         if (w.land == kLandSeek) {
            if (flying(w.obj)) re_place(f, leader, i, now);
            continue;
         }
         // Circling with no place: one free yet (look_again)?
         if (w.land == kLandCircle) {
            if (flying(w.obj) && now >= w.lookAgainAt) look_again(f, leader, i, now);
            continue;
         }
         // On its way out from under the roof (head_out): out, it is let go;
         // stuck, never on the trail or out too long, let go where it is.
         if (w.land == kLandOutward) {
            const char* why = nullptr;
            if (w.outLooks >= flyer_trail::kOutLooks) why = "flown back out from under the roof";
            else if (now - w.outFrom > flyer_trail::kOutMax) why = "still under the roof on its way out";
            else if (!w.onTrail && now - w.outFrom > flyer_trail::kOutJoin) why = "no way out along your trail";
            else if (w.onTrail && now - w.gainedAt > flyer_trail::kStuckFor) why = "stuck on its way out";
            if (why) drop(f, i, why, w.outCode);
            continue;
         }
         if (w.land != kLandDown || !s_takeOff) continue;
         const int st = state_of(w.obj);
         float below = 0.0f, hitY = 0.0f, normalY = 0.0f, onto = 0.0f;
         look_down(w.obj, below, hitY, normalY, onto);
         const flyer_orders::Interrupt why =
            flyer_orders::landing_interrupted(st == kStateLanding, st == kStateLanded, now - w.landAt, below, onto);
         if (why == flyer_orders::Interrupt::None) continue;
         if (order_log()) {
            const char* what = why == flyer_orders::Interrupt::Onto    ? "was coming down onto a flyer or vehicle"
                               : why == flyer_orders::Interrupt::Stuck ? "has not touched down"
                                                                       : "landed on top of a flyer or vehicle";
            if (w.landTries < flyer_orders::kLandRetries)
               install_log("[Wingmen] slot %d (#%u) %s: it looks for a clear place (try %d of %d)", w.slot + 1,
                           w.handle, what, w.landTries + 1, flyer_orders::kLandRetries);
            else
               install_log("[Wingmen] slot %d (#%u) %s, its last try: it lands no more", w.slot + 1, w.handle, what);
         }
         w.landTries++;
         s_takeOff(w.obj);
         w.land = kLandSeek;
         w.touchedDown = false;
      }
   }
   for (int i = 0; i < f.count; ++i) {
      Wingman& w = f.wing[i];
      if (w.land == kLandLeaving && flying(w.obj)) w.land = kLandNone;   // up again: to its slot
      // Up again before its leader (it should wait aboard): it circles till a
      // place is free, or under a roof, where that circle would take it into the
      // roof, it is given one again (re_place: a place, or low round where there
      // is room).
      if (w.land == kLandDown && flying(w.obj) && f.landing) {
         if (f.padRoof < flyer_orders::kNoRoof) w.land = kLandSeek;
         else circle_till_free(w, V3{}, 0.0f, now);
      }
      // Down beside its leader: the script hears it once (OnWingmanLanded).
      if (w.land == kLandDown && state_of(w.obj) == kStateLanded && !w.touchedDown) {
         w.touchedDown = true;
         queue_event(LuaEventId::WingmanLanded, w.pilot, w.obj);
      }
      // Down, it gets out (AILowLevel::SetExitVehicle, with no command post, as
      // the AI's own end-of-game exit asks), unless its class says
      // WingmanExitOnLand = 0: then it waits aboard for its leader.
      // Asked again every kExitAgain while its pilot is still aboard (its flyer
      // still flown by its controller; once out, it is let go, "landed"),
      // kExitTries in all.
      if (w.land == kLandDown && state_of(w.obj) == kStateLanded && s_exitVehicle &&
          flyer_wingmen::exits_on_land(keys_of(w.obj))) {
         s_exitVehicle(w.uc + kUC_LowLevel, nullptr);
         w.land = kLandOut;
         w.exitAt = now;
         w.exitTries = 1;
         if (order_log()) install_log("[Wingmen] slot %d (#%u) landed: its pilot gets out", w.slot + 1, w.handle);
      } else if (w.land == kLandOut && s_exitVehicle && state_of(w.obj) == kStateLanded &&
                 *(uint8_t* const*)(w.obj + kFly_Ctrl) == w.uc && now - w.exitAt >= kExitAgain &&
                 w.exitTries < kExitTries) {
         s_exitVehicle(w.uc + kUC_LowLevel, nullptr);
         w.exitAt = now;
         ++w.exitTries;
         if (order_log())
            install_log("[Wingmen] slot %d (#%u) still aboard: its pilot asked to get out again (%d)", w.slot + 1,
                        w.handle, w.exitTries);
      }
   }
}

// ---- Getting out with the leader gone -------------------------------------------------
// A wingman landed or landing beside a leader whose pilot got out (so the flight
// is let go) still gets its own pilot out: it settles, and is asked to get out
// once down, again every kExitAgain, kExitTries in all, for kSettleFor at most.
// Meanwhile its fly agent does not take it up again (agent_take_off_mt).
struct Settling {
   uint8_t* obj = nullptr;
   uint32_t handle = 0;
   uint8_t* uc = nullptr;
   double   since = 0.0;
   double   exitAt = -1e9;
   int      tries = 0;
};
constexpr int kMaxSettling = 2 * flyer_wingmen::kMaxWingmen;
Settling s_settling[kMaxSettling];
int      s_settlingCount = 0;

void settle(const Formation& f, double now)
{
   if (!s_exitVehicle) return;
   for (int i = 0; i < f.count; ++i) {
      const Wingman& w = f.wing[i];
      if (w.land != kLandDown && w.land != kLandOut) continue;
      if (handle_of(w.obj) != w.handle || *(uint8_t* const*)(w.obj + kFly_Ctrl) != w.uc) continue;
      if (!flyer_wingmen::exits_on_land(keys_of(w.obj))) continue;
      if (s_settlingCount >= kMaxSettling) return;
      Settling& e = s_settling[s_settlingCount++];
      e = Settling{w.obj, w.handle, w.uc, now, w.land == kLandOut ? w.exitAt : -1e9, w.exitTries};
      if (order_log())
         install_log("[Wingmen] slot %d (#%u) gets its pilot out once down, its leader gone", w.slot + 1, w.handle);
   }
}

bool settling(const uint8_t* obj)
{
   for (int i = 0; i < s_settlingCount; ++i)
      if (s_settling[i].obj == obj && handle_of(obj) == s_settling[i].handle) return true;
   return false;
}

void update_settling(double now)
{
   for (int i = s_settlingCount - 1; i >= 0; --i) {
      Settling& e = s_settling[i];
      const bool gone = handle_of(e.obj) != e.handle || !alive(e.obj);
      const bool out = !gone && *(uint8_t* const*)(e.obj + kFly_Ctrl) != e.uc;
      if (!gone && !out && now - e.since <= kSettleFor) {
         if (s_exitVehicle && state_of(e.obj) == kStateLanded && now - e.exitAt >= kExitAgain && e.tries < kExitTries) {
            s_exitVehicle(e.uc + kUC_LowLevel, nullptr);
            e.exitAt = now;
            ++e.tries;
         }
         continue;
      }
      if (order_log())
         install_log("[Wingmen] #%u %s", e.handle,
                     out ? "landed: its pilot got out" : gone ? "gone before its pilot got out"
                                                           : "still aboard: no longer asked to get out");
      s_settling[i] = s_settling[--s_settlingCount];
      s_settling[s_settlingCount] = Settling{};
   }
}

// Every update: one coming down near enough to its spot lands, as a node's
// LandOnArrival lands a flyer.
void maybe_land(Wingman& w)
{
   if (w.land != kLandComing || !flying(w.obj) || !flyer_orders::may_land(position_of(w.obj), w.spot)) return;
   s_land(w.obj);
   w.land = kLandDown;
   w.landAt = seconds();
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
   f.quiet = false;
   f.called = false;
   f.brokenOff = false;
   f.handPicked = false;
   f.landing = false;
   f.outAt = 0.0;
}

// Whether the player's flight is made up to its size now (tops_up).
bool topping_up(const ClassKeys* keys)
{
   return flyer_orders::tops_up(s_locked, s_player.brokenOff, s_player.handPicked, s_player.called, auto_now(keys));
}

// The order a flight starts with: the mission's standing order
// (SetWingmenOrder), else its class's WingmanStartOrder, else form up.
Order start_order_for(const uint8_t* leader)
{
   return s_scriptOrder >= 0 ? (Order)s_scriptOrder : flyer_orders::start_order(keys_of(leader));
}

const Line& line_for(Order o)
{
   return o == Order::Engage ? kSayEngage : o == Order::Cover ? kSayCover : o == Order::Attack ? kSayAttack : kSayFormUp;
}

// The wingmen's markers, each tick: off while a .hud draws its own, on while
// none does (a wingman that joined before the HUD was up included).
void update_marks()
{
   if (!s_player.leader) return;
   const bool drawn = hud_wingmen_bound();
   for (int i = 0; i < s_player.count; ++i) {
      Wingman& w = s_player.wing[i];
      if (drawn) unmark(w);
      else if (handle_of(w.obj) == w.handle && alive(w.obj)) mark(w, s_player.leader);
   }
}

// The attack target's marker (unmark_target), each tick: on the target the
// flight attacks, off as the attack ends.
void update_target_mark()
{
   uint8_t* want = nullptr;
   if (s_addMarker && g_flyerWingmenMarkers && s_player.leader && s_player.order == Order::Attack &&
       !hud_wingmen_target_bound()) {
      uint8_t* const t = s_player.attackTarget;
      if (t && handle_of(t) == s_player.attackHandle && alive(t)) want = t;
   }
   if (want == s_targetMark.obj && (!want || handle_of(want) == s_targetMark.handle)) return;
   unmark_target();
   if (!want) return;
   const int team = team_of(s_player.leader);
   if (team < 0) return;
   s_targetMark = TargetMark{want, handle_of(want), 1u << team};
   s_addMarker(want, nullptr, kMarkerIcon, kMarkerScale, s_targetMark.teams, kTargetMarkColor, 1, 0, 0, 0.0f);
}

// The player got out of its flyer, landed with its flight: the wingmen still on
// their way in come down beside it all the same, and their pilots get out (or
// wait aboard), as with the player aboard, for kArriveFor at most; then the rest
// are let go, under a roof those still flying once they have flown back out
// (head_out). The user: "I just don't see any late arriving wingmen" (they were
// let go as the player got out). Back aboard, the player leads them again; in
// another flyer, or with its own gone or flown off, they are let go. True while
// it goes on.
bool landing_without_you(double now)
{
   Formation& f = s_player;
   uint8_t* const parked = f.leader;
   if (!f.landing || f.count == 0 || !parked || handle_of(parked) != f.leaderHandle || !alive(parked) ||
       state_of(parked) != kStateLanded)
      return false;
   if (f.outAt <= 0.0) {
      f.outAt = now;
      if (order_log())
         install_log("[Wingmen] you got out: your %d wingmen land beside your flyer all the same, %.0f s at most",
                     f.count, flyer_orders::kArriveFor);
   }
   if (flyer_orders::arrival_over(now - f.outAt)) {
      // The rest are let go: those down get their pilots out (settle), and under
      // a roof those still flying fly back out along the trail first (head_out).
      settle(f, now);
      const bool roof = f.padRoof < flyer_orders::kNoRoof;
      for (int i = f.count - 1; i >= 0; --i) {
         const Wingman& w = f.wing[i];
         if (w.land == kLandOutward) continue;
         if (roof && handle_of(w.obj) == w.handle && alive(w.obj) && flying(w.obj))
            head_out(f, i, now, "60 s after you got out", "leader");
         else
            drop(f, i, "60 s after you got out", "leader");
      }
      if (f.count == 0) return false;
   }
   drop_gone(f);
   if (s_land) update_landing(f, parked);
   for (int i = f.count - 1; i >= 0; --i) {
      const char* code = nullptr;
      if (const char* why = lost(f.wing[i], f, now, &code)) drop(f, i, why, code);
   }
   if (f.count > 0) return true;
   if (order_log()) install_log("[Wingmen] your flight is down and out, with you out of your flyer");
   return false;
}

void tick(double now)
{
   update_settling(now);
   uint8_t* const leader = player_flyer();
   if (!leader && landing_without_you(now)) {
      // Its wingmen still landing beside the flyer the player got out of.
   } else if (!leader || !leads(leader)) {
      if (s_player.landing) settle(s_player, now);
      release_all(s_player, leader ? "the player's flyer leads none" : "the player is not flying", "leader");
      s_player.leader = nullptr;
      s_player.leaderHandle = 0;
      s_player.shapeCount = 0;
      s_player.shapeName = 0;
      s_player.vee = false;
      reset_orders(s_player);
   } else {
      if (leader != s_player.leader || handle_of(leader) != s_player.leaderHandle) {
         if (s_player.landing) settle(s_player, now);
         release_all(s_player, "a new leader", "leader");
         s_player.leader = leader;
         s_player.leaderHandle = handle_of(leader);
         reset_orders(s_player);
         s_player.order = start_order_for(leader);
         choose_player_shape(leader);
      }
      if (s_player.outAt > 0.0) {   // back aboard the flyer it got out of (landing_without_you)
         s_player.outAt = 0.0;
         if (order_log()) install_log("[Wingmen] you are back aboard: %d wingmen with you", s_player.count);
      }
      drop_gone(s_player);
      if (s_land && lands(leader)) update_landing(s_player, leader);
      for (int i = s_player.count - 1; i >= 0; --i) {
         const char* code = nullptr;
         if (const char* why = lost(s_player.wing[i], s_player, now, &code)) drop(s_player, i, why, code);
      }
      if (flying(leader) && !s_player.landing) {
         update_wingmen(s_player, leader, now);
         assign_targets(s_player, leader, now);
         // The flyers the mission names join first. Then losses are made up
         // whenever they happen, whatever the order: once the player has called
         // wingmen, or from takeoff with WingmanAuto; never in a flight the
         // mission locked, nor after the player broke one off, nor in one it
         // picked by hand (topping_up).
         const int before = s_player.count;
         join_roster(leader, now);
         const ClassKeys* keys = keys_of(leader);
         const int want = flyer_wingmen::flight_size(s_scriptSize, keys, g_flyerWingmenCount);
         if (topping_up(keys) && s_player.count < want) fill(leader, now);
         if (s_player.count > before && s_player.order != Order::FormUp) assign_targets(s_player, leader, now);
      }
      report(now);
   }
   update_marks();
   update_target_mark();
   tick_squadrons(now);
}

// The player leads a flight, in the air.
bool player_leads_now()
{
   return s_player.leader && handle_of(s_player.leader) == s_player.leaderHandle && alive(s_player.leader) &&
          flying(s_player.leader);
}

// The friendly AI flyer in the air under the player's reticule, and the
// controller flying it; null for none.
uint8_t* aimed_flyer(uint8_t* leader, uint8_t** uc)
{
   uint8_t* const ctl = leader + kFly_Controllable;
   for (int k = 0; k < kReticuleChannels; ++k) {
      uint8_t* const t = *(uint8_t**)(ctl + kCtl_Reticule + k * 8);
      if (!t || handle_of(t) != *(const uint32_t*)(ctl + kCtl_ReticuleHandle + k * 8)) continue;
      if (t == leader || !is_rtti(t, kRttiFlyer) || !alive(t) || !flying(t) || team_of(t) != team_of(leader)) continue;
      uint8_t* const c = *(uint8_t**)(t + kFly_Ctrl);
      if (!c || !is_rtti(c, kRttiUnitController)) continue;   // flown by a player
      *uc = c;
      return t;
   }
   return nullptr;
}

// The squad key with a flyer under the reticule (flyer_orders_core.hpp,
// pick_verdict): one the flight has room for, and that may fly with this leader
// as fill would take it, joins in its first free slot that takes its class (an
// AI squadron's leader leaves its squadron to it). Picked into an empty flight,
// the flight is the player's own pick: in form up, and not made up to its size.
flyer_orders::Pick pick(uint8_t* leader, double now)
{
   uint8_t* uc = nullptr;
   uint8_t* const t = aimed_flyer(leader, &uc);
   Formation* in = nullptr;
   const bool ours = t && find(t, &in) && in == &s_player;
   const ClassKeys* keys = keys_of(leader);
   const float leaderMax = s_maxSpeed(leader, nullptr);
   bool mayJoin = false, room = false;
   int slot = -1;
   if (t && !ours) {
      const uint32_t cls = class_id(t);
      mayJoin = flyer_wingmen::may_follow(keys_of(t)) && !excluded(handle_of(t)) &&
                flyer_wingmen::accepts(keys, cls, flyer_wingmen::can_keep_up(s_maxSpeed(t, nullptr), leaderMax));
      int total = flyer_wingmen::flight_size(s_scriptSize, keys, g_flyerWingmenCount);
      const int cap = capacity(s_player, keys);
      if (total > cap) total = cap;
      slot = free_slot(s_player, keys, cls, false);
      room = s_player.count < total && slot >= 0;
   }
   const flyer_orders::Pick v = flyer_orders::pick_verdict(t != nullptr, ours, s_locked, mayJoin, room);
   if (v == flyer_orders::Pick::Refuse)
      install_log("[Wingmen] squad key on #%u: %s", handle_of(t),
                  s_locked  ? "the mission locked your flight (SetWingmenLocked)"
                  : !mayJoin ? "it may not fly with you (its class or yours)"
                             : "your flight has no room for it");
   if (v != flyer_orders::Pick::Join) return v;
   if (Formation* led = led_by(t); led && led->count) dissolve(*led, "its leader was picked by the player");
   const bool first = s_player.count == 0;
   const float d = flyer_wingmen::length(position_of(t) - position_of(leader));
   enlist(t, uc, slot, std::isfinite(d) ? d : 0.0f, leaderMax, now, false);
   if (first) {
      s_player.handPicked = true;
      s_player.brokenOff = false;
      s_player.called = false;
      set_order(s_player, Order::FormUp, now);
   } else if (s_player.order != Order::FormUp) {
      assign_targets(s_player, leader, now);
   }
   say(kSayJoin);
   install_log("[Wingmen] squad key on #%u: it joins your flight (%d)%s", handle_of(t), s_player.count,
               first ? ", picked by hand: losses are not made up" : "");
   return v;
}

// The squad key held (flyer_orders_core.hpp, squad_hold): the flight breaks off,
// every wingman let go, and none joins on its own until the squad key calls a
// flight up again, in form up, or picks one. Not a flight the mission locked.
void break_off()
{
   if (s_locked) {
      install_log("[Wingmen] squad key held: the mission locked your flight (SetWingmenLocked); it stays");
      return;
   }
   const int n = s_player.count;
   release_all(s_player, "broken off by the player", "breakoff");
   s_player.brokenOff = true;
   s_player.called = false;
   s_player.handPicked = false;
   s_player.order = Order::FormUp;
   s_player.resume = Order::FormUp;
   s_player.attackTarget = nullptr;
   s_player.attackHandle = 0;
   say(kSayBreakOff);
   install_log("[Wingmen] squad key held: break off, %d let go; none joins on its own until the squad key calls a "
               "flight up or picks one",
               n);
}

// The squad key, in the air. Held for a second, it breaks the flight off
// (break_off), as the trooper squad key's hold lets its followers go; as it goes
// down, with a friendly AI flyer under the reticule, it picks that one (pick),
// else it calls wingmen up when it leads none (in the order the flight starts
// with, or form up after a break off; not in a flight the mission locked), else
// switches between forming up and engaging (from an attack or cover, forming
// up).
void squad_key(double now)
{
   if (!player_leads_now()) {
      s_squadHold = flyer_orders::SquadHold{};
      return;
   }
   const uint32_t trigger = *(const uint32_t*)(s_player.leader + kFly_Controllable + kCtl_Squad);
   const bool pressed = (trigger & kSquadPressed) == kSquadPressed;
   if (flyer_orders::squad_hold(s_squadHold, (trigger & kSquadDown) != 0, pressed, now)) {
      break_off();
      return;
   }
   if (!pressed || now - s_lastPress < kKeyRepeat) return;
   s_lastPress = now;
   if (pick(s_player.leader, now) != flyer_orders::Pick::None) return;
   if (!s_player.count) {
      if (s_locked) {
         say(kSayNone);
         install_log("[Wingmen] squad key: the mission locked your flight (SetWingmenLocked); no one called up");
         return;
      }
      const bool afterBreakOff = s_player.brokenOff;
      s_player.brokenOff = false;
      s_player.handPicked = false;
      s_player.called = true;
      fill(s_player.leader, now);
      const Order start = flyer_orders::call_up_order(afterBreakOff, start_order_for(s_player.leader));
      if (s_player.count) set_order(s_player, start, now);
      say(s_player.count ? line_for(start) : kSayNone);
      install_log("[Wingmen] squad key: %s%s", s_player.count ? "called up, " : "no one to call up",
                  s_player.count ? order_name(start) : "");
      return;
   }
   set_order(s_player, flyer_orders::after_squad_key(s_player.order), now);
   say(s_player.order == Order::FormUp ? kSayFormUp : kSayEngage);
   install_log("[Wingmen] squad key: %s", order_name(s_player.order));
}

// The comm keys Attack and Defend, as held now: their bits of the player's
// processed inputs, which its own controller (PlayerController::mJoystick) is
// fed. The game makes team chat of the comm keys only online:
// RawControllerInputs::ReadLatest runs the chat and comm-key processing
// (ChatInputProcess, TeamOrderInputProcess) only in a networked game, and in
// single player StandardInputProcess alone, which still turns every binding,
// the comm keys' included, into these bits.
uint32_t comm_bits(uint8_t* leader)
{
   uint8_t* const ctrl = *(uint8_t**)(leader + kFly_Ctrl);
   if (!ctrl || !is_rtti(ctrl, kRttiPlayerController)) return 0;
   const uint8_t* const inputs = *(const uint8_t* const*)(ctrl + kPC_Inputs);
   if (!inputs) return 0;
   return *(const uint32_t*)(inputs + kInputs_Bits) &
          ((1u << kInputCommAttack) | (1u << kInputCommDefend) | (1u << kInputCommFormation));
}

// ---- The formation key ------------------------------------------------------------------
// The comm key Spotted (F5 by default) in the air (the user, 2026-10-14: "We'll
// utilise an unused Fnum key whilst in a flyer, so anything from F5 to F10. F5
// is best I think"): the flight's next formation, round: the vee, its leader
// class's own WingmanSlot layout where it has one, and each of the map's
// formation shapes that holds the flight (as many slots as it has wingmen or
// calls up), in the map's order. A wingman whose slot the new one lacks moves to
// a free one (reslot). It holds until the player leads from another flyer, or a
// mission script sets one (SetWingmenFormation).
void reslot();

void say_formation(const char* name)
{
   if (!s_message) return;
   const wchar_t* prefix = L"Wingmen formation:";
   if (s_localize)
      if (const wchar_t* own = s_localize(pbl_hash("game.wingmen.formation"), true); own && *own) prefix = own;
   wchar_t text[128];
   swprintf_s(text, L"%s %S", prefix, name ? name : "");
   uint8_t color[4] = {0xFF, 0xFF, 0xFF, 0xFF};
   if (s_messageColor) std::memcpy(color, s_messageColor, sizeof color);
   s_message(0, text, color);
}

void cycle_formation()
{
   Formation& f = s_player;
   uint8_t* const leader = f.leader;
   const ClassKeys* keys = keys_of(leader);
   int want = flyer_wingmen::flight_size(s_scriptSize, keys, g_flyerWingmenCount);
   if (want < f.count) want = f.count;
   // The choices, in turn: 0 the vee, 1 the class's own layout, 2 a map shape.
   constexpr int kMaxChoices = 2 + kMaxMapShapes;
   int kinds[kMaxChoices];
   uint32_t names[kMaxChoices];
   int n = 0;
   kinds[n] = 0;
   names[n++] = 0;
   if (keys && keys->hasSlots && keys->slotCount >= want) {
      kinds[n] = 1;
      names[n++] = 0;
   }
   if (s_shapeList) {
      uint8_t* node = *(uint8_t**)(s_shapeList + kList_Next);
      for (int i = 0; i < 1024 && node && node != s_shapeList && n < kMaxChoices; ++i) {
         const uint8_t* const fm = *(const uint8_t* const*)(node + kList_Payload);
         if (!fm) break;   // the head: the list's end
         node = *(uint8_t**)(node + kList_Next);
         Shape sh;
         if (!read_shape_at(fm, sh) || sh.size - 1 < want) continue;
         kinds[n] = 2;
         names[n++] = sh.name;
      }
   }
   // Which it flies now, and the next.
   int cur = 0;
   for (int k = 0; k < n; ++k)
      if ((f.shapeCount > 0 && kinds[k] == 2 && names[k] == f.shapeName) ||
          (f.shapeCount == 0 && !f.vee && kinds[k] == 1))
         cur = k;
   const int next = (cur + 1) % n;
   f.vee = kinds[next] == 0;
   f.shapeCount = 0;
   f.shapeName = 0;
   char label[32] = "vee";
   if (kinds[next] == 1) {
      std::snprintf(label, sizeof label, "%s", "own");
   } else if (kinds[next] == 2) {
      Shape sh;
      if (read_shape(names[next], sh)) {
         const MapShapes m = map_shapes();
         f.shapeCount = shape_slots_for(sh, m.unturn, sh.size - 1, f.shape);
         f.shapeName = sh.name;
      }
      if (const char* nm = shape_name(names[next])) std::snprintf(label, sizeof label, "%s", nm);
      else std::snprintf(label, sizeof label, "%d of %d", next, n - 1);
   }
   reslot();
   say_formation(label);
   install_log("[Wingmen] formation key: %s (%d of %d formations that hold %d), %d wingmen", label, next + 1, n, want,
               f.count);
}

// The comm keys' Attack and Defend (F11 and F12 by default), in the air with
// wingmen, as each goes down: attack my target (the player's lock, else an enemy
// under its reticule, as the key is pressed), and cover me. Online their team
// chat goes out as ever.
void comm_key(double now)
{
   if (!s_commKeys || !player_leads_now()) {
      s_commHeld = 0;
      return;
   }
   const uint32_t held = comm_bits(s_player.leader);
   const uint32_t went = held & ~s_commHeld;
   s_commHeld = held;
   if (went & (1u << kInputCommFormation)) cycle_formation();
   const uint32_t pressed = went & ((1u << kInputCommAttack) | (1u << kInputCommDefend));
   if (!pressed) return;
   const bool defend = (pressed & (1u << kInputCommAttack)) == 0;   // both at once: the attack
   if (!s_player.count) {
      say(kSayNone);
      install_log("[Wingmen] comm key %s: no wingmen to order", defend ? "Defend" : "Attack");
      return;
   }
   if (defend) {
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
// chasing, still flying in or on the leader's trail has its own business).
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
      if (!f.wing[i].target && !f.wing[i].joining && !f.wing[i].onTrail) slots[copying++] = f.wing[i].slot;
   for (int i = 0; i < f.count; ++i) {
      Wingman& w = f.wing[i];
      if (w.target || w.joining || w.onTrail) continue;
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
// has gone after a target meanwhile, nor on the leader's trail).
void copy_trick(Wingman& w, double now)
{
   const int trick = w.trick;
   w.trick = -1;
   w.holdUntil = now + flyer_wingmen::kTrickSettle;   // if it does not trick, it settles from here
   uint8_t* const fl = w.obj;
   if (handle_of(fl) != w.handle || !alive(fl) || !flying(fl) || w.target || w.onTrail || (fl[kFly_Flags] & kFlagTrick))
      return;
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
      update_trail(now);
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
      caught("the flyer update (the wingmen's tick, keys, trail and landing)");
   }
   const bool r = s_update(ctrl, edx, dt);
   // The script hears what changed once the flyer's own update is done.
   if (s_eventCount) flush_events();
   return r;
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
      caught("a wingman's top speed");
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
      caught("a wingman's lowest speed");
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
      caught("a trick's check");
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
// Its target's open side (flyer_orders_core.hpp, pick_open), worked out again
// every kOpenEvery: a ray in onto it from kOpenReach out along each approach
// direction, up first; with up clear, open sky. Off that side and with the
// straight line out to it blocked, the nearest point kSetupRange out along an
// approach direction it reaches in a straight line and that sees the setup
// point (pick_via), else further out across that side (escape_point), looked at
// again in kOpenEvery.
void refresh_open(uint8_t* flyer, Wingman& w, V3 target, double now)
{
   if (now - w.openAt < kOpenEvery) return;
   w.openAt = now;
   uint8_t* const t = w.target;
   float clear[flyer_orders::kApproachDirs];
   for (int k = 0; k < flyer_orders::kApproachDirs; ++k) {
      const V3 d = flyer_orders::approach_dir(k);
      float along = 0.0f;
      clear[k] = blocked(target + d * flyer_orders::kOpenReach, target + d * flyer_orders::kSightShort, flyer, t, &along)
                    ? flyer_orders::kOpenReach - along
                    : flyer_orders::kOpenReach;
   }
   const V3 me = position_of(flyer);
   const bool wasSky = w.sky;
   w.sky = clear[0] >= flyer_orders::kOpenReach;
   w.open = w.sky ? V3{0.0f, 1.0f, 0.0f} : flyer_orders::approach_dir(flyer_orders::pick_open(clear, me - target));
   w.hasVia = false;
   if (!w.sky && !flyer_orders::on_open_side(me, target, w.open)) {
      const V3 setup = flyer_orders::setup_point(target, w.open);
      if (blocked(me, setup, flyer, t)) {
         bool reach[flyer_orders::kApproachDirs], seesSetup[flyer_orders::kApproachDirs];
         float dist[flyer_orders::kApproachDirs];
         for (int k = 0; k < flyer_orders::kApproachDirs; ++k) {
            const V3 p = target + flyer_orders::approach_dir(k) * flyer_orders::kSetupRange;
            dist[k] = flyer_wingmen::length(p - me);
            reach[k] = dist[k] >= kViaReached && !blocked(me, p, flyer, t);   // not one it is at
            seesSetup[k] = reach[k] && !blocked(p, setup, flyer, t);
         }
         const int v = flyer_orders::pick_via(reach, seesSetup, dist);
         const float* wm = matrix_of(flyer);
         const V3 nose = wm ? row(wm, 2) : V3{0.0f, 0.0f, 1.0f};
         w.hasVia = true;
         w.via = v >= 0 ? target + flyer_orders::approach_dir(v) * flyer_orders::kSetupRange
                        : flyer_orders::escape_point(me, target, w.open, nose);
      }
   }
   if (w.sky != wasSky && order_log())
      install_log("[Wingmen] slot %d (#%u): #%u is %s", w.slot + 1, w.handle, w.targetHandle,
                  w.sky ? "under open sky: passes as ever" : "shut in from above: passes along its open side");
}

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
      refresh_open(flyer, w, target, now);
      if (!w.run.extending && !w.sky && !flyer_orders::on_open_side(me, target, w.open)) {
         // Round to its open side first; past the way round, it looks again now.
         if (w.hasVia && flyer_wingmen::length(w.via - me) < kViaReached) {
            w.openAt = -1e9;
            refresh_open(flyer, w, target, now);
         }
         st.point = w.hasVia ? w.via : flyer_orders::setup_point(target, w.open);
         speed = hi;
      } else {
         flyer_orders::update_run(w.run, w.shape, d, aheadCos, now, !w.sees);
         if (w.run.extending) {
            st.point = flyer_orders::extend_point_along(me, nose, w.open);
            speed = hi;
         } else {
            st.point = flyer_real_flight::pursuit_point(me, mySpeed, target, targetVel);
            speed = mid;
            safe = kSafeHeightTaper;
         }
      }
   }
   V3 heading = st.point - me;
   if (!flyer_wingmen::finite(st.point) || !flyer_wingmen::normalise(heading)) return false;
   st.target = flyer_wingmen::target_for_speed(speed, lo, mid, hi);
   st.keepCos = flyer_real_flight::kPursuitKeepCos;
   st.chasing = true;
   if (w.roofed) safe = kSafeHeightTaperFactor;   // under a roof itself: the game's lift off (no_lift)
   write_dest(flyer, st, heading, safe);
   return true;
}

// While its leader lands: down toward a point over its spot, at its slowest from
// kLandSlowFrom in, the safe height let go near it, along the leader's trail
// while a wall is between (follow_trail); else (no place free, or up again
// before its leader) round the sky over its leader, or under a roof low and
// slow round where there is room (roof_circle).
bool steer_land(uint8_t* flyer, Formation& f, Wingman& w, Steer& st)
{
   const float* wm = matrix_of(flyer);
   if (!wm || !f.leader || handle_of(f.leader) != f.leaderHandle) return false;
   const V3 me = row(wm, 3);
   const float leaderSpeed = flyer_wingmen::length(velocity_of(f.leader));
   if (std::isfinite(leaderSpeed)) f.leaderSpeed = f.leaderTarget = leaderSpeed;
   const float hi = flyer_wingmen::stretched_max(s_maxSpeed(flyer, nullptr), high_speed(f));
   const float lo = flyer_wingmen::stretched_min(s_minSpeed(flyer, nullptr), low_speed(f));
   const float mid = s_midSpeed(flyer, nullptr);
   int safe = kSafeHeightFull;
   float speed = mid;
   if (w.land == kLandOutward) {
      // Let go under a roof: back out along the leader's trail first (head_out),
      // at its slowest, not lifted.
      V3 onTrail;
      if (!follow_trail_out(flyer, f, w, seconds(), onTrail)) return false;
      st.point = onTrail;
      speed = lo;
      safe = kSafeHeightTaperFactor;
   } else if (w.land == kLandComing) {
      st.point = w.spot + V3{0.0f, flyer_orders::hover_height(f.padRoof), 0.0f};
      if (flyer_orders::across(me, w.spot) < flyer_orders::kLandSlowFrom) speed = lo;
      safe = kSafeHeightTaper;
      V3 onTrail;
      float behind = 0.0f;
      if (follow_trail(flyer, f, w, st.point, seconds(), onTrail, behind)) {
         // The trail ends at the leader, down: kept clear of it, as in its slot.
         const V3 down = position_of(f.leader);
         st.point = f.padFrame.valid
                       ? flyer_wingmen::to_world(f.padFrame, me,
                                                 flyer_wingmen::keep_clear(flyer_wingmen::in_frame(f.padFrame, me, onTrail),
                                                                           flyer_wingmen::in_frame(f.padFrame, me, down)))
                       : onTrail;
         safe = kSafeHeightTaperFactor;   // where the leader flew: not lifted (steer_before)
      }
   } else if (w.land == kLandCircle && w.circleR > 0.0f) {
      // Under a roof with no place yet: low and slow round where there is room
      // (roof_circle), till one is free.
      st.point = flyer_orders::loiter_point(w.circleAt, me, w.circleR);
      speed = lo;
   } else {
      st.point = flyer_orders::loiter_point(f.pad + V3{0.0f, flyer_orders::kLoiterHeight, 0.0f}, me);
   }
   // Under a roof, or its leader under one and its line clear: the game's lift
   // off, as on the trail, so it is not sent up into the roof.
   if (no_lift(f, w, seconds())) safe = kSafeHeightTaperFactor;
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
   const double now = seconds();
   if (now < w->waitUntil) return false;   // stuck on its leader's trail: its own AI for now (follow_trail)
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
   // Under a roof the player's flight closes up (Formation::squeeze).
   const float squeeze = f == &s_player ? f->squeeze : 1.0f;
   st.slot = flyer_wingmen::to_world(st.frame, leaderPos, slot_of(*f, w->slot) * squeeze);

   // Its speeds are the stretched ones, as its getters answer for it (above),
   // and the target the one the game's throttle turns into that speed.
   const float hi = flyer_wingmen::stretched_max(s_maxSpeed(flyer, nullptr), high_speed(*f));
   const float lo = flyer_wingmen::stretched_min(s_minSpeed(flyer, nullptr), low_speed(*f));
   const float mid = s_midSpeed(flyer, nullptr);

   // A wall between it and its slot: the leader's trail (follow_trail), clear of
   // the leader, at the speed that keeps its place in the line, and never
   // lifted: the stock lift rises 25 m over whatever is ahead (EntityFlyer::
   // RaiseHeight's ray at buildings and vehicles), a hangar's whole ship, which
   // would take it into the roof; the trail is where the leader flew, so
   // SH_TAPER_FACTOR with no factor.
   V3 onTrail;
   float behind = 0.0f;
   if (follow_trail(flyer, *f, *w, st.slot, now, onTrail, behind)) {
      const V3 steer = flyer_wingmen::keep_clear(flyer_wingmen::in_frame(st.frame, me, onTrail),
                                                 flyer_wingmen::in_frame(st.frame, me, leaderPos));
      st.point = flyer_wingmen::to_world(st.frame, me, steer);
      V3 heading = st.point - me;
      if (!flyer_wingmen::finite(st.point) || !flyer_wingmen::normalise(heading)) return false;
      st.target = flyer_wingmen::target_for_speed(flyer_wingmen::formation_speed(f->leaderTarget, behind, lo, hi), lo,
                                                  mid, hi);
      st.chasing = true;   // off its slot: no slot statistics
      write_dest(flyer, st, heading, kSafeHeightTaperFactor);
      st.f = f;
      st.w = w;
      return true;
   }

   const V3 slot = flyer_wingmen::in_frame(st.frame, me, st.slot);   // the slot, from the wingman
   if (!w->holding) flyer_wingmen::update_trim(w->trim, slot, dt);
   const V3 steer = flyer_wingmen::keep_clear(
      flyer_wingmen::steer_offset(slot, flyer_wingmen::lead_distance(st.leaderSpeed), w->trim.climb),
      flyer_wingmen::in_frame(st.frame, me, leaderPos));
   st.point = flyer_wingmen::to_world(st.frame, me, steer);
   V3 heading = st.frame.fwd;
   float speed = flyer_wingmen::formation_speed(f->leaderTarget + w->trim.speed, slot.z, lo, hi);

   // Through the leader's trick and its own copy: straight off its nose, at the
   // leader's speed.
   V3 nose = row(wm, 2);
   if (w->holding && flyer_wingmen::normalise(nose)) {
      st.point = me + nose * flyer_wingmen::kHoldAhead;
      heading = nose;
      speed = flyer_wingmen::formation_speed(f->leaderTarget, 0.0f, lo, hi);
   }
   st.target = flyer_wingmen::target_for_speed(speed, lo, mid, hi);
   // At its slot under a roof (or flying in to one with a clear line): the game's
   // lift off, as on the trail (no_lift). Up 25 m over the top of what is ahead,
   // in a hangar the whole ship, is into the roof.
   write_dest(flyer, st, heading, no_lift(*f, *w, now) ? kSafeHeightTaperFactor : kSafeHeightFull);
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
      caught("the steering before CalculateDest");
      steering = chasing = false;
      if (st.dest) std::memcpy(st.dest, st.saved, kDestSize);
      flyer_real_flight::restore(onPath);
   }
   const bool r = ((fn_calc_dest_t)s_calcDest)(flyer, dt, dest, point, speed, myPos);
   if (steering) {
      __try {
         steer_after(flyer, st, r, point, speed);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         caught("the steering after CalculateDest");
      }
   } else if (chasing) {
      __try {
         flyer_real_flight::steer_after(flyer, onPath, r, point, speed);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         caught("real flight's steering after CalculateDest");
         flyer_real_flight::restore(onPath);
      }
   }
   // Every AI flyer, however steered, clear of the flyers around it.
   if (r && flyer_avoidance::active()) {
      __try {
         flyer_avoidance::adjust(flyer, (const uint8_t*)dest, point, speed);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         caught("the avoidance after CalculateDest");
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
// Read from the player's processed inputs (comm_bits), as its controller reads
// the rest: PlayerController::Update loads them from its +0x88 (MOV EAX, [EBX +
// 0x88], EBX the controller, then their four floats); ProcessedInputs::GetDigital
// tests bit n of their +0x10 for digital input n below 0x1E.
bool install_comm_keys(uintptr_t base)
{
   const auto* a = g_addr;
   if (!a->player_controller_update || !a->processed_inputs_get_digital) return false;
   return starts_with(base, a->player_controller_update + 0xD, "\x8B\xD9", 2) &&
          starts_with(base, a->player_controller_update + 0x242, "\x8B\x83\x88\x00\x00\x00\x8B\x08", 8) &&
          starts_with(base, a->processed_inputs_get_digital,
                      "\x8B\xD1\x8B\x4C\x24\x04\x83\xF9\x1E\x7D\x18\xB8\x01\x00\x00\x00\xD3\xE0\x23\x42\x10\x25\xFF\xFF"
                      "\xFF\x3F",
                      26);
}

// The wingmen's markers: TargetManager::AddMarker(object, region, icon, scale,
// teams, colour, hudTarget, flash, pulse, pulseFrom) and RemoveMarker(object,
// region, teams), both cdecl, the caller popping, as Lua's MapAddEntityMarker
// and MapRemoveEntityMarker call them.
bool install_markers(uintptr_t base)
{
   const auto* a = g_addr;
   if (!a->target_manager_add_marker || !a->target_manager_remove_marker) return false;
   if (!starts_with(base, a->target_manager_add_marker, "\x8B\x44\x24\x04\x83\xEC\x0C\x55\x33\xED\x3B\xC5", 12) ||
       !starts_with(base, a->target_manager_remove_marker, "\x8B\x44\x24\x08\x55\x8B\x6C\x24\x08\x85\xED", 11))
      return false;
   s_addMarker = (fn_add_marker_t)resolve(base, a->target_manager_add_marker);
   s_removeMarker = (fn_remove_marker_t)resolve(base, a->target_manager_remove_marker);
   return true;
}

// The orders' reads and calls, as the game's own code makes them.
bool install_orders(uintptr_t base)
{
   const auto* a = g_addr;
   if (!a->controllable_locked_target || !a->uc_force_attack_target || !a->object_affiliation || !a->hud_message ||
       !a->hud_message_color || !a->localize_find)
      return false;
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
      starts_with(base, a->localize_find, "\x8B\x44\x24\x04\x50\x68\x00\x80\x00\x00", 10);
   if (!ok) return false;
   s_lockedTarget = (fn_locked_t)resolve(base, a->controllable_locked_target);
   s_forceTarget = (fn_force_t)resolve(base, a->uc_force_attack_target);
   s_affiliation = (fn_affiliation_t)resolve(base, a->object_affiliation);
   s_message = (fn_message_t)resolve(base, a->hud_message);
   s_messageColor = (const uint8_t*)resolve(base, a->hud_message_color);
   s_localize = (fn_localize_t)resolve(base, a->localize_find);
   s_commKeys = install_comm_keys(base);
   // Holding fire: AttackHelper::Reset, thiscall() on the AttackHelper, RET, as
   // SetVisibleTarget calls it before taking a target (its mCurPriority from
   // 0x00AD1984 first, then the target and its handle cleared).
   if (a->attack_helper_reset &&
       starts_with(base, a->attack_helper_reset, "\xDB\x05\x84\x19\xAD\x00\x53\x33\xDB\x56\x8B\xF1\xD9\x5E\x1C", 15))
      s_attackReset = (fn_attack_reset_t)resolve(base, a->attack_helper_reset);
   return true;
}

// Landing with the player: EntityFlyer::Land and TakeOff, thiscall() on the
// entity, and CollisionManager::RayHit for the ground beside the leader. Both
// may be detoured already, Land by the LandOnArrival fix and TakeOff by the
// carrier fixes, each of which only adds to it (both install before the
// wingmen, so a JMP at either is theirs).
bool install_landing(uintptr_t base)
{
   const auto* a = g_addr;
   if (!a->entity_flyer_land || !a->entity_flyer_take_off || !a->collision_ray_hit) return false;
   const bool ok =
      (starts_with(base, a->entity_flyer_land, "\x56\x8B\xF1\x8B\x86\xA4\x05\x00\x00", 9) ||
       *(const uint8_t*)resolve(base, a->entity_flyer_land) == 0xE9) &&
      (starts_with(base, a->entity_flyer_take_off, "\x51\x56\x8B\xF1\x8B\x86\xA4\x05\x00\x00\x85\xC0\x57", 13) ||
       *(const uint8_t*)resolve(base, a->entity_flyer_take_off) == 0xE9) &&
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
// (AILowLevel::SetExitVehicle). Stock, no hint ever qualifies (a hint's command
// post is stubbed out, docs/RE/HintNodes.md); should they work, a wingman lands
// only with its leader, so for one the answer is no (and no hint is reserved). thiscall() on the agent, the answer
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
      caught("the land hint check");
      wingman = false;
   }
   return wingman ? false : s_landHint(agent);
}

// The fly agent's TAKEOFF and FLY states take a landed flyer up
// (EntityFlyer::TakeOff, thiscall() on the flyer). A wingman landed beside its
// leader stays down instead: its pilot gets out, or (WingmanExitOnLand = 0) it
// waits to take off with its leader (end_landing calls TakeOff itself). Each
// caller returns straight after the call.
void __fastcall agent_take_off_mt(uint8_t* flyer, void*)
{
   bool wait = false;
   __try {
      Formation* f = nullptr;
      const Wingman* w = find(flyer, &f);
      wait = (w && f->landing && w->land == kLandDown) || settling(flyer);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      caught("the take-off check");
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

// A mission script's names for world objects: PblHashTableCode::_Find
// (cdecl(table, slots, hash); its probe starts from half the slots) on
// EntityEx::mIdMap, as the game's own Lua functions search it (_GetEntity,
// modtools 0x0047103E: push the hash, 0x800, the table; call).
bool install_names(uintptr_t base)
{
   const auto* a = g_addr;
   if (!a->pbl_hash_table_find || !a->entityex_id_map) return false;
   if (!starts_with(base, a->pbl_hash_table_find, "\x8B\x54\x24\x08\xD1\xFA\x56\x8B\x74\x24\x10", 11)) return false;
   s_findName = (fn_find_name_t)resolve(base, a->pbl_hash_table_find);
   s_idMap = resolve(base, a->entityex_id_map);
   return true;
}

// ---- A mission script's calls (flyer_wingmen_lua, below) ----------------------------------
void exclude(uint32_t handle)
{
   if (!handle || excluded(handle)) return;
   if (s_excludedCount == kMaxExcluded) {   // the oldest goes
      std::memmove(s_excluded, s_excluded + 1, (kMaxExcluded - 1) * sizeof(uint32_t));
      --s_excludedCount;
   }
   s_excluded[s_excludedCount++] = handle;
}

void unexclude(uint32_t handle)
{
   for (int i = s_excludedCount - 1; i >= 0; --i)
      if (s_excluded[i] == handle) s_excluded[i] = s_excluded[--s_excludedCount];
}

// A unit as a script gives it, as a world object: by name, or the pointer it
// passed (an entity pointer, or a character's vehicle) if that is one.
uint8_t* object_of_ref(const WingmanRef& unit)
{
   if (unit.name) return object_named(unit.name);
   return unit.obj && is_rtti(unit.obj, kRttiGameObject) ? unit.obj : nullptr;
}

// Losses and the mission's names, at once rather than at the next tick: where
// the player flies with a flight, the named join, and new wingmen take up the
// flight's order.
void refresh(double now)
{
   if (!player_leads_now() || s_player.landing) return;
   uint8_t* const leader = s_player.leader;
   const int before = s_player.count;
   join_roster(leader, now);
   const ClassKeys* keys = keys_of(leader);
   const int want = flyer_wingmen::flight_size(s_scriptSize, keys, g_flyerWingmenCount);
   if (topping_up(keys) && s_player.count < want) fill(leader, now);
   if (s_player.count > before && s_player.order != Order::FormUp) assign_targets(s_player, leader, now);
}

bool add_unit(const WingmanRef& unit)
{
   RosterEntry e;
   if (unit.name) {
      e.name = unit.name;
   } else {
      uint8_t* const obj = object_of_ref(unit);
      if (!obj || !is_rtti(obj, kRttiFlyer)) return false;
      e.obj = obj;
      e.handle = handle_of(obj);
   }
   bool listed = false;
   for (int r = 0; r < s_rosterCount && !listed; ++r)
      listed = e.name ? s_roster[r].name == e.name
                      : !s_roster[r].name && s_roster[r].obj == e.obj && s_roster[r].handle == e.handle;
   if (!listed) {
      if (s_rosterCount >= flyer_wingmen::kMaxWingmen) return false;
      s_roster[s_rosterCount++] = e;
   }
   s_scripted = true;
   if (uint8_t* const obj = roster_object(e)) unexclude(handle_of(obj));
   if (e.name)
      install_log("[Wingmen] the mission names 0x%08X to your flight (%d named)", e.name, s_rosterCount);
   else
      install_log("[Wingmen] the mission names #%u to your flight (%d named)", e.handle, s_rosterCount);
   refresh(seconds());
   return true;
}

bool remove_unit(const WingmanRef& unit)
{
   uint8_t* const obj = object_of_ref(unit);
   bool any = false;
   for (int r = s_rosterCount - 1; r >= 0; --r) {
      const RosterEntry& e = s_roster[r];
      const bool match = unit.name ? e.name == unit.name : obj && (e.name ? roster_object(e) == obj : e.obj == obj);
      if (!match) continue;
      s_roster[r] = s_roster[--s_rosterCount];
      s_roster[s_rosterCount] = RosterEntry{};
      any = true;
   }
   if (obj) {
      exclude(handle_of(obj));   // nor called up on its own again
      for (int i = 0; i < s_player.count; ++i)
         if (s_player.wing[i].obj == obj) {
            drop(s_player, i, "let go by the mission", "removed");
            any = true;
            break;
         }
   }
   return any;
}

bool give_order(Order o, const WingmanRef& target, bool hasTarget)
{
   const double now = seconds();
   if (o != Order::Attack) {
      s_scriptOrder = (int)o;   // the standing order: flights to come start with it too
      install_log("[Wingmen] the mission's standing order: %s", order_name(o));
      if (s_player.leader) set_order(s_player, o, now, true);
      return true;
   }
   // Attack: its target, else the player's own (its lock, else what is under its
   // reticule); anything not on the player's side, within reach of it.
   if (!player_leads_now()) return false;
   uint8_t* const leader = s_player.leader;
   const char* how = "the mission's target";
   uint8_t* const t = hasTarget ? object_of_ref(target) : player_target(leader, &how);
   if (!t || t == leader || !alive(t)) return false;
   if (s_affiliation ? s_affiliation(leader, t) > 0 : team_of(t) == team_of(leader)) return false;   // a friend
   s_player.resume = flyer_orders::resume_after_attack(s_player.order, s_player.resume);
   s_player.attackTarget = t;
   s_player.attackHandle = handle_of(t);
   install_log("[Wingmen] the mission orders an attack on #%u (%s), %.0f m off", handle_of(t), how,
               (double)flyer_wingmen::length(position_of(t) - position_of(leader)));
   set_order(s_player, Order::Attack, now, true);
   return true;
}

// After the formation changed: a wingman whose slot it lacks moves to a free
// one, else is let go.
void reslot()
{
   Formation& f = s_player;
   const ClassKeys* keys = keys_of(f.leader);
   const int cap = capacity(f, keys);
   for (int i = f.count - 1; i >= 0; --i) {
      Wingman& w = f.wing[i];
      if (w.slot < cap) continue;
      w.slot = -1;
      const int slot = free_slot(f, keys, class_id(w.obj), true);
      if (slot >= 0) w.slot = slot;
      else drop(f, i, "the mission's formation holds fewer", "formation");
   }
   f.mirror = false;
}

bool set_formation(const char* name)
{
   if (!name || !*name) {
      s_scriptShapeSet = false;
      s_scriptVee = false;
      s_scriptShape = 0;
   } else if (flyer_orders::word_is(name, "vee")) {
      s_scriptShapeSet = true;
      s_scriptVee = true;
      s_scriptShape = 0;
   } else {
      Shape s;
      const uint32_t shape = pbl_hash(name);
      if (!read_shape(shape, s)) return false;   // not on this map
      s_scriptShapeSet = true;
      s_scriptVee = false;
      s_scriptShape = shape;
   }
   if (s_player.leader) {
      choose_player_shape(s_player.leader);
      reslot();
   }
   return true;
}

} // namespace

// ---- The player's flight from a mission script (flyer_wingmen.hpp) ----------------------
namespace flyer_wingmen_lua {

bool available() { return s_detoured; }

int call(bool setSize, int size)
{
   if (!s_detoured) return 0;
   int n = 0;
   __try {
      s_scripted = true;
      s_scriptAuto = 1;
      if (setSize) s_scriptSize = size < 0 ? -1 : size > flyer_wingmen::kMaxWingmen ? flyer_wingmen::kMaxWingmen : size;
      s_player.called = true;
      s_player.brokenOff = false;    // over the player's break off ...
      s_player.handPicked = false;   // ... and its own pick
      install_log("[Wingmen] the mission calls your wingmen up (%d)", s_scriptSize);
      refresh(seconds());
      n = player_leads_now() ? s_player.count : 0;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      caught("a script's CallWingmen");
      n = 0;
   }
   flush_events();
   return n;
}

int dismiss()
{
   if (!s_detoured) return 0;
   int n = 0;
   __try {
      s_scriptAuto = 0;
      s_player.called = false;
      s_rosterCount = 0;
      for (RosterEntry& e : s_roster) e = RosterEntry{};
      n = s_player.count;
      release_all(s_player, "dismissed by the mission", "dismissed");
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      caught("a script's DismissWingmen");
   }
   flush_events();
   return n;
}

bool add(WingmanRef unit)
{
   if (!s_detoured) return false;
   bool ok = false;
   __try {
      ok = add_unit(unit);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      caught("a script's AddWingman");
      ok = false;
   }
   flush_events();
   return ok;
}

bool remove(WingmanRef unit)
{
   if (!s_detoured) return false;
   bool ok = false;
   __try {
      ok = remove_unit(unit);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      caught("a script's RemoveWingman");
      ok = false;
   }
   flush_events();
   return ok;
}

bool give_order(const char* name, WingmanRef target, bool hasTarget)
{
   Order o;
   if (!s_detoured || !flyer_orders::order_from(name, o, true)) return false;
   bool ok = false;
   __try {
      ok = ::give_order(o, target, hasTarget);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      caught("a script's SetWingmenOrder");
      ok = false;
   }
   flush_events();
   return ok;
}

const char* order() { return s_detoured && s_player.leader ? flyer_orders::order_word(s_player.order) : nullptr; }

void lock(bool locked)
{
   s_locked = locked;
   install_log("[Wingmen] the mission %s your flight", locked ? "locks" : "unlocks");
}

bool formation(const char* name)
{
   if (!s_detoured) return false;
   bool ok = false;
   __try {
      ok = set_formation(name);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      caught("a script's SetWingmenFormation");
      ok = false;
   }
   flush_events();
   return ok;
}

void land(int land) { s_scriptLand = land < 0 ? -1 : land ? 1 : 0; }

int count() { return s_detoured && s_player.leader ? s_player.count : 0; }

uint8_t* wingman(int i)
{
   if (!s_detoured || !s_player.leader || i < 0 || i >= s_player.count) return nullptr;
   // In slot order.
   int done = -1;
   for (int k = 0; k <= i; ++k) {
      int next = -1;
      for (int j = 0; j < s_player.count; ++j)
         if (s_player.wing[j].slot > done && (next < 0 || s_player.wing[j].slot < s_player.wing[next].slot)) next = j;
      if (next < 0) return nullptr;
      done = s_player.wing[next].slot;
      if (k == i) return s_player.wing[next].obj;
   }
   return nullptr;
}

bool is_wingman(const uint8_t* flyer)
{
   Formation* f = nullptr;
   return s_detoured && flyer && find(flyer, &f) && f == &s_player;
}

uint8_t* leader_of(const uint8_t* flyer)
{
   Formation* f = nullptr;
   return s_detoured && flyer && find(flyer, &f) ? f->leader : nullptr;
}

int pilot(const uint8_t* flyer)
{
   if (!flyer) return -1;
   if (const Wingman* w = find(flyer); w && w->pilot >= 0) return w->pilot;
   int index = -1;
   __try {
      index = pilot_of(flyer);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      caught("a script's look-up of a wingman's pilot");
      index = -1;
   }
   return index;
}

uint8_t* character_object(int index, bool vehicleOnly)
{
   uint8_t* obj = nullptr;
   __try {
      int n = 0;
      const uint8_t* const chars = characters(n);
      if (chars && index >= 0 && index < n) {
         const uint8_t* const chr = chars + (size_t)index * layout::Character::kSize;
         uint8_t* const vehicle = *(uint8_t* const*)(chr + layout::Character::kVehicle);
         uint8_t* const unit = *(uint8_t* const*)(chr + layout::Character::kUnit);
         obj = vehicle ? object_of(vehicle) : !vehicleOnly && unit ? object_of(unit) : nullptr;
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      caught("a script's look-up of a character's flyer");
      obj = nullptr;
   }
   return obj;
}

uint8_t* object(WingmanRef unit)
{
   if (!s_detoured) return nullptr;
   uint8_t* obj = nullptr;
   __try {
      obj = object_of_ref(unit);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      caught("a script's look-up of a unit");
      obj = nullptr;
   }
   return obj;
}

const char* lost_reason() { return s_lostReason; }

} // namespace flyer_wingmen_lua

int flyer_wingmen_hud_list(uint8_t** out, int max)
{
   if (!s_detoured || !out || max <= 0) return 0;
   int n = 0;
   bool taken[flyer_wingmen::kMaxWingmen] = {};
   for (int k = 0; k < s_player.count && n < max; ++k) {
      int i = -1;   // the next in slot order
      for (int j = 0; j < s_player.count; ++j)
         if (!taken[j] && (i < 0 || s_player.wing[j].slot < s_player.wing[i].slot)) i = j;
      taken[i] = true;
      const Wingman& w = s_player.wing[i];
      if (handle_of(w.obj) == w.handle && alive(w.obj)) out[n++] = w.obj;
   }
   return n;
}

uint8_t* flyer_wingmen_hud_target()
{
   if (!s_detoured || !s_player.leader || s_player.order != Order::Attack) return nullptr;
   uint8_t* const t = s_player.attackTarget;
   return t && handle_of(t) == s_player.attackHandle && alive(t) ? t : nullptr;
}

// The mission is over: its flights and what its script said go with it, and
// nothing is fired (its Lua state is going).
void flyer_wingmen_mission_end()
{
   for (Formation& f : s_forms) f = Formation{};
   for (RosterEntry& e : s_roster) e = RosterEntry{};
   s_rosterCount = 0;
   s_excludedCount = 0;
   s_scripted = false;
   s_locked = false;
   s_scriptSize = -1;
   s_scriptAuto = -1;
   s_scriptOrder = -1;
   s_scriptLand = -1;
   s_scriptShapeSet = false;
   s_scriptVee = false;
   s_scriptShape = 0;
   s_eventCount = 0;
   s_lostReason = nullptr;
   s_commHeld = 0;
   s_squadHold = flyer_orders::SquadHold{};
   for (Settling& e : s_settling) e = Settling{};
   s_settlingCount = 0;
   s_targetMark = TargetMark{};
   flyer_trail::clear(s_trail);
   s_trailOf = 0;
   s_mapShapesLogged = false;
}

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
      install_log("[Wingmen] orders installed: the squad key calls a flight up, picks the friendly flyer under your "
                  "reticule, or forms up or engages, and held for a second breaks the flight off; %s; a chasing "
                  "wingman fires only with a line of sight to its target%s",
                  s_commKeys ? "the comm keys Attack and Defend (F11 and F12 by default) order attack my target and "
                               "cover me"
                             : "the comm keys' orders NOT installed (the player's inputs are read differently)",
                  s_attackReset ? "" : " (its own AI may still fire without one: AttackHelper::Reset differs)");
   else
      install_log("[Wingmen] orders NOT installed: their reads differ; the squad key forms up or engages, with no "
                  "message");
   if (install_markers(base))
      install_log("[Wingmen] markers installed: %s",
                  g_flyerWingmenMarkers ? "each of your wingmen has a cyan circle on the map and a bracket on screen"
                                        : "off ([AI] FlyerWingmenMarkers=0)");
   else
      install_log("[Wingmen] markers NOT installed: TargetManager's markers differ");
   // Installed whatever the INI says: a leader class's WingmanLand decides for it.
   if (install_landing(base)) {
      const bool kept = install_waiting(base);
      install_log("[Wingmen] landing with you installed (%s): wingmen land beside you, or circle overhead, and their "
                  "pilots get out (WingmanExitOnLand = 0 on a class keeps them aboard to take off with you); %s",
                  g_flyerWingmenLand ? "on, but for classes with WingmanLand = 0"
                                     : "off ([AI] FlyerWingmenLand=0), but for classes with WingmanLand = 1",
                  kept ? "none lands at a land hint on its own"
                       : "NOT kept from landing at land hints on their own, nor aboard (the fly agent's calls differ)");
   } else {
      install_log("[Wingmen] landing with you NOT installed: EntityFlyer's Land or TakeOff differ; wingmen are let go "
                  "when you land");
   }
   // A mission script's calls need nothing installed but the names it uses.
   install_log("[Wingmen] mission scripts may call up, name, order and lock your flight (CallWingmen, AddWingman, "
               "SetWingmenOrder, SetWingmenLocked ...); %s",
               install_names(base) ? "flyers may be named by their world object's name"
                                   : "NOT by name (PblHashTableCode::_Find differs): by entity or character only");

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
   for (Site* s : {&s_landHintSite, &s_takeOffSites[0], &s_takeOffSites[1]}) {
      if (s->at) protected_write(s->at, s->orig, 5);
      *s = Site{};
   }
   s_landHint = nullptr;
   s_exitVehicle = nullptr;
   s_lockedTarget = nullptr;
   s_forceTarget = nullptr;
   s_attackReset = nullptr;
   s_affiliation = nullptr;
   s_message = nullptr;
   s_messageColor = nullptr;
   s_localize = nullptr;
   s_commKeys = false;
   s_commHeld = 0;
   s_addMarker = nullptr;
   s_removeMarker = nullptr;
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
   flyer_wingmen_mission_end();
   s_squadrons = false;
   s_findShape = nullptr;
   s_shapeList = nullptr;
   s_findName = nullptr;
   s_idMap = nullptr;
   s_classes.clear();
}
