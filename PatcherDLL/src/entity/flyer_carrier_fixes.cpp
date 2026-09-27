#include "pch.h"
#include "flyer_carrier_fixes.hpp"
#include "flyer_boost_animation.hpp"
#include "core/resolve.hpp"
#include "core/x86_emit.hpp"
#include "util/crash_logger.hpp"

#include <cmath>
#include <detours.h>

// =============================================================================
// EntityCarrier / EntityCarrierClass fixes
//
// Full RE notes: docs/RE/EntityCarrierSystem.md.  The short version of the
// vanilla lifecycle this file builds on:
//
//   VehicleSpawn::UpdateSpawn   spawns cargo + carrier, AttachCargo(0), then
//                               InitAsFlying + Land: the carrier is born in
//                               LANDING behind the pad (gCalcBeginLandPos).
//   EntityFlyer::Update         LANDING -> LANDED when the ground check passes.
//   VehicleSpawn::UpdateLive    LANDED -> DetachCargo(0) + TakeOff;
//                               FLYING -> vtable[3](1), i.e. the carrier is
//                               DELETED (scalar deleting destructor), never
//                               killed.  Runs once per live VehicleTracker.
//   EntityCarrier::Update       after EntityFlyer::Update, positions every
//                               attached cargo from the carrier matrix.
//
// What this file adds:
//   - Memory-safety guards: SetProperty cargo-node overflow, AttachCargo /
//     DetachCargo slot bounds, AttachCargo null cargo.
//   - Multi-cargo: extra cargo for slots 1..3, detached at touchdown.
//   - Cargo team save/restore so carried cargo can't be used as a spawn point.
//   - Custom descent (lerp onto the pad) and post-drop ascent (forward flight).
//   - Cargo-drop animation override + frustum-cull bypass in the render hook.
//   - Carrier turrets: ActivatePhysics vtable patch, fire-while-airborne patch,
//     PILOT_SELF turret AI.
//   - Terrain-wobble suppression (RayHit neutralised at altitude).
//   - Pad lifecycle gaps vanilla leaves open (see padCarrierUpdate and
//     dropStrandedCargo).
//
// Per-carrier state is keyed by (object pointer, PblHandle id at +0x204).
// Vanilla deletes finished carriers from VehicleSpawn, not from their own
// Update, and EntityCarrier::sMemoryPool hands the freed block straight to the
// next carrier, so a bare pointer key would let the next carrier inherit the
// previous one's state.  The handle id changes on every allocation.
//
// IMPORTANT: EntityCarrier meshes MUST have at least one collision primitive
// (p_ shape).  EntityFlyerClass::SetProperty auto-generates an oversized
// "main_body" capsule from the bounding box when there are none.
// =============================================================================

// ---------------------------------------------------------------------------
// Per-build layout
//
// Steam and GOG share one release layout; modtools is a debug build.  Instance
// fields of EntityFlyer/EntityCarrier in the 0x5xx..0x1Dxx range sit 0x40 lower
// on release, EntityFlyerClass fields 0xC8 lower, EntityCarrierClass cargo
// fields 0xE0 lower.  Every value here was read out of each build's own
// disassembly (see project memory "carrier-steam-port-status" for the sites).
// All instance offsets are from the object base (the pointer AttachCargo,
// DetachCargo and TakeOff receive; Update receives base+0x240).
// ---------------------------------------------------------------------------

struct CarrierLayout {
   // EntityCarrierClass
   uint32_t clsCargoNodes;       // CargoInfo[4]
   uint32_t clsCargoCount;       // int, directly after entry 3 (so entry 4 == the count)

   // EntityFlyerClass
   uint32_t clsTakeoffAnim;      // ZephyrAnim* ("takeoff"), nFrames at +8
   uint32_t clsTakeoffSpeed;     // TakeoffSpeed
   uint32_t clsLandingTime;      // LandingTime
   uint32_t clsLandedHeight;     // -(model bbox min Y)
   uint32_t clsWeaponCount;      // int, aimer count used by ActivatePhysics
   uint32_t clsPassengerCount;   // uint8

   // EntityFlyer / EntityCarrier instance
   uint32_t flightState;         // int: 0 landed, 1 takeoff, 2 flying, 3 landing
   uint32_t progress;            // float, takeoff/landing anim progress
   uint32_t cls;                 // EntityCarrierClass*
   uint32_t landedHeight;        // float, instance landing threshold (incl. cargo)
   uint32_t passengers;          // PassengerSlot*[]
   uint32_t turrets;             // MountedTurret*[8]
   uint32_t turretCount;         // int8
   uint32_t aimers;              // Aimer*[]
   uint32_t postCollision;       // embedded sub-object activated with (-15, 2)
   uint32_t animRef;             // ZephyrAnim* the render reads nFrames from
   uint32_t netAnimDelta;        // float, net-interpolated anim delta
   uint32_t cargoSlots;          // CargoSlot[4]

   // Code sites (unrelocated), identical VAs on Steam and GOG
   uintptr_t visJz;              // 6-byte JZ that skips the render on a cull miss
   uintptr_t rayHit1;            // CALL CollisionManager::RayHit, TAKEOFF branch
   uintptr_t rayHit2;            // CALL CollisionManager::RayHit, LANDING branch

   // Convention differences
   bool attachSlotIsIndex;       // AttachCargo honours its slot argument
   bool updateSpawnRegcall;      // UpdateSpawn takes dt in XMM1 + bare RET
   bool fireStateMachineHasDt;   // trigger state machine takes (dt, fire)
   bool rayHitSse;               // RayHit returns its fraction in XMM0
};

static constexpr CarrierLayout kCarrierModtools = {
   /* clsCargoNodes */ 0x1180, /* clsCargoCount */ 0x11C0,
   /* clsTakeoffAnim */ 0x87C, /* clsTakeoffSpeed */ 0x8E8, /* clsLandingTime */ 0x8EC,
   /* clsLandedHeight */ 0x8F4, /* clsWeaponCount */ 0xD48, /* clsPassengerCount */ 0xE14,
   /* flightState */ 0x5A4, /* progress */ 0x5A8, /* cls */ 0x66C, /* landedHeight */ 0x600,
   /* passengers */ 0x670, /* turrets */ 0x680, /* turretCount */ 0x6A0, /* aimers */ 0x6A8,
   /* postCollision */ 0x1D10, /* animRef */ 0x1870, /* netAnimDelta */ 0x1D00,
   /* cargoSlots */ 0x1DD0,
   /* visJz */ 0x004F6999, /* rayHit1 */ 0x004FE8CD, /* rayHit2 */ 0x004FEAE2,
   /* attachSlotIsIndex */ true, /* updateSpawnRegcall */ false,
   /* fireStateMachineHasDt */ true, /* rayHitSse */ false,
};

static constexpr CarrierLayout kCarrierRelease = {
   /* clsCargoNodes */ 0x10A0, /* clsCargoCount */ 0x10E0,
   /* clsTakeoffAnim */ 0x7B4, /* clsTakeoffSpeed */ 0x820, /* clsLandingTime */ 0x824,
   /* clsLandedHeight */ 0x82C, /* clsWeaponCount */ 0xC80, /* clsPassengerCount */ 0xD4C,
   /* flightState */ 0x564, /* progress */ 0x568, /* cls */ 0x62C, /* landedHeight */ 0x5C0,
   /* passengers */ 0x630, /* turrets */ 0x640, /* turretCount */ 0x660, /* aimers */ 0x668,
   /* postCollision */ 0x1CD0, /* animRef */ 0x1830, /* netAnimDelta */ 0x1CC0,
   /* cargoSlots */ 0x1D90,
   /* visJz */ 0x004AB082, /* rayHit1 */ 0x004AE246, /* rayHit2 */ 0x004AE478,
   /* attachSlotIsIndex */ false, /* updateSpawnRegcall */ true,
   /* fireStateMachineHasDt */ false, /* rayHitSse */ true,
};

static const CarrierLayout* L = nullptr;

// Build-invariant offsets (all verified on both layouts).
static constexpr uintptr_t kControllableBase = 0x240;  // Update's `this`
static constexpr uintptr_t kRenderBase       = 0x94;   // EntityFlyer::Render's `this`
static constexpr uintptr_t kHandleId         = 0x204;  // PblHandled id (GameObject)
static constexpr uintptr_t kTeamBits         = 0x234;  // bits 4-7 team, 8-11 perceived
static constexpr uintptr_t kMatrix           = 0xF0;   // PblMatrix: right, up, forward, pos
static constexpr uintptr_t kPosX             = 0x120;
static constexpr uintptr_t kPosY             = 0x124;
static constexpr uintptr_t kPosZ             = 0x128;
static constexpr uintptr_t kForward          = 0x110;  // matrix row 2
static constexpr uintptr_t kSphere           = 0xC4;   // scene bounding sphere x,y,z,r

static constexpr int kMaxCargo = 4;
static constexpr unsigned int kCargoNodeName_Hash   = 0x3e2c4da4u;
static constexpr unsigned int kCargoNodeOffset_Hash = 0x910a89fcu;

// GameObject vtable slots (verified identical on modtools and Steam).
static constexpr int kVt_DeletingDtor = 3;   // scalar deleting destructor, arg 1 = free
static constexpr int kVt_Activate     = 5;
static constexpr int kVt_SetTeam      = 36;

// VehicleSpawn fields (build-invariant; engine field names in comments).
static constexpr uintptr_t kVS_PadTransform = 0x30;   // mMatrix
static constexpr uintptr_t kVS_SpawnCount   = 0x7C;   // mSpawnCount
static constexpr uintptr_t kVS_SpawnClass   = 0x90;   // mSpawnClass[8] (cargo class)
static constexpr uintptr_t kVS_UseCarrier   = 0xD0;   // mUseCarrier[8]
static constexpr uintptr_t kVS_ListSentinel = 0xD8;   // mTrackerList
static constexpr uintptr_t kVS_TrackerCount = 0xE8;   // mTrackerList._iCount
static constexpr uintptr_t kVS_CarrierPtr   = 0xEC;   // mCarrier (PblHandle ptr)
static constexpr uintptr_t kVS_CarrierGen   = 0xF0;   // mCarrier (PblHandle id)
static constexpr uintptr_t kVS_Team         = 0xF8;   // mSpawnTeam (1-based)

static inline int&   fieldI(char* p, uintptr_t off) { return *(int*)(p + off); }
static inline float& fieldF(char* p, uintptr_t off) { return *(float*)(p + off); }

static inline CargoSlot* cargoSlots(char* base) { return (CargoSlot*)(base + L->cargoSlots); }
static inline int  flightState(char* base) { return fieldI(base, L->flightState); }
static inline char* carrierClass(char* base) { return *(char**)(base + L->cls); }

// Live cargo in a slot, or null.  Mirrors the engine's PblHandle test.
static void* slotCargo(char* base, int slot)
{
   CargoSlot& s = cargoSlots(base)[slot];
   if (!s.mObjectPtr) return nullptr;
   if (*(int*)((char*)s.mObjectPtr + kHandleId) != s.mObjectGen) return nullptr;
   return s.mObjectPtr;
}

static void setTeam(void* obj, int team)
{
   typedef void(__thiscall* SetTeam_t)(void*, int);
   ((SetTeam_t)(*(void***)obj)[kVt_SetTeam])(obj, team);
}

static void* g_carrierVtable = nullptr;

// ---------------------------------------------------------------------------
// Per-carrier tracking
// ---------------------------------------------------------------------------

static constexpr int kMaxTrackedCarriers = 8;

struct CarrierTrack {
   char*    base;               // nullptr = free
   int      handleId;           // base+0x204 at registration

   float    padX, padY, padZ;
   float    descentDuration;    // LandingTime (clamped)
   float    forwardSpeed;       // TakeoffSpeed (clamped)
   float    landedHt;           // class LandedHeight
   int      savedCargoTeam[kMaxCargo];  // -1 = nothing saved

   int      lastState;
   bool     cargoDropped;

   // Descent (LANDING)
   bool     landActive;
   float    landStartX, landStartY, landStartZ, landTargetY, landElapsed;

   // Post-drop ascent (TAKEOFF): transform snapshot + forward displacement
   bool     ascentActive;
   float    ascentElapsed;
   float    fwdDirX, fwdDirZ;
   float    snapX, snapZ;
   float    snapRot[12];        // matrix rows 0..2 (+0xF0..+0x11F)

   // Cargo-drop animation override (render hook)
   bool     animActive;
   DWORD    animStartTick, animLastMs, animPausedMs;
   float    animDuration;
};
static CarrierTrack g_tracks[kMaxTrackedCarriers] = {};

static bool trackAlive(const CarrierTrack& t)
{
   if (!t.base) return false;
   __try {
      return *(int*)(t.base + kHandleId) == t.handleId;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// Live track for this carrier, or null.  Frees dead entries it walks past.
static CarrierTrack* trackFind(char* base)
{
   for (CarrierTrack& t : g_tracks) {
      if (!t.base) continue;
      if (!trackAlive(t)) { t = {}; continue; }
      if (t.base == base) return &t;
   }
   return nullptr;
}

static CarrierTrack* trackAcquire(char* base)
{
   if (CarrierTrack* t = trackFind(base)) return t;
   for (CarrierTrack& t : g_tracks) {
      if (t.base) continue;
      t = {};
      t.base      = base;
      t.handleId  = *(int*)(base + kHandleId);
      t.lastState = -1;
      for (int& team : t.savedCargoTeam) team = -1;
      return &t;
   }
   static bool s_reported = false;
   if (!s_reported) {
      s_reported = true;
      if (auto fn = get_gamelog())
         fn("[Carrier] more than %d carriers in flight; extras fly vanilla\n", kMaxTrackedCarriers);
   }
   return nullptr;
}

static void trackRelease(char* base)
{
   for (CarrierTrack& t : g_tracks)
      if (t.base == base) t = {};
}

// Take a cargo's team and park it on team 0 while it is carried, so the cargo
// can't be used as a spawn point / entered mid-air.  Restored on detach.
static void saveCargoTeam(CarrierTrack& t, int slot, void* cargo)
{
   int* bits = (int*)((char*)cargo + kTeamBits);
   int team  = (*bits >> 4) & 0xF;
   t.savedCargoTeam[slot] = team;
   if (team != 0) {
      setTeam(cargo, 0);
      *bits &= ~0xFF0;
   }
}

static void restoreCargoTeam(CarrierTrack& t, int slot, void* cargo)
{
   int team = t.savedCargoTeam[slot];
   t.savedCargoTeam[slot] = -1;
   if (team <= 0) return;
   setTeam(cargo, team);
   int* bits = (int*)((char*)cargo + kTeamBits);
   *bits ^= ((team << 4) ^ *bits) & 0xF0;
   *bits ^= ((team << 8) ^ *bits) & 0xF00;
}

// GameLoop::sPauseMode, for pause-aware animation timing (null = unmapped).
static uint8_t* g_pauseMode = nullptr;

// ---------------------------------------------------------------------------
// EntityCarrierClass::SetProperty
//   __thiscall(EntityCarrierClass* this, uint hash, const char* value)
//
// No bounds check on mCargoCount before writing mCargoInfo[mCargoCount].  At
// count 4 the write lands on mCargoCount itself, at 5 on mSoundCargoPickup.
// ---------------------------------------------------------------------------

using fn_SetProperty_t = void(__fastcall*)(void* ecx, void* edx, unsigned int hash, const char* value);
static fn_SetProperty_t original_SetProperty = nullptr;

static void __fastcall hooked_SetProperty(void* ecx, void* /*edx*/, unsigned int hash, const char* value)
{
   if (hash == kCargoNodeName_Hash || hash == kCargoNodeOffset_Hash) {
      if (fieldI((char*)ecx, L->clsCargoCount) >= kMaxCargo) return;
   }
   original_SetProperty(ecx, nullptr, hash, value);
}

// ---------------------------------------------------------------------------
// EntityCarrier::AttachCargo
//   __thiscall(EntityCarrier* base, int slot, GameObject* cargo), RET 8
//
// No slot bounds check, and the cargo is dereferenced before any null check.
// Release builds' copy ignores the slot argument entirely (LTCG specialised it
// for the only caller, which passes 0), so there the argument is garbage and
// must not be tested; see attachCargoToSlot() for filling other slots.
// ---------------------------------------------------------------------------

using fn_AttachCargo_t = bool(__fastcall*)(void* ecx, void* edx, int slot, void* cargo);
static fn_AttachCargo_t original_AttachCargo = nullptr;

static bool __fastcall hooked_AttachCargo(void* ecx, void* /*edx*/, int slot, void* cargo)
{
   if (L->attachSlotIsIndex && (unsigned)slot >= (unsigned)kMaxCargo) return false;
   if (!cargo) return false;
   return original_AttachCargo(ecx, nullptr, slot, cargo);
}

// Attach `cargo` to any slot, on every build.  Where AttachCargo is pinned to
// slot 0 we present class node `slot` as node 0 and an empty slot 0, let the
// engine run its own placement math, then move the slot it wrote into place and
// put back what we borrowed.  The class is shared by every carrier of this
// type, so the restore runs on the exception path too.
static bool attachCargoToSlot(char* base, int slot, void* cargo)
{
   if (L->attachSlotIsIndex || slot == 0)
      return original_AttachCargo(base, nullptr, slot, cargo);
   if ((unsigned)slot >= (unsigned)kMaxCargo) return false;

   bool ok = false;
   __try {
      char* cls = carrierClass(base);
      if (cls) {
         CargoInfo* nodes = (CargoInfo*)(cls + L->clsCargoNodes);
         CargoSlot* slots = cargoSlots(base);
         CargoInfo savedNode = nodes[0];
         CargoSlot savedSlot = slots[0];
         __try {
            nodes[0] = nodes[slot];
            slots[0] = {};
            original_AttachCargo(base, nullptr, 0, cargo);
            slots[slot] = slots[0];
            ok = true;
         } __finally {
            slots[0] = savedSlot;
            nodes[0] = savedNode;
         }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
   return ok;
}

// ---------------------------------------------------------------------------
// EntityCarrier::DetachCargo
//   __thiscall(EntityCarrier* base, int slot), RET 4
//
// No slot bounds check.  Also restores the cargo's team and, for slot 0,
// starts the cargo-drop animation override.
// ---------------------------------------------------------------------------

using fn_DetachCargo_t = bool(__fastcall*)(void* ecx, void* edx, int slot);
static fn_DetachCargo_t original_DetachCargo = nullptr;

static bool s_renderHooked = false;

static void startDropAnimation(CarrierTrack& t, char* base)
{
   float dur = 3.0f;
   __try {
      char* cls = carrierClass(base);
      void* anim = cls ? *(void**)(cls + L->clsTakeoffAnim) : nullptr;
      if (anim) {
         unsigned short nFrames = *(unsigned short*)((char*)anim + 8);
         if (nFrames > 0) dur = (float)nFrames / 30.0f;
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {}

   t.animActive    = true;
   t.animStartTick = GetTickCount();
   t.animLastMs    = t.animStartTick;
   t.animPausedMs  = 0;
   t.animDuration  = dur;
}

static bool __fastcall hooked_DetachCargo(void* ecx, void* /*edx*/, int slot)
{
   if ((unsigned)slot >= (unsigned)kMaxCargo) return false;
   char* base = (char*)ecx;

   void* cargo = nullptr;
   __try { cargo = slotCargo(base, slot); } __except (EXCEPTION_EXECUTE_HANDLER) {}

   bool result = original_DetachCargo(ecx, nullptr, slot);
   if (!cargo) return result;

   __try {
      if (CarrierTrack* t = trackFind(base)) {
         restoreCargoTeam(*t, slot, cargo);
         if (s_renderHooked && slot == 0) startDropAnimation(*t, base);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {}
   return result;
}

// Detach every live cargo slot through the hooked path (team restore).
static void detachAllCargo(char* base, int firstSlot = 0)
{
   for (int s = firstSlot; s < kMaxCargo; s++) {
      bool live = false;
      __try { live = slotCargo(base, s) != nullptr; } __except (EXCEPTION_EXECUTE_HANDLER) {}
      if (live) hooked_DetachCargo(base, nullptr, s);
   }
}

// ---------------------------------------------------------------------------
// Runtime code patches: render cull bypass + RayHit neutralisation
// ---------------------------------------------------------------------------

static unsigned char* codeSite(uintptr_t va)
{
   return va ? (unsigned char*)resolve(va) : nullptr;
}

static void writeCode(unsigned char* site, const void* bytes, size_t len)
{
   DWORD oldProt;
   if (VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &oldProt)) {
      memcpy(site, bytes, len);
      VirtualProtect(site, len, oldProt, &oldProt);
   }
}

// Visibility JZ (0F 84 rel32) that skips the whole flyer render when the
// bounding sphere fails the frustum test.  NOP'd for the duration of one
// carrier render call.
static unsigned char* s_visJz = nullptr;
static unsigned char  s_visJzOrig[6];
static const unsigned char kNop6[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

// The two downward RayHit CALLs in EntityFlyer::Update (TAKEOFF / LANDING
// branches).  Neutralised while a tracked carrier is high above its pad so the
// terrain normal can't make it swirl: groundDistance comes out as 1024 and the
// terrain-normal block is skipped.  modtools returns the ray fraction on the
// x87 stack (FLD1 + 3 NOPs); release returns it in XMM0, so the CALL is
// redirected to a stub instead (RayHit is caller-cleans, stack stays balanced).
static unsigned char* s_rayHit[2] = {};
static unsigned char  s_rayHitOrig[2][5];
static unsigned char  s_rayHitPatch[2][5];
static float s_rayHitOne = 1.0f;

static __declspec(naked) void rayHitStub_sse()
{
   __asm {
      movss xmm0, dword ptr [s_rayHitOne]
      ret
   }
}

static void codePatchesInit()
{
   s_visJz = codeSite(L->visJz);
   if (s_visJz) memcpy(s_visJzOrig, s_visJz, 6);

   const uintptr_t vas[2] = { L->rayHit1, L->rayHit2 };
   for (int i = 0; i < 2; i++) {
      s_rayHit[i] = codeSite(vas[i]);
      if (!s_rayHit[i]) continue;
      memcpy(s_rayHitOrig[i], s_rayHit[i], 5);
      if (L->rayHitSse) {
         x86::encode_branch(s_rayHitPatch[i], s_rayHit[i], x86::kCall, &rayHitStub_sse);
      } else {
         const unsigned char fld1[5] = { 0xD9, 0xE8, 0x90, 0x90, 0x90 };
         memcpy(s_rayHitPatch[i], fld1, 5);
      }
   }
}

static void rayHitSet(bool neutralise)
{
   for (int i = 0; i < 2; i++)
      if (s_rayHit[i]) writeCode(s_rayHit[i], neutralise ? s_rayHitPatch[i] : s_rayHitOrig[i], 5);
}

// ---------------------------------------------------------------------------
// EntityFlyer::Render (this = base+0x94)
//
// For tracked carriers: bypass the frustum cull (the sphere is kept at the
// pivot, see syncBoundingSphere), hold the takeoff anim at frame 0 during the
// descent, and after the drop play it 0 -> 1 on a wall clock.  Other flyers go
// through the boost-animation path.
// ---------------------------------------------------------------------------

using fn_FlyerRender_t = void(__fastcall*)(void* ecx, void* edx, unsigned int lod, float p3, unsigned int p4);
static fn_FlyerRender_t original_FlyerRender = nullptr;

static void renderUncalled(void* ecx, unsigned int lod, float p3, unsigned int p4)
{
   if (s_visJz) writeCode(s_visJz, kNop6, 6);
   original_FlyerRender(ecx, nullptr, lod, p3, p4);
   if (s_visJz) writeCode(s_visJz, s_visJzOrig, 6);
}

static void __fastcall hooked_FlyerRender(void* ecx, void* /*edx*/, unsigned int lod, float p3, unsigned int p4)
{
   char* base = (char*)ecx - kRenderBase;
   CarrierTrack* t = trackFind(base);

   if (!t) {
      bool boost = flyer_boost_anim_render_prepare(base);
      original_FlyerRender(ecx, nullptr, lod, p3, p4);
      if (boost) flyer_boost_anim_render_restore(base);
      return;
   }

   float* progSlot     = &fieldF(base, L->progress);
   float* netDeltaSlot = &fieldF(base, L->netAnimDelta);
   void** animRefSlot  = (void**)(base + L->animRef);
   const float savedProg     = *progSlot;
   const float savedNetDelta = *netDeltaSlot;
   void* const savedAnimRef  = *animRefSlot;

   if (t->animActive) {
      DWORD now = GetTickCount();
      if (g_pauseMode && *g_pauseMode) t->animPausedMs += now - t->animLastMs;
      t->animLastMs = now;
      float elapsed = (float)(now - t->animStartTick - t->animPausedMs) / 1000.0f;
      float prog = (t->animDuration > 0.0f) ? elapsed / t->animDuration : 1.0f;
      if (prog > 1.0f) prog = 1.0f;

      *progSlot     = prog;
      *netDeltaSlot = 0.0f;
      // The render may be pointed at another clip (e.g. landing): force the
      // takeoff clip so nFrames matches the progress we drive.
      char* cls = carrierClass(base);
      void* takeoff = cls ? *(void**)(cls + L->clsTakeoffAnim) : nullptr;
      if (takeoff) *animRefSlot = takeoff;

      renderUncalled(ecx, 0, p3, p4);   // LOD 0: the skinned mesh
   } else {
      // Vanilla plays the takeoff clip backwards during LANDING; hold frame 0.
      if (flightState(base) == 3) *progSlot = 0.0f;
      renderUncalled(ecx, lod, p3, p4);
   }

   *progSlot     = savedProg;
   *netDeltaSlot = savedNetDelta;
   *animRefSlot  = savedAnimRef;
}

// ---------------------------------------------------------------------------
// Carrier turret fire patch
//
// MountedTurret::Update blocks the occupant's fire when the parent EntityFlyer
// is not LANDED.  The gate is replaced by a JMP to a cave that skips the state
// test when the parent is an EntityCarrier.
//   modtools 0x565c4c, 17 bytes: MOV ECX,[EAX+0x5A4] / TEST ECX,ECX / JNZ
//   Steam    0x5a64df, 18 bytes: CMP [EAX+0x564],0 / JNZ
// ---------------------------------------------------------------------------

static constexpr size_t kTurretFirePatch_max = 18;
static unsigned char* s_turretFireSite  = nullptr;
static size_t         s_turretFireLen   = 0;
static uintptr_t      s_turretFireAllow = 0;
static uintptr_t      s_turretFireBlock = 0;
static uintptr_t      s_turretFireStateOff = 0;
static unsigned char  s_turretFireSaved[kTurretFirePatch_max] = {};

// Entered from the patch site with ESI = the turret's parent-controllable.
static __declspec(naked) void turretFire_cave()
{
   __asm {
      mov  edx, [esi]
      mov  ecx, esi
      call dword ptr [edx + 0x24]     // EAX = parent EntityFlyer base
      mov  ecx, [eax]
      cmp  ecx, dword ptr [g_carrierVtable]
      je   _allow
      mov  ecx, dword ptr [s_turretFireStateOff]
      mov  ecx, [eax + ecx]
      test ecx, ecx
      jnz  _block
   _allow:
      jmp  dword ptr [s_turretFireAllow]
   _block:
      jmp  dword ptr [s_turretFireBlock]
   }
}

static void turretFireInstall()
{
   if (!g_addr->turret_fire_check || !g_addr->turret_fire_allow || !g_addr->turret_fire_block) return;

   // The patch must end exactly where the "allow" path resumes.
   const size_t len = (size_t)(g_addr->turret_fire_allow - g_addr->turret_fire_check);
   if (len < 5 || len > kTurretFirePatch_max) return;

   s_turretFireSite     = (unsigned char*)resolve(g_addr->turret_fire_check);
   s_turretFireLen      = len;
   s_turretFireAllow    = (uintptr_t)resolve(g_addr->turret_fire_allow);
   s_turretFireBlock    = (uintptr_t)resolve(g_addr->turret_fire_block);
   s_turretFireStateOff = L->flightState;

   unsigned char patch[kTurretFirePatch_max];
   x86::encode_branch(patch, s_turretFireSite, x86::kJmp, &turretFire_cave, sizeof(patch));

   memcpy(s_turretFireSaved, s_turretFireSite, len);
   writeCode(s_turretFireSite, patch, len);
}

// ---------------------------------------------------------------------------
// Controllable::CreateController null check (modtools only)
//
// The PlayerController path reads [ESI+0xD0] and dereferences +0xD4 with no
// null check (the UnitController path has one).  Only observed under a
// debugger; a 23-byte cave on debug codegen, so it stays modtools-only.
// ---------------------------------------------------------------------------

static constexpr size_t kCreateCtrlPatch_len = 23;   // 0x0055b2e8..0x0055b2fe
static unsigned char* s_createCtrlSite   = nullptr;
static uintptr_t      s_createCtrlResume = 0;
static uintptr_t      s_playerCtrlCtor   = 0;
static unsigned char  s_createCtrlSaved[kCreateCtrlPatch_len] = {};

static __declspec(naked) void createCtrl_cave()
{
   __asm {
      mov  eax, [esi + 0xD0]
      test eax, eax
      jz   _null
      mov  ecx, [eax + 0xD4]
      push ecx
      lea  ecx, [esi + 0x18]
      call dword ptr [s_playerCtrlCtor]
      jmp  dword ptr [s_createCtrlResume]
   _null:
      xor  edi, edi                   // EDI = 0 -> AddController check skips
      jmp  dword ptr [s_createCtrlResume]
   }
}

static void createCtrlInstall()
{
   if (g_build != GameBuild::Modtools || !g_addr->create_ctrl_patch) return;
   s_createCtrlSite   = (unsigned char*)resolve(g_addr->create_ctrl_patch);
   s_createCtrlResume = (uintptr_t)resolve(g_addr->create_ctrl_resume);
   s_playerCtrlCtor   = (uintptr_t)resolve(g_addr->player_ctrl_ctor);

   unsigned char patch[kCreateCtrlPatch_len];
   x86::encode_branch(patch, s_createCtrlSite, x86::kJmp, &createCtrl_cave, sizeof(patch));

   memcpy(s_createCtrlSaved, s_createCtrlSite, kCreateCtrlPatch_len);
   writeCode(s_createCtrlSite, patch, kCreateCtrlPatch_len);
}

// ---------------------------------------------------------------------------
// PILOT_SELF turret AI (MountedTurret::UpdateIndirect)
//
// PILOT_SELF turrets get no threat data of their own, so the parent carrier's
// UnitController is borrowed for the call: the AI aims the turret (heading
// controls at +0x88/+0x8C) and ProcessFire writes its fire decision into the
// carrier's triggers, which are read and then restored.  The turret's weapon is
// not updated by anyone else for PILOT_SELF, so it is pumped here every frame
// (heat, reload and charge keep ticking while the turret searches).
//
// Known limitation: every turret shares the carrier's AI state for its call,
// so with several turrets their target selection is not independent.
// ---------------------------------------------------------------------------

// Controllable-relative, build-invariant.
static constexpr uintptr_t kCtl_Fire      = 0x38;   // uint32 trigger[2]
static constexpr uintptr_t kCtl_TurnCtrl  = 0x88;
static constexpr uintptr_t kCtl_PitchCtrl = 0x8C;
static constexpr uintptr_t kCtl_AI        = 0xC8;   // UnitController*
static constexpr uintptr_t kCtl_PilotType = 0x144;  // 1 = PILOT_SELF
static constexpr uintptr_t kCtl_Weapons   = 0x224;  // Weapon*[2]
static constexpr uintptr_t kCtl_WeaponIdx = 0x234;
static constexpr uintptr_t kCtl_Parent    = 0x24C;

using fn_TurretUpdateIndirect_t = bool(__fastcall*)(void* ecx, void* edx, float dt);
static fn_TurretUpdateIndirect_t original_TurretUpdateIndirect = nullptr;

// Trigger fire state machine:
//   modtools 0x00562dd0 : __thiscall(uint32_t* trigger, float dt, char fire)  RET 8
//   release  0x0043a950 : __thiscall(uint32_t* trigger, char fire)            RET 4
using fn_FireStateMachine_t     = void(__fastcall*)(void* ecx, void* edx, float dt, char fire);
using fn_FireStateMachineNoDt_t = void(__fastcall*)(void* ecx, void* edx, char fire);
static void* g_FireStateMachine = nullptr;

static void fireStateMachine(void* trigger, float dt, char fire)
{
   if (L->fireStateMachineHasDt)
      ((fn_FireStateMachine_t)g_FireStateMachine)(trigger, nullptr, dt, fire);
   else
      ((fn_FireStateMachineNoDt_t)g_FireStateMachine)(trigger, nullptr, fire);
}

static bool __fastcall hooked_TurretUpdateIndirect(void* ecx, void* /*edx*/, float dt)
{
   char* ctrl = (char*)ecx;
   if (fieldI(ctrl, kCtl_PilotType) != 1)
      return original_TurretUpdateIndirect(ecx, nullptr, dt);

   char* parent = *(char**)(ctrl + kCtl_Parent);
   bool isCarrier = false;
   __try { isCarrier = parent && *(void**)parent == g_carrierVtable; }
   __except (EXCEPTION_EXECUTE_HANDLER) {}
   if (!isCarrier) return original_TurretUpdateIndirect(ecx, nullptr, dt);

   char* parentCtrl = parent + kControllableBase;
   void* parentAI   = *(void**)(parentCtrl + kCtl_AI);
   if (!parentAI) return original_TurretUpdateIndirect(ecx, nullptr, dt);

   uint32_t* parentFire = (uint32_t*)(parentCtrl + kCtl_Fire);
   const uint32_t savedFire0 = parentFire[0], savedFire1 = parentFire[1];

   void* turretAI = *(void**)(ctrl + kCtl_AI);
   *(void**)(ctrl + kCtl_AI) = parentAI;
   bool result = original_TurretUpdateIndirect(ecx, nullptr, dt);
   *(void**)(ctrl + kCtl_AI) = turretAI;

   // ProcessFire's decision landed in the parent's triggers (bit 0 = fire).
   const bool aiWantsToFire = ((parentFire[0] | parentFire[1]) & 1) != 0;
   parentFire[0] = savedFire0;
   parentFire[1] = savedFire1;

   static constexpr float kAimThreshold = 0.3f;
   const float turn  = fieldF(ctrl, kCtl_TurnCtrl);
   const float pitch = fieldF(ctrl, kCtl_PitchCtrl);
   const bool onTarget = aiWantsToFire && fabsf(turn) < kAimThreshold && fabsf(pitch) < kAimThreshold;

   const int weaponIdx = fieldI(ctrl, kCtl_WeaponIdx);
   if (weaponIdx < 0 || weaponIdx >= 2) return result;

   fireStateMachine(ctrl + kCtl_Fire + weaponIdx * 4, dt, onTarget ? 1 : 0);

   void* weapon = *(void**)(ctrl + kCtl_Weapons + weaponIdx * 4);
   if (weapon) {
      using fn_WeaponUpdate_t = bool(__thiscall*)(void* weapon, float dt);
      __try {
         ((fn_WeaponUpdate_t)(*(void***)weapon)[1])(weapon, dt);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         static bool s_reported = false;
         if (!s_reported) {
            s_reported = true;
            if (auto fn = get_gamelog()) fn("[TurretAI] Weapon::Update_ exception for wpn=%p\n", weapon);
         }
      }
   }
   return result;
}

// ---------------------------------------------------------------------------
// EntityCarrier::ActivatePhysics (vtable[41])
//
// The carrier's override only activates the main Controllable (priority -1);
// EntityFlyer's also activates the post-collision sub-object, aimers, turrets
// and passenger slots, so carrier turrets were built but never activated
// (invisible, inert).  This is EntityFlyer::ActivatePhysics with the carrier's
// -1 priority kept.
// ---------------------------------------------------------------------------

static constexpr int kVt_ActivatePhysics = 41;
using fn_ActivateChild_t = void(__fastcall*)(void* ecx, void* edx);
static fn_ActivateChild_t g_TurretActivate    = nullptr;
static fn_ActivateChild_t g_AimerActivate     = nullptr;
static fn_ActivateChild_t g_PassengerActivate = nullptr;
static void*              s_origActivatePhysics = nullptr;

static void activateSubObject(char* obj, int priority)
{
   typedef void(__thiscall* fn_t)(void*, int, int);
   ((fn_t)(*(void***)obj)[2])(obj, priority, 2);
}

static void __fastcall carrier_ActivatePhysics(void* ecx, void* /*edx*/)
{
   char* base = (char*)ecx;
   activateSubObject(base + kControllableBase, -1);
   activateSubObject(base + L->postCollision, -15);

   char* cls = carrierClass(base);
   if (cls) {
      int n = fieldI(cls, L->clsWeaponCount);
      void** aimers = (void**)(base + L->aimers);
      for (int i = 0; i < n; i++)
         if (aimers[i]) g_AimerActivate(aimers[i], nullptr);
   }

   int turretCount = *(signed char*)(base + L->turretCount);
   void** turrets = (void**)(base + L->turrets);
   for (int i = 0; i < turretCount; i++)
      if (turrets[i]) g_TurretActivate(turrets[i], nullptr);

   if (cls) {
      int n = *(unsigned char*)(cls + L->clsPassengerCount);
      void** passengers = (void**)(base + L->passengers);
      for (int i = 0; i < n; i++)
         if (passengers[i]) g_PassengerActivate(passengers[i], nullptr);
   }
}

// ---------------------------------------------------------------------------
// EntityFlyer::TakeOff
//   __thiscall(EntityFlyer* base)
//
// For carriers: blocked while LANDING (the flyer AI calls it mid-descent and
// yanks the carrier back up).  From LANDED after a drop, snapshot the transform
// and start the forward ascent.
// ---------------------------------------------------------------------------

using fn_TakeOff_t = void(__fastcall*)(void* ecx, void* edx);
static fn_TakeOff_t original_TakeOff = nullptr;

static void __fastcall hooked_TakeOff(void* ecx, void* /*edx*/)
{
   char* base = (char*)ecx;
   __try {
      if (*(void**)base == g_carrierVtable) {
         const int state = flightState(base);
         if (state == 3) return;

         CarrierTrack* t = (state == 0) ? trackFind(base) : nullptr;
         if (t && t->cargoDropped) {
            t->snapX = fieldF(base, kPosX);
            t->snapZ = fieldF(base, kPosZ);
            memcpy(t->snapRot, base + kMatrix, sizeof(t->snapRot));

            float fx = fieldF(base, kForward), fz = fieldF(base, kForward + 8);
            float len = sqrtf(fx * fx + fz * fz);
            if (len > 0.001f) { fx /= len; fz /= len; }
            t->fwdDirX = fx;
            t->fwdDirZ = fz;
            t->ascentElapsed = 0.0f;
            t->ascentActive  = true;
         }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {}
   original_TakeOff(ecx, nullptr);
}

// ---------------------------------------------------------------------------
// EntityCarrier::Update
//   __thiscall(base+0x240, float dt), RET 4, returns false when dead
// ---------------------------------------------------------------------------

using fn_CarrierUpdate_t = bool(__fastcall*)(void* ecx, void* edx, float dt);
static fn_CarrierUpdate_t original_CarrierUpdate = nullptr;

// During the post-drop ascent the movement controller swings the carrier onto
// a flight-path start pose.  Hold the rotation from the snapshot and move X/Z
// along the saved heading, ramping to TakeoffSpeed over 3 s; vanilla keeps
// driving Y.
static void applyAscentLock(CarrierTrack& t, char* base)
{
   if (!t.ascentActive) return;
   if (flightState(base) != 1) return;

   memcpy(base + kMatrix, t.snapRot, sizeof(t.snapRot));

   static constexpr float kRamp = 3.0f;
   const float spd = t.forwardSpeed, e = t.ascentElapsed;
   const float dist = (e <= kRamp) ? spd * e * e / (2.0f * kRamp)
                                   : spd * kRamp / 2.0f + spd * (e - kRamp);
   fieldF(base, kPosX) = t.snapX + t.fwdDirX * dist;
   fieldF(base, kPosZ) = t.snapZ + t.fwdDirZ * dist;
}

// Descent: from wherever LANDING starts, smoothstep X/Y/Z onto the pad over
// LandingTime.  Y targets padY + instance LandedHeight - 1 so vanilla's own
// ground check (groundDistance < LandedHeight) fires directly over the pad.
static void updateFlight(CarrierTrack& t, char* base, float dt)
{
   int state = flightState(base);
   const int prev = t.lastState;
   t.lastState = state;

   if (state == 3 && prev != 3 && !t.landActive) {
      t.landStartX  = fieldF(base, kPosX);
      t.landStartY  = fieldF(base, kPosY);
      t.landStartZ  = fieldF(base, kPosZ);
      t.landElapsed = 0.0f;
      t.landTargetY = t.padY + fieldF(base, L->landedHeight) - 1.0f;
      t.landActive  = true;
   }

   if (state == 3 && t.landActive) {
      t.landElapsed += dt;
      float u = t.landElapsed / t.descentDuration;
      if (u > 1.0f) u = 1.0f;
      const float e = u * u * (3.0f - 2.0f * u);
      fieldF(base, kPosX) = t.landStartX + (t.padX - t.landStartX) * e;
      fieldF(base, kPosY) = t.landStartY + (t.landTargetY - t.landStartY) * e;
      fieldF(base, kPosZ) = t.landStartZ + (t.padZ - t.landStartZ) * e;
   }

   if (state != 3 && t.landActive) {
      t.landActive = false;
      if (state == 0) t.cargoDropped = true;
   }

   if (t.ascentActive) {
      if (state == 3) {                         // no re-landing after the drop
         fieldI(base, L->flightState) = 1;
         state = 1;
      }
      if (state == 1) t.ascentElapsed += dt;
      else            t.ascentActive = false;
   }
}

// Cargo that should no longer be on the carrier:
//  - LANDED: vanilla (UpdateLive) only drops slot 0, so drop slots 1..3 here.
//  - TAKEOFF/FLYING with cargo still attached: the landing aborted (slope over
//    ~20 degrees, water, DenyFlyerLand) or something took off without the
//    drop.  UpdateLive deletes the carrier as soon as it reaches FLYING and the
//    destructor does not detach, which would leave the cargo with a dangling
//    CollisionObject::mParent and our team-0 park.  Drop it here, near the
//    ground, instead.
static void dropStrandedCargo(char* base)
{
   const int state = flightState(base);
   if (state == 0)                 detachAllCargo(base, 1);
   else if (state == 1 || state == 2) detachAllCargo(base, 0);
}

// Our position writes bypass EntityFlyer::SetPosition, which is what normally
// moves the scene bounding sphere.  Keep it on the pivot with a generous radius
// so the carrier doesn't get culled.
static void syncBoundingSphere(char* base)
{
   fieldF(base, kSphere + 0) = fieldF(base, kPosX);
   fieldF(base, kSphere + 4) = fieldF(base, kPosY);
   fieldF(base, kSphere + 8) = fieldF(base, kPosZ);
   if (fieldF(base, kSphere + 12) < 80.0f) fieldF(base, kSphere + 12) = 80.0f;
}

static bool __fastcall hooked_CarrierUpdate(void* ecx, void* /*edx*/, float dt)
{
   char* base = (char*)ecx - kControllableBase;
   CarrierTrack* t = trackFind(base);

   bool neutralised = false;
   __try {
      if (t) {
         applyAscentLock(*t, base);
         const float threshold = (t->landedHt * 2.0f > 10.0f) ? t->landedHt * 2.0f : 10.0f;
         if (fieldF(base, kPosY) - t->padY > threshold) {
            rayHitSet(true);
            neutralised = true;
         }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {}

   const bool alive = original_CarrierUpdate(ecx, nullptr, dt);
   if (neutralised) rayHitSet(false);

   if (!alive) {
      if (t) *t = {};
      return false;
   }

   __try {
      if (t) {
         applyAscentLock(*t, base);
         updateFlight(*t, base, dt);
      }
      dropStrandedCargo(base);
      syncBoundingSphere(base);
   } __except (EXCEPTION_EXECUTE_HANDLER) {}
   return true;
}

// ---------------------------------------------------------------------------
// VehicleSpawn::UpdateSpawn
//
//   modtools : ECX = this, dt at [EBP+8], RET 4
//   release  : ECX = this, dt in XMM1,   bare RET
// The thunks below bridge the release convention so the C hook stays shared.
// ---------------------------------------------------------------------------

using fn_UpdateSpawn_t = void(__fastcall*)(void* ecx, void* edx, float dt);
static fn_UpdateSpawn_t original_UpdateSpawn = nullptr;

static void __fastcall hooked_UpdateSpawn(void* ecx, void* edx, float dt);

static __declspec(naked) void hooked_UpdateSpawn_regcall()
{
   __asm {
      sub   esp, 4
      movss dword ptr [esp], xmm1     // dt -> stack arg (ECX already = this)
      call  hooked_UpdateSpawn        // __fastcall: callee pops the float
      ret
   }
}

static __declspec(naked) void __fastcall call_orig_UpdateSpawn_regcall(void*, void*, float)
{
   __asm {
      movss xmm1, dword ptr [esp + 4]
      call  dword ptr [original_UpdateSpawn]
      ret   4
   }
}

static void call_original_UpdateSpawn(void* ecx, float dt)
{
   if (L->updateSpawnRegcall) call_orig_UpdateSpawn_regcall(ecx, nullptr, dt);
   else                       original_UpdateSpawn(ecx, nullptr, dt);
}

// The pad's carrier is only ever handled from VehicleSpawn::UpdateLive, which
// runs once per LIVE VehicleTracker.  If the carried cargo was destroyed its
// tracker is gone, so nothing drops, launches or deletes the carrier: it sits
// on the pad forever and UpdateSpawn bails at its first line because mCarrier
// is still valid, so the pad never spawns again.  VehicleSpawn::Update calls
// UpdateSpawn exactly when trackers < mSpawnCount, i.e. in that case, after
// UpdateLive has had its turn, so running UpdateLive's carrier block here is
// safe to repeat and fills the gap.
static void padCarrierUpdate(char* vs)
{
   __try {
      char* carrier = *(char**)(vs + kVS_CarrierPtr);
      if (!carrier) return;
      if (fieldI(carrier, kHandleId) != fieldI(vs, kVS_CarrierGen)) return;  // original clears it
      if (*(void**)carrier != g_carrierVtable) return;

      const int state = flightState(carrier);
      if (state == 0) {
         detachAllCargo(carrier);
         hooked_TakeOff(carrier, nullptr);
      } else if (state == 2) {
         detachAllCargo(carrier);
         trackRelease(carrier);
         typedef void(__thiscall* Dtor_t)(void*, int);
         ((Dtor_t)(*(void***)carrier)[kVt_DeletingDtor])(carrier, 1);
         fieldI(vs, kVS_CarrierPtr) = 0;
         fieldI(vs, kVS_CarrierGen) = 0;
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Allocate and link a VehicleTracker for an extra cargo entity.
typedef void* (__thiscall* fn_MemPoolAlloc_t)(void* pool, unsigned int size);
static fn_MemPoolAlloc_t g_MemPoolAlloc = nullptr;
static void*             g_VehicleTrackerPool = nullptr;

static void createTracker(char* vs, void* cargo)
{
   if (!g_MemPoolAlloc || !g_VehicleTrackerPool || !cargo) return;
   int* tracker = (int*)g_MemPoolAlloc(g_VehicleTrackerPool, 0x1C);
   if (!tracker) return;
   memset(tracker, 0, 0x1C);

   char* sentinel = vs + kVS_ListSentinel;
   int*  sentinelI = (int*)sentinel;
   tracker[0] = (int)sentinel;
   tracker[1] = (int)sentinel;
   tracker[3] = (int)tracker;
   tracker[4] = (int)cargo;                        // mVehicle ptr
   tracker[5] = fieldI((char*)cargo, kHandleId);   // mVehicle id

   tracker[2] = sentinelI[2];                      // link at head
   sentinelI[2] = (int)tracker;
   *(int*)(tracker[2] + 4) = (int)tracker;
   fieldI(vs, kVS_TrackerCount) += 1;
}

// Spawn cargo for slots 1..N-1 of a multi-cargo carrier, within the pad's
// remaining spawn budget.  Same order as vanilla: attach, team, activate,
// tracker.
static void spawnExtraCargo(char* vs, char* carrier, CarrierTrack* t, int team, int countAfter)
{
   char* cls = carrierClass(carrier);
   if (!cls) return;
   const int cargoCount = fieldI(cls, L->clsCargoCount);
   if (cargoCount <= 1) return;

   int slotsToFill = cargoCount;
   const int budget = fieldI(vs, kVS_SpawnCount) - countAfter;
   if (slotsToFill > budget + 1) slotsToFill = budget + 1;
   if (slotsToFill > kMaxCargo)  slotsToFill = kMaxCargo;
   if (slotsToFill <= 1) return;

   void* spawnClass = *(void**)(vs + kVS_SpawnClass + team * 4);
   if (!spawnClass) return;
   auto fn = get_gamelog();

   for (int slot = 1; slot < slotsToFill; slot++) {
      typedef void* (__thiscall* SpawnEntity_t)(void* cls, void* transform);
      typedef void* (__thiscall* GetEntity_t)(void* obj);
      void* spawned = ((SpawnEntity_t)(*(void***)spawnClass)[2])(spawnClass, vs + kVS_PadTransform);
      if (!spawned) continue;
      void* cargo = ((GetEntity_t)(*(void***)spawned)[9])(spawned);
      if (!cargo) continue;

      if (!attachCargoToSlot(carrier, slot, cargo)) {
         if (fn) fn("[Carrier] slot %d: attach failed\n", slot);
         continue;
      }

      setTeam(cargo, team);
      int* bits = (int*)((char*)cargo + kTeamBits);
      *bits ^= ((team << 4) ^ *bits) & 0xF0;
      *bits ^= ((team << 8) ^ *bits) & 0xF00;
      if (t) saveCargoTeam(*t, slot, cargo);

      // The engine's activation path dereferences Controllable::mPilot with no
      // null check on one branch and carried cargo has no pilot; the guard in
      // hover_pilot_null_fix.cpp covers it.  Marked expected so the VEH logger
      // doesn't file a report if that guard declined to install.
      crash_logger_begin_expected_fault();
      __try {
         typedef void(__thiscall* Activate_t)(void*);
         ((Activate_t)(*(void***)cargo)[kVt_Activate])(cargo);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         static bool s_reported = false;
         if (fn && !s_reported) {
            s_reported = true;
            fn("[Carrier] cargo activation faulted (slot %d); the mPilot guard did not install\n", slot);
         }
      }
      crash_logger_end_expected_fault();

      createTracker(vs, cargo);
   }
}

static void __fastcall hooked_UpdateSpawn(void* ecx, void* /*edx*/, float dt)
{
   char* vs = (char*)ecx;
   padCarrierUpdate(vs);

   int countBefore = 0;
   __try { countBefore = fieldI(vs, kVS_TrackerCount); } __except (EXCEPTION_EXECUTE_HANDLER) {}

   call_original_UpdateSpawn(ecx, dt);

   __try {
      const int countAfter = fieldI(vs, kVS_TrackerCount);
      if (countAfter <= countBefore) return;

      const int team = fieldI(vs, kVS_Team);
      if (team < 1 || team > 7 || !*(vs + kVS_UseCarrier + team)) return;

      char* carrier = *(char**)(vs + kVS_CarrierPtr);
      if (!carrier || fieldI(carrier, kHandleId) != fieldI(vs, kVS_CarrierGen)) return;
      if (*(void**)carrier != g_carrierVtable) return;

      CarrierTrack* t = trackAcquire(carrier);
      if (t) {
         const float* pad = (const float*)(vs + kVS_PadTransform);
         t->padX = pad[12];
         t->padY = pad[13];
         t->padZ = pad[14];

         char* cls = carrierClass(carrier);
         const float landingTm = cls ? fieldF(cls, L->clsLandingTime)  : 10.0f;
         const float speed     = cls ? fieldF(cls, L->clsTakeoffSpeed) : 20.0f;
         t->descentDuration = (landingTm > 2.0f) ? landingTm : 2.0f;
         t->forwardSpeed    = (speed > 1.0f) ? speed : 1.0f;
         t->landedHt        = cls ? fieldF(cls, L->clsLandedHeight) : 5.0f;

         // Slot 0 was attached inside the original, before we tracked it.
         if (void* cargo0 = slotCargo(carrier, 0)) saveCargoTeam(*t, 0, cargo0);
      }

      spawnExtraCargo(vs, carrier, t, team, countAfter);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (auto fn = get_gamelog()) fn("[Carrier] exception after UpdateSpawn\n");
   }
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

static bool s_flightHooked     = false;
static bool s_turretAIHooked   = false;
static void** s_activatePhysicsSlot = nullptr;
static void*  s_updateSpawnDetour   = nullptr;

void entity_carrier_fixes_install(uintptr_t exe_base)
{
   if (g_build == GameBuild::Modtools)                              L = &kCarrierModtools;
   else if (g_build == GameBuild::Steam || g_build == GameBuild::GOG) L = &kCarrierRelease;
   else return;

   if (!g_addr->carrier_set_property || !g_addr->carrier_attach_cargo ||
       !g_addr->carrier_detach_cargo || !g_addr->carrier_vtable)
      return;

   original_SetProperty = (fn_SetProperty_t)resolve(exe_base, g_addr->carrier_set_property);
   original_AttachCargo = (fn_AttachCargo_t)resolve(exe_base, g_addr->carrier_attach_cargo);
   original_DetachCargo = (fn_DetachCargo_t)resolve(exe_base, g_addr->carrier_detach_cargo);
   g_carrierVtable      = resolve(exe_base, g_addr->carrier_vtable);

   if (g_addr->gameloop_pause_mode)
      g_pauseMode = (uint8_t*)resolve(exe_base, g_addr->gameloop_pause_mode);

   s_renderHooked = g_addr->flyer_render != 0;
   if (s_renderHooked)
      original_FlyerRender = (fn_FlyerRender_t)resolve(exe_base, g_addr->flyer_render);

   s_flightHooked = g_addr->carrier_update_spawn && g_addr->carrier_update && g_addr->carrier_take_off;
   if (s_flightHooked) {
      original_UpdateSpawn   = (fn_UpdateSpawn_t)  resolve(exe_base, g_addr->carrier_update_spawn);
      original_CarrierUpdate = (fn_CarrierUpdate_t)resolve(exe_base, g_addr->carrier_update);
      original_TakeOff       = (fn_TakeOff_t)      resolve(exe_base, g_addr->carrier_take_off);
      if (g_addr->mem_pool_alloc)
         g_MemPoolAlloc = (fn_MemPoolAlloc_t)resolve(exe_base, g_addr->mem_pool_alloc);
      if (g_addr->vehicle_tracker_pool)
         g_VehicleTrackerPool = resolve(exe_base, g_addr->vehicle_tracker_pool);
   }

   codePatchesInit();

   s_turretAIHooked = g_addr->turret_update_indirect && g_addr->trigger_update;
   if (s_turretAIHooked) {
      original_TurretUpdateIndirect =
         (fn_TurretUpdateIndirect_t)resolve(exe_base, g_addr->turret_update_indirect);
      g_FireStateMachine = resolve(exe_base, g_addr->trigger_update);
   }

   s_updateSpawnDetour = L->updateSpawnRegcall ? (void*)hooked_UpdateSpawn_regcall
                                               : (void*)hooked_UpdateSpawn;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   DetourAttach(&(PVOID&)original_AttachCargo, hooked_AttachCargo);
   DetourAttach(&(PVOID&)original_DetachCargo, hooked_DetachCargo);
   if (s_renderHooked)
      DetourAttach(&(PVOID&)original_FlyerRender, hooked_FlyerRender);
   if (s_flightHooked) {
      DetourAttach(&(PVOID&)original_UpdateSpawn,   s_updateSpawnDetour);
      DetourAttach(&(PVOID&)original_CarrierUpdate, hooked_CarrierUpdate);
      DetourAttach(&(PVOID&)original_TakeOff,       hooked_TakeOff);
   }
   if (s_turretAIHooked)
      DetourAttach(&(PVOID&)original_TurretUpdateIndirect, hooked_TurretUpdateIndirect);
   DetourTransactionCommit();

   // After the Detours commit, to avoid page-protection conflicts.
   if (g_addr->turret_activate && g_addr->aimer_activate && g_addr->passenger_activate) {
      g_TurretActivate    = (fn_ActivateChild_t)resolve(exe_base, g_addr->turret_activate);
      g_AimerActivate     = (fn_ActivateChild_t)resolve(exe_base, g_addr->aimer_activate);
      g_PassengerActivate = (fn_ActivateChild_t)resolve(exe_base, g_addr->passenger_activate);

      void** slot = (void**)g_carrierVtable + kVt_ActivatePhysics;
      DWORD oldProt;
      if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
         s_origActivatePhysics = *slot;
         *slot = (void*)&carrier_ActivatePhysics;
         VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
         s_activatePhysicsSlot = slot;
      }
   }

   turretFireInstall();
   createCtrlInstall();
}

void entity_carrier_fixes_uninstall()
{
   if (!original_SetProperty) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_SetProperty, hooked_SetProperty);
   DetourDetach(&(PVOID&)original_AttachCargo, hooked_AttachCargo);
   DetourDetach(&(PVOID&)original_DetachCargo, hooked_DetachCargo);
   if (s_renderHooked)
      DetourDetach(&(PVOID&)original_FlyerRender, hooked_FlyerRender);
   if (s_flightHooked) {
      DetourDetach(&(PVOID&)original_UpdateSpawn,   s_updateSpawnDetour);
      DetourDetach(&(PVOID&)original_CarrierUpdate, hooked_CarrierUpdate);
      DetourDetach(&(PVOID&)original_TakeOff,       hooked_TakeOff);
   }
   if (s_turretAIHooked)
      DetourDetach(&(PVOID&)original_TurretUpdateIndirect, hooked_TurretUpdateIndirect);
   DetourTransactionCommit();

   if (s_activatePhysicsSlot) {
      DWORD oldProt;
      if (VirtualProtect(s_activatePhysicsSlot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
         *s_activatePhysicsSlot = s_origActivatePhysics;
         VirtualProtect(s_activatePhysicsSlot, sizeof(void*), oldProt, &oldProt);
      }
      s_activatePhysicsSlot = nullptr;
   }
   if (s_turretFireSite) {
      writeCode(s_turretFireSite, s_turretFireSaved, s_turretFireLen);
      s_turretFireSite = nullptr;
   }
   if (s_createCtrlSite) {
      writeCode(s_createCtrlSite, s_createCtrlSaved, kCreateCtrlPatch_len);
      s_createCtrlSite = nullptr;
   }
   original_SetProperty = nullptr;
   for (CarrierTrack& t : g_tracks) t = {};
}
