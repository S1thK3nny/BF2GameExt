#include "pch.h"
#include "grappling_hook.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>
#include <string.h>

// =============================================================================
// Grappling Hook
//
// The engine ships a complete grappling hook that nothing ever uses.  Read
// against the Phantom build, where the whole class is symbolized, the design is
// finished rather than half-written:
//
//   OrdnanceGrapplingHookClass  ODF class label "grapplinghook"
//   WeaponGrapplingHookClass    ODF class label "grapplinghookweapon"
//
//   enum GrapplingHookState { HOOK_ATTACHING=0, HOOK_WAITING=1, HOOK_LIFTING=2,
//                             HOOK_HANGING=3, HOOK_ABORTING=4 };
//
//   ctor      captures the weapon fire point in soldier-local space
//             (mWeaponOffset) and calls EntitySoldier::SetGrapplingHook(this),
//             which stores the ordnance in EntitySoldier::mHook.
//   Collide   accepts only collision categories 8 and 0x10 (the "solid world"
//             mask); anything else calls HookFailure(), which flips the state to
//             HOOK_ABORTING, negates the velocity and flies the hook back to the
//             weapon.  On a good hit it records the hit body and the hit point in
//             that body's local space - so the hook tracks a moving object - takes
//             the ordnance out of the collision manager, forces the lifespan to
//             20 s, halves the velocity, and removes the soldier's collision body.
//   Update    re-derives the hook position from the hit body's matrix every frame
//             and re-aims the velocity at it, then runs the state machine.
//             HOOK_WAITING counts mWaitTime (0.4 s) down and switches to
//             HOOK_LIFTING.
//   the pull  is NOT in Update.  EntitySoldier::Move and ::MoveOffGround test
//             mHook: when it is set they skip gravity entirely and call
//             OrdnanceGrapplingHook::MoveSoldier, which advances the soldier's
//             collision object by dt * ordnanceVelocity, plays the class's
//             mAttachSoldierAnimation and zeroes the velocity.  So the pull speed
//             IS the ODF ordnance velocity, halved on impact - there is no
//             separate speed property to add.
//   arrival   is a horizontal (X/Z only) test against twice the soldier radius.
//             It re-adds the soldier as a soft body, zeroes the velocity, stands
//             the soldier up, and returns false so the ordnance is destroyed.
//
// Update also requires the weapon's fire trigger to still be held
// (mFireWeapon->mTrigger, weapon+0x74, bit 0).  Releasing it during HOOK_LIFTING
// drops through to the arrival path; releasing during flight destroys the hook
// outright.  Firing is a tap, so that gate kills every shot - see fix 5.
// WeaponGrapplingHook::CheckFire refuses to fire again while mCableOut is set,
// so one hook at a time.
//
// One ODF property is parsed by the engine, on the ordnance class:
//
//   SoldierAnimation = "<clip>"      hash 0x5F0CE10D, class+0x16C
//
// It is fed to SoldierAnimationBank::AddAnimation and played on the soldier for
// the whole pull.  It defaults to INVALID_ANIMATION (-1).  Two more are added
// here for the cable, which the engine gives no way to reach:
//
//   CableTexture = "<texture>"       cable texture, default com_bldg_minigun
//
// -----------------------------------------------------------------------------
// What is actually broken, and what this file fixes
//
// 1. The cable draws untextured.
//    The displayable vtable at ordnance+0x98 (0x00A50D40) has the real cable
//    renderer in slot 19 (0x006D14D0): a camera-facing ribbon with
//    clamp((int)(len*8), 2, 80) segments, drawn through the static sShader that
//    OrdnanceGrapplingHook::PlatformInit builds from the "Particle" shader def
//    plus sTextureHash.  In the Phantom build PlatformInit opens with
//    sTextureHash = PblHash("com_bldg_minigun"); every shipped build dropped that
//    line, so sTextureHash stays 0.  Writing a hash back is the whole fix.  The
//    shader resolves its texture lazily at draw time, so the name has to be one the
//    map already loads -- see kCableTexture.
//
// 2. MoveSoldier plays an invalid animation.
//    With no SoldierAnimation in the ODF, mAttachSoldierAnimation is
//    INVALID_ANIMATION and MoveSoldier still drives the animator with it every
//    frame of the pull.  When the property is absent we run the move ourselves and
//    skip only that call.
//
// 3. The soldier can be left with no collision body.
//    Collide removes it and only the arrival path puts it back, so any other way
//    out of HOOK_WAITING / HOOK_LIFTING - the ordnance pool recycling, the soldier
//    dying, a map change - leaves the soldier with nothing to stand on.  The
//    destructor also skips EntitySoldier::SetGrapplingHook(NULL) entirely when
//    mFireWeapon is NULL, which leaves mHook dangling at freed memory for
//    EntitySoldier::Move to call into on the next frame.  Both are closed below.
//
// 4. Update dereferences mFireWeapon without checking it.
//    mFireWeapon is NULL whenever the firing weapon is not a WeaponGrapplingHook,
//    which an ODF can arrange by pointing an ordinary cannon at a grapple
//    ordnance.  Update then reads mFireWeapon->mTrigger through a null pointer.
//
// 5. A tapped shot dies before it goes anywhere.
//    The trigger gate above cancels the hook on the first frame the fire button is
//    not down, and firing a weapon in this game is a tap: press and release both
//    land while the hook is still in the air.  Update is therefore shown a held
//    trigger for the whole flight and pull, with the value restored straight
//    afterwards so nothing else sees the change.
//
// 6. Nothing is drawn but the cable.
//    Slot 19 of the grapple's displayable vtable is the cable renderer, and it
//    never chains to the base render, so the hook head is never on screen at all.
//    OrdnanceBullet::Render is called alongside it, which puts the ODF's
//    GeometryName up on a matrix built from the ordnance velocity.
//
// 7. Firing while airborne stops the soldier dead.
//    MoveSoldier ends every call with SetVelocity(zero), from the frame the hook
//    is fired, so a soldier who fires mid-fall hangs in the air for the whole
//    flight.  Gravity is restored until the hook actually attaches.
//
// 8. A hook outlives the soldier who fired it.
//    mFireWeapon dangles once the firer dies, and Update reaches through it before
//    the engine's own staleness check gets a look in.
//
// 9. Retracting an attached hook strands the soldier.
//    While mCollBodyID is set the ordnance position is re-derived from the hit
//    body every frame, so it can never fly home to be destroyed, and the soldier
//    it took the collision body from waits forever.
//
// 10. A retracting hook flies home backwards.
//    HookFailure negates the velocity, and the model is oriented along it.
//
// 11. The cable anchor is captured once and never moves with the weapon.
//
// 12. Letting go fires a fresh hook on the same press.
//    WeaponCannon::Fire sets mCableOut, so CheckFire correctly refuses a second
//    hook while one is out -- but the let-go press destroys the ordnance, whose
//    destructor clears mCableOut in the same frame.  The button is still down on
//    the next one, so the weapon fires again immediately and the player is yanked
//    off by a new hook instead of flying free.
//
// 13. A hook survives boarding a vehicle.
//    Nothing tells the ordnance its soldier has climbed into something, so the
//    hook stays put and pulls them back to it the moment they get out.
//
// 14. The hook will not stick to terrain.
//    Collide accepts COLL_RIGID and COLL_STATIC only, so the ground throws it off.
//
// The one thing added on top is what a second press of fire does while the hook
// is out.  CheckFire refuses to fire again while the cable is out, so that press
// is inert and free to mean something else: mid-pull it drops the soldier off
// carrying the speed they had built up, and before that it reels the hook back in
// rather than leaving the player waiting out the twenty second lifespan.  The
// press is read from Trigger::Update, because trigger state is consumed within
// the frame it is produced and is already back to idle by the time any ordnance
// updates.
//
// TODO: When cancelling mid air and instantly reshooting (this shouldn't even be possible) the hook goes backwards, disappears and the player remains frozen until respawning.
//
// TODO: Rework slingshotting.
//
// TODO: Make animation direction-aware.
//
// Everything else is the engine's, running unmodified.
// =============================================================================

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

// OrdnanceGrapplingHook (356 bytes).  Identical in the modtools and Phantom
// builds, so these are the PDB field offsets.
static constexpr int kOrd_ClassPtr    = 0x030;  // OrdnanceGrapplingHookClass*
static constexpr int kOrd_SoldierPtr  = 0x054;  // PblHandle<GameObject>.mObject
static constexpr int kOrd_SoldierKey  = 0x058;  // PblHandle<GameObject>.mSavedHandleId
static constexpr int kOrd_VelX        = 0x0FC;
static constexpr int kOrd_State       = 0x12C;  // GrapplingHookState
static constexpr int kOrd_FireWeapon  = 0x130;  // WeaponGrapplingHook*
static constexpr int kOrd_HitObject   = 0x134;  // PblHandle<GameObject> mHitObject
static constexpr int kOrd_HitObjectKey= 0x138;
static constexpr int kOrd_CollBodyID  = 0x13C;  // -1 = not stuck to anything

// OrdnanceGrapplingHookClass::mAttachSoldierAnimation.
static constexpr int kOrdClass_Anim   = 0x16C;

// OrdnanceGrapplingHook::mWeaponOffset - the cable's anchor on the soldier, in the
// soldier's collision space.  The constructor fills it in once and Render has used
// it ever since.
static constexpr int kOrd_WeaponOffset = 0x14C;

// Weapon::mAimer and Weapon::mTrigger.  Byte 0 bit 0 of a Trigger is "held".
static constexpr int kWeapon_Aimer    = 0x070;
static constexpr int kWeapon_Trigger  = 0x074;

// Aimer::mFirePos - the live fire point, and the field BarrelFireOrigin relocates
// to the barrel hardpoint.
static constexpr int kAimer_FirePos   = 0x088;


// The displayable sub-object Render is called on, relative to the ordnance.
static constexpr int kOrd_DisplayableOff = 0x098;

// EntitySoldier.  The soldier struct is shifted +4 against Phantom around mHook,
// so these are the modtools values.
static constexpr int kSol_CollObject  = 0x00C;  // CollisionObject sub-object
static constexpr int kSol_HandleKey   = 0x204;
static constexpr int kSol_Hook        = 0x454;  // OrdnanceGrapplingHook* mHook
static constexpr int kSol_VelY        = 0x4F0;  // mVelocity.y
static constexpr int kSol_Flags       = 0x9E4;  // bit 1 = standing on something

// The gravity EntitySoldier::Move applies when the soldier has no hook.
static constexpr float kGravity = -18.0f;

// Vtable slots, in bytes.
static constexpr int kCollVt_SetPosition = 0x08;
static constexpr int kCollVt_GetMatrix   = 0x40;
static constexpr int kGoVt_IsRtti        = 0x00;
static constexpr int kGoVt_SetVelocity   = 0x48;

static constexpr int kState_Attaching  = 0;
static constexpr int kState_Aborting   = 4;
static constexpr int kState_Waiting    = 1;
static constexpr int kState_Lifting    = 2;
static constexpr int kInvalidAnimation = -1;

// ---------------------------------------------------------------------------
// Function types
// ---------------------------------------------------------------------------

typedef unsigned int (__fastcall* fn_Update_t)(void* ecx, void* edx, float dt);
typedef void (__fastcall* fn_Dtor_t)(void* ecx, void* edx);
typedef void (__fastcall* fn_MoveSoldier_t)(void* ecx, void* edx, void* soldier, float dt);
typedef void (__fastcall* fn_Render_t)(void* ecx, void* edx, uint32_t lod, float f, uint32_t flags);
typedef void (__fastcall* fn_HookFailure_t)(void* ecx, void* edx);
typedef void (__fastcall* fn_SetProperty_t)(void* ecx, void* edx, uint32_t hash,
                                            const char* value);
typedef void (__cdecl* fn_PlatformFn_t)();
typedef bool (__fastcall* fn_CheckFire_t)(void* ecx, void* edx);
typedef bool (__fastcall* fn_EnterControllable_t)(void* ecx, void* edx, void* target);
typedef void (__fastcall* fn_TriggerUpdate_t)(uint32_t* trigger, void* edx, uint32_t dt,
                                              char buttonDown);
typedef bool (__fastcall* fn_IsInCollisionList_t)(void* coll, void* edx);
typedef void (__fastcall* fn_AddSoftBody_t)(void* coll, void* edx);

typedef bool   (__fastcall* fn_IsRtti_t)(void* ecx, void* edx, uint32_t hash);
typedef float* (__fastcall* fn_GetMatrix_t)(void* ecx, void* edx);
typedef void   (__fastcall* fn_SetPosition_t)(void* ecx, void* edx, const float* pos);
typedef void   (__fastcall* fn_SetVelocity_t)(void* ecx, void* edx, const float* vel);

// ---------------------------------------------------------------------------
// Resolved pointers
// ---------------------------------------------------------------------------

static fn_Update_t      original_Update      = nullptr;
static fn_Dtor_t        original_Dtor        = nullptr;
static fn_MoveSoldier_t original_MoveSoldier = nullptr;
static fn_Render_t      original_Render      = nullptr;
static fn_Render_t      fn_BulletRender      = nullptr;
static fn_HookFailure_t   fn_HookFailure          = nullptr;
static fn_SetProperty_t   original_SetProperty     = nullptr;
static fn_PlatformFn_t    fn_PlatformInit          = nullptr;
static fn_PlatformFn_t    fn_PlatformCleanup       = nullptr;
static fn_CheckFire_t     original_CheckFire        = nullptr;
static fn_EnterControllable_t original_EnterControllable = nullptr;
static fn_TriggerUpdate_t original_TriggerUpdate = nullptr;

static fn_IsInCollisionList_t fn_IsInCollisionList = nullptr;
static fn_AddSoftBody_t       fn_AddSoftBody       = nullptr;
static uint32_t*              g_soldierRttiHash    = nullptr;

static bool g_installed = false;

// ---------------------------------------------------------------------------
// Cable texture
// ---------------------------------------------------------------------------

// PblHash: FNV-1a over the lowercased bytes.  Computed here rather than through
// the engine's PblHash because grapple_install() runs from dllmain, where the
// exe's sections are not executable yet.
static uint32_t pbl_hash(const char* s)
{
   uint32_t h = 0x811c9dc5u;
   for (; *s; ++s) {
      h ^= (uint32_t)(uint8_t)(*s | 0x20);
      h *= 0x01000193u;
   }
   return h;
}

// The Phantom build's own choice.  Overridable per ODF with CableTexture, which is
// the better answer than picking for the modder: the shader resolves its texture
// lazily at draw time, so any name works as long as the map loads it.
//
// Widening the cable does not help it read from the side.  sCableThickness
// (0x00AD3D94, 0.02) is only the ribbon's half width: the strip is built from the
// cable direction crossed with a fixed world axis, not with anything from the
// camera - the camera fetch at the top of Render is dead, its result overwritten
// before use - so the cable is a flat plane and disappears edge-on however wide it
// is.  Making it read from every angle means orienting the strip to the camera, or
// drawing a second one crossed at ninety degrees, and either way the geometry has
// to be built here rather than by the engine.
static constexpr char kCableTexture[] = "com_bldg_minigun";

static uint32_t* g_cableTextureHash = nullptr;   // OrdnanceGrapplingHook::sTextureHash

// ---------------------------------------------------------------------------
// Fix 14: let the hook stick to terrain.
//
// Collide gates on the collision object's CollisionObjectType:
//
//   COLL_ITEM = 1, COLL_SOFT = 2, COLL_ASTEROID = 4, COLL_RIGID = 8,
//   COLL_STATIC = 16, COLL_ATTACHED = 32, COLL_SPECIAL = 64, COLL_TERRAIN = 128, ...
//
// and accepts only COLL_RIGID and COLL_STATIC, so buildings, props and vehicles
// hold but the ground does not -- the hook bounces straight off it and reels back.
// Terrain does reach Collide: OrdnanceBullet::Update sweeps with a mask of 0x90
// (COLL_STATIC | COLL_TERRAIN) and defers the hit for Collide to handle.
//
// Nothing downstream needs terrain to be special.  A terrain CollisionObject has
// no owning GameObject, so Collide leaves mCollBodyID at -1 and the hook simply
// stays where it landed instead of tracking a body -- which is what a hook in the
// ground should do anyway.
//
//   0060F10D  83 F8 10     CMP EAX,0x10          ->  A9 98 00 00 00  TEST EAX,0x98
//   0060F110  74 17        JZ  accept            ->  75 15           JNZ accept
//   0060F112  83 F8 08     CMP EAX,8             ->  90 90 90        NOP
//   0060F115  74 12        JZ  accept
//
// Same ten bytes, and the fall-through to HookFailure is untouched.  The mask is
// exact for these three because a body carries a single type, never a union.
static const uint8_t kCollideOrig[10] = {
   0x83, 0xF8, 0x10, 0x74, 0x17, 0x83, 0xF8, 0x08, 0x74, 0x12,
};
static const uint8_t kCollidePatch[10] = {
   0xA9, 0x98, 0x00, 0x00, 0x00, 0x75, 0x15, 0x90, 0x90, 0x90,
};
static uint8_t* g_collideSite = nullptr;

// ---------------------------------------------------------------------------
// Hook: OrdnanceGrapplingHookClass::SetProperty
//
// One property on top of the engine's SoldierAnimation.  It reaches a static
// rather than the class, because the cable shader is built once at startup and
// shared by every grapple -- with more than one grapple ODF in a mod, the last one
// parsed wins.
//
//   CableTexture = "<texture>"    the cable's texture
// ---------------------------------------------------------------------------

static constexpr uint32_t kHashCableTexture = 0x237c85a9;  // PblHash("CableTexture")

static void __fastcall hooked_SetProperty(void* ecx, void* /*edx*/, uint32_t hash,
                                          const char* value)
{
   if (hash == kHashCableTexture && value && value[0]) {
      if (g_cableTextureHash && fn_PlatformInit && fn_PlatformCleanup) {
         *g_cableTextureHash = pbl_hash(value);
         // PlatformInit only reads sTextureHash when it builds the shader, so the
         // shader has to be thrown away and rebuilt for the change to land.  Class
         // properties are parsed at level load, when no grapple exists and nothing
         // holds a reference to it.
         __try {
            fn_PlatformCleanup();
            fn_PlatformInit();
         }
         __except (EXCEPTION_EXECUTE_HANDLER) {
         }
      }
      return;
   }

   original_SetProperty(ecx, nullptr, hash, value);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// The handle the ordnance holds on the firing soldier, re-validated the way the
// engine does it: a stale PblHandle keeps its pointer but the key no longer
// matches the object's own.  Null unless the object is still a live EntitySoldier.
static void* live_soldier(void* ordnance)
{
   char* ord = (char*)ordnance;

   __try {
      void* obj = *(void**)(ord + kOrd_SoldierPtr);
      int   key = *(int*)(ord + kOrd_SoldierKey);
      if (!obj) return nullptr;
      if (*(int*)((char*)obj + kSol_HandleKey) != key) return nullptr;

      void** vt = *(void***)obj;
      if (!vt || !g_soldierRttiHash) return nullptr;
      if (!((fn_IsRtti_t)vt[kGoVt_IsRtti / 4])(obj, nullptr, *g_soldierRttiHash))
         return nullptr;

      return obj;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return nullptr;
   }
}

// The Trigger the firing weapon reads, or null.  Bit 0 of its first byte is
// "button down", which is what Update gates the whole hook on.
static uint32_t* fire_trigger(void* ordnance)
{
   __try {
      void* weapon = *(void**)((char*)ordnance + kOrd_FireWeapon);
      if (!weapon) return nullptr;
      return *(uint32_t**)((char*)weapon + kWeapon_Trigger);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return nullptr;
   }
}

// Per-hook state.  Fixed and small: CheckFire allows one hook per weapon at a
// time, so this only ever holds one entry per soldier currently on a rope.
//
// The trigger byte cannot be sampled from Update.  Trigger state is consumed
// within the frame it is produced, well before ordnance updates run, so by the
// time Update sees the Trigger its low bits are back to idle whether the button
// is down or not.  The button is therefore read at source, from Trigger::Update.
struct HookState {
   void*     ordnance;
   uint32_t* trigger;    // the Trigger the firing weapon reads
   bool      lastDown;   // button state at the last Trigger::Update
   bool      pressed;    // a fresh press is waiting to be acted on
   bool      attached;   // Collide has taken the soldier's collision body away
   bool      killed;     // the soldier boarded something; drop the hook
};

static constexpr int kMaxTracked = 8;
static HookState     g_hooks[kMaxTracked];

// Weapons whose fire is held off until the player lets go of the button, so the
// press that released a hook cannot go on to fire the next one.  Only the trigger
// is ever dereferenced; the weapon is compared, never read.
struct FireHold {
   void*     weapon;
   uint32_t* trigger;
};
static FireHold g_fireHold[kMaxTracked];

static void hold_fire(void* weapon, uint32_t* trigger)
{
   if (!weapon) return;
   for (int i = 0; i < kMaxTracked; ++i)
      if (g_fireHold[i].weapon == weapon) return;
   for (int i = 0; i < kMaxTracked; ++i) {
      if (!g_fireHold[i].weapon) {
         g_fireHold[i].weapon  = weapon;
         g_fireHold[i].trigger = trigger;
         return;
      }
   }
}

static bool fire_is_held(void* weapon)
{
   for (int i = 0; i < kMaxTracked; ++i)
      if (g_fireHold[i].weapon == weapon) return true;
   return false;
}

static HookState* track_find(void* ordnance, uint32_t* trigger)
{
   HookState* freeSlot = nullptr;
   for (int i = 0; i < kMaxTracked; ++i) {
      if (g_hooks[i].ordnance == ordnance) return &g_hooks[i];
      if (!g_hooks[i].ordnance && !freeSlot) freeSlot = &g_hooks[i];
   }
   if (!trigger || !freeSlot) return nullptr;

   freeSlot->ordnance = ordnance;
   freeSlot->trigger  = trigger;
   // The hook only exists because the button went down this frame, so start from
   // "down" and let the release that follows arm the next press.
   freeSlot->lastDown = true;
   freeSlot->pressed  = false;
   freeSlot->attached = false;
   freeSlot->killed   = false;
   return freeSlot;
}

static void track_forget(void* ordnance)
{
   for (int i = 0; i < kMaxTracked; ++i)
      if (g_hooks[i].ordnance == ordnance) g_hooks[i] = HookState{};
}

// ---------------------------------------------------------------------------
// Hook: Trigger::Update
//
// Runs from PlayerController::Update with the raw button state, before anything
// consumes it.  Only the Triggers belonging to a live hook are of interest.
// ---------------------------------------------------------------------------

static void __fastcall hooked_TriggerUpdate(uint32_t* trigger, void* /*edx*/, uint32_t dt,
                                            char buttonDown)
{
   if (trigger) {
      const bool down = buttonDown != 0;
      for (int i = 0; i < kMaxTracked; ++i) {
         if (g_hooks[i].ordnance && g_hooks[i].trigger == trigger) {
            if (down && !g_hooks[i].lastDown) g_hooks[i].pressed = true;
            g_hooks[i].lastDown = down;
         }
      }
      // Fix 12: the button is up again, so the weapon may fire.
      if (!down) {
         for (int i = 0; i < kMaxTracked; ++i)
            if (g_fireHold[i].trigger == trigger) g_fireHold[i] = FireHold{};
      }
   }

   original_TriggerUpdate(trigger, nullptr, dt, buttonDown);
}

static int attach_animation(void* ordnance)
{
   __try {
      void* cls = *(void**)((char*)ordnance + kOrd_ClassPtr);
      if (!cls) return kInvalidAnimation;
      return *(int*)((char*)cls + kOrdClass_Anim);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return kInvalidAnimation;
   }
}

// Fix 11.  The constructor captures the fire point in the soldier's collision
// space once, and Render has anchored the cable there ever since, so the cable
// leaves whatever pose the weapon happened to be in at the moment of the shot.
// Recomputing it from the live Aimer::mFirePos each frame keeps the cable on the
// barrel hardpoint as the weapon moves - and picks up BarrelFireOrigin, which
// writes that same field.
//
// The collision matrix is a rigid transform, so inverting it is the transpose of
// the basis applied to the point less the translation; no engine call needed.
static void refresh_cable_anchor(void* ordnance, void* soldier)
{
   void* weapon = *(void**)((char*)ordnance + kOrd_FireWeapon);
   if (!weapon) return;
   void* aimer = *(void**)((char*)weapon + kWeapon_Aimer);
   if (!aimer) return;

   void*  coll   = (char*)soldier + kSol_CollObject;
   void** collVt = *(void***)coll;
   if (!collVt) return;
   const float* m = ((fn_GetMatrix_t)collVt[kCollVt_GetMatrix / 4])(coll, nullptr);
   if (!m) return;

   const float* firePos = (const float*)((char*)aimer + kAimer_FirePos);
   const float d[3] = {
      firePos[0] - m[12], firePos[1] - m[13], firePos[2] - m[14],
   };

   float* offset = (float*)((char*)ordnance + kOrd_WeaponOffset);
   offset[0] = d[0] * m[0] + d[1] * m[1]  + d[2] * m[2];
   offset[1] = d[0] * m[4] + d[1] * m[5]  + d[2] * m[6];
   offset[2] = d[0] * m[8] + d[1] * m[9]  + d[2] * m[10];
}

// ---------------------------------------------------------------------------
// Hook: WeaponGrapplingHook::CheckFire
//
// Fix 12.  The engine's own gate is mCableOut, which WeaponCannon::Fire sets and
// the ordnance destructor clears -- correct, but it clears in the same frame the
// let-go press destroys the hook, so the still-down button fires a new one on the
// very next frame.  Refuse until the button has actually come up.
// ---------------------------------------------------------------------------

static bool __fastcall hooked_CheckFire(void* ecx, void* /*edx*/)
{
   if (fire_is_held(ecx)) return false;
   return original_CheckFire(ecx, nullptr);
}

// ---------------------------------------------------------------------------
// Hook: EntitySoldier::EnterControllable
//
// Fix 13.  A hook left behind when its owner boards a vehicle keeps its claim on
// them: the ordnance sits where it landed and yanks them back to it the moment
// they climb out.  Boarding drops the hook.
// ---------------------------------------------------------------------------

static bool __fastcall hooked_EnterControllable(void* ecx, void* /*edx*/, void* target)
{
   for (int i = 0; i < kMaxTracked; ++i) {
      if (!g_hooks[i].ordnance) continue;
      if (live_soldier(g_hooks[i].ordnance) == ecx) g_hooks[i].killed = true;
   }

   return original_EnterControllable(ecx, nullptr, target);
}

// Whether the hook has taken the soldier's collision body, which is the point
// from which the soldier is ours to move rather than the engine's.
static bool hook_has_attached(void* ordnance, int state)
{
   if (state == kState_Waiting || state == kState_Lifting) return true;
   const HookState* tracked = track_find(ordnance, nullptr);
   return tracked && tracked->attached;
}

// ---------------------------------------------------------------------------
// Hook: OrdnanceGrapplingHook::MoveSoldier
//
// Fix 7 first: EntitySoldier::Move hands every frame to this function from the
// moment the hook is fired, and all of them end in SetVelocity(zero).  A soldier
// who fires while falling therefore stops dead in the air and hangs there for the
// whole flight.  Until the hook has attached the soldier still has their collision
// body and nothing needs to own their movement, so put back the gravity Move
// skipped on our behalf and leave them alone.  From the attach onwards the freeze
// is load-bearing -- Collide has taken the collision body away, and without it a
// soldier under gravity drops through the world.
//
// Then fix 2: with a SoldierAnimation in the ODF the engine's version is exactly
// what we want, so it runs untouched.  Without one this is the same function minus
// the animator call that would otherwise be handed INVALID_ANIMATION.
// ---------------------------------------------------------------------------

static void __fastcall hooked_MoveSoldier(void* ecx, void* /*edx*/, void* soldier, float dt)
{
   if (!soldier) return;

   bool attached = true;
   __try {
      attached = hook_has_attached(ecx, *(int*)((char*)ecx + kOrd_State));
      if (!attached) {
         if ((*(uint8_t*)((char*)soldier + kSol_Flags) & 2) == 0)
            *(float*)((char*)soldier + kSol_VelY) += dt * kGravity;
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return;
   }
   if (!attached) return;

   if (attach_animation(ecx) != kInvalidAnimation) {
      original_MoveSoldier(ecx, nullptr, soldier, dt);
      return;
   }

   __try {
      void*  coll   = (char*)soldier + kSol_CollObject;
      void** collVt = *(void***)coll;
      if (!collVt) return;

      if (*(int*)((char*)ecx + kOrd_State) == kState_Lifting) {
         const float* m =
            ((fn_GetMatrix_t)collVt[kCollVt_GetMatrix / 4])(coll, nullptr);
         if (m) {
            const float* vel = (const float*)((char*)ecx + kOrd_VelX);
            const float pos[3] = {
               m[12] + dt * vel[0],
               m[13] + dt * vel[1],
               m[14] + dt * vel[2],
            };
            ((fn_SetPosition_t)collVt[kCollVt_SetPosition / 4])(coll, nullptr, pos);
         }
      }

      void** solVt = *(void***)soldier;
      if (solVt) {
         static const float kZero[3] = {0.0f, 0.0f, 0.0f};
         ((fn_SetVelocity_t)solVt[kGoVt_SetVelocity / 4])(soldier, nullptr, kZero);
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

// ---------------------------------------------------------------------------
// Hook: OrdnanceGrapplingHook::Update
//
// Fixes 4, 5, 8 and 9, plus what a second press of fire does.  Returning 0
// destroys the ordnance, which is what the engine already does for every other
// "this hook cannot continue" case.
// ---------------------------------------------------------------------------

static unsigned int __fastcall hooked_Update(void* ecx, void* /*edx*/, float dt)
{
   __try {
      if (*(void**)((char*)ecx + kOrd_FireWeapon) == nullptr) return 0;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return 0;
   }

   // Fix 8.  When the firing soldier dies, the weapon dies with them and
   // mFireWeapon is left pointing into freed memory; mFireWeapon->mTrigger then
   // reads back as fill bytes and dereferencing it faults.  The engine kills the
   // hook the moment the soldier's handle goes stale, but not until it is inside
   // Update, so make the same check before reaching through the weapon at all.
   if (!live_soldier(ecx)) return original_Update(ecx, nullptr, dt);

   int stateBefore = -1;
   __try {
      stateBefore = *(int*)((char*)ecx + kOrd_State);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      stateBefore = -1;
   }

   uint32_t*  trigger = fire_trigger(ecx);
   HookState* state   = track_find(ecx, trigger);

   // Fix 13.  The soldier boarded something; let the engine tear the hook down.
   if (state && state->killed) return 0;

   if (state && (stateBefore == kState_Waiting || stateBefore == kState_Lifting))
      state->attached = true;


   // A fresh press of fire while the hook is out is the player asking for it
   // back.  CheckFire will not let the weapon fire again while the cable is out,
   // so the press is otherwise inert and free to mean this instead.
   bool letGo = false;
   if (state && state->pressed) {
      state->pressed = false;
      // Fix 12: whatever this press meant, it must not also fire the weapon.
      __try {
         hold_fire(*(void**)((char*)ecx + kOrd_FireWeapon), trigger);
      }
      __except (EXCEPTION_EXECUTE_HANDLER) {
      }
      if (stateBefore == kState_Lifting) {
         // Mid-pull: drop off here, carrying the speed built up.
         letGo = true;
      }
      else if (stateBefore == kState_Attaching || stateBefore == kState_Waiting) {
         // Still flying, or stuck somewhere useless: reel it in now rather than
         // waiting out the twenty second lifespan.  HookFailure is what the
         // engine itself calls for a hook that hit something it cannot hold, and
         // is a no-op if the retraction is already running.
         //
         // Fix 9.  A hook that has already stuck needs cutting loose first.  While
         // mCollBodyID is set Update re-derives the ordnance position from the hit
         // body every frame, so a retracting hook is pinned where it landed and
         // never reaches the weapon to be destroyed - leaving the soldier hanging
         // with no collision body until they kill themselves.  Clearing the hit
         // body lets it fly home, and the collision body goes back now rather than
         // when the hook finally arrives, so the soldier falls normally meanwhile.
         __try {
            if (*(int*)((char*)ecx + kOrd_CollBodyID) >= 0) {
               *(int*)((char*)ecx + kOrd_CollBodyID)   = -1;
               *(void**)((char*)ecx + kOrd_HitObject)  = nullptr;
               *(int*)((char*)ecx + kOrd_HitObjectKey) = 0;
            }
            if (state->attached) {
               void* soldier = live_soldier(ecx);
               if (soldier) {
                  void* coll = (char*)soldier + kSol_CollObject;
                  if (!fn_IsInCollisionList(coll, nullptr))
                     fn_AddSoftBody(coll, nullptr);
               }
               state->attached = false;
            }
            fn_HookFailure(ecx, nullptr);
         }
         __except (EXCEPTION_EXECUTE_HANDLER) {
         }
      }
   }

   // Fix 5.  Firing is a tap, so the trigger is long back up by the time the hook
   // has travelled anywhere.  Show Update a held trigger for the whole flight and
   // pull, and put the value back straight afterwards so nothing else sees it.
   // The one exception is the frame the player asked to let go, where the engine's
   // own release path is exactly what is wanted.
   uint32_t savedTrigger = 0;
   __try {
      if (trigger) {
         savedTrigger = *trigger;
         if (letGo) *trigger &= ~1u;
         else       *trigger |= 1u;
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      trigger = nullptr;
   }

   const unsigned int result = original_Update(ecx, nullptr, dt);

   __try {
      if (trigger) *trigger = savedTrigger;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }

   // The engine has just zeroed the velocity on its way through the arrival path,
   // so hand the pull velocity back and let the soldier carry it.  Arrivals under
   // their own steam keep the engine's clean stop.
   if (result == 0 && letGo) {
      void* soldier = live_soldier(ecx);
      if (soldier) {
         __try {
            void** solVt = *(void***)soldier;
            if (solVt)
               ((fn_SetVelocity_t)solVt[kGoVt_SetVelocity / 4])(
                  soldier, nullptr, (const float*)((char*)ecx + kOrd_VelX));
         }
         __except (EXCEPTION_EXECUTE_HANDLER) {
         }
      }
   }

   return result;
}

// ---------------------------------------------------------------------------
// Hook: OrdnanceGrapplingHook::Render
//
// Fix 6.  `this` is the displayable sub-object at ordnance+0x98 for both calls, so
// the base bullet render needs nothing the cable renderer does not already have.
// It builds its matrix from the ordnance velocity, so the hook points along its
// flight and, once stuck, back down the rope.  Model first, cable over the top.
// ---------------------------------------------------------------------------

static void __fastcall hooked_Render(void* ecx, void* /*edx*/, uint32_t lod, float f,
                                     uint32_t flags)
{
   __try {
      // Fix 10.  HookFailure negates the velocity to fly the hook home, and the
      // base render orients the model along it, so a retracting hook comes back
      // nose first.  Flip the velocity across the draw so it keeps pointing the
      // way it was thrown, prongs trailing.
      void*      ord     = (char*)ecx - kOrd_DisplayableOff;
      float*     vel     = (float*)((char*)ord + kOrd_VelX);
      const bool comingHome = *(int*)((char*)ord + kOrd_State) == kState_Aborting;

      if (void* soldier = live_soldier(ord)) refresh_cable_anchor(ord, soldier);

      if (comingHome) { vel[0] = -vel[0]; vel[1] = -vel[1]; vel[2] = -vel[2]; }
      fn_BulletRender(ecx, nullptr, lod, f, flags);
      if (comingHome) { vel[0] = -vel[0]; vel[1] = -vel[1]; vel[2] = -vel[2]; }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }

   original_Render(ecx, nullptr, lod, f, flags);
}

// ---------------------------------------------------------------------------
// Hook: ~OrdnanceGrapplingHook
//
// Fix 3.  The soldier's collision body is removed by Collide and only the arrival
// path puts it back, so restore it here for every other exit.  mState still holds
// the state the ordnance died in - the destructor does not touch it - and only
// HOOK_WAITING / HOOK_LIFTING can be reached with the body removed, so no other
// case is disturbed.
// ---------------------------------------------------------------------------

static void __fastcall hooked_Dtor(void* ecx, void* /*edx*/)
{
   int state = -1;

   __try {
      state = *(int*)((char*)ecx + kOrd_State);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      state = -1;
   }

   // Collide takes the body away on any hit it accepts.  The state at this point
   // no longer says whether that happened -- a hook reeled in from HOOK_WAITING
   // dies in HOOK_ABORTING -- so go by what was actually seen.
   const HookState* tracked = track_find(ecx, nullptr);
   const bool killed   = tracked && tracked->killed;
   const bool attached =
      !killed &&
      ((tracked && tracked->attached) || state == kState_Waiting || state == kState_Lifting);

   // Resolve before the destructor runs: it clears the handle on its way out.
   void* soldier = live_soldier(ecx);

   track_forget(ecx);

   original_Dtor(ecx, nullptr);

   if (!soldier) return;

   __try {
      // The destructor skips SetGrapplingHook(NULL) when mFireWeapon is NULL,
      // which would leave mHook pointing at this ordnance after it is freed.
      if (*(void**)((char*)soldier + kSol_Hook) == ecx)
         *(void**)((char*)soldier + kSol_Hook) = nullptr;

      if (!attached) return;

      void* coll = (char*)soldier + kSol_CollObject;
      if (!fn_IsInCollisionList(coll, nullptr))
         fn_AddSoftBody(coll, nullptr);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void grapple_install(uintptr_t exe_base)
{
   HOOK_REQUIRE_MODTOOLS();

   if (!g_addr->grapple_update || !g_addr->grapple_dtor ||
       !g_addr->grapple_move_soldier || !g_addr->grapple_rtti_hash ||
       !g_addr->grapple_cable_texture_hash || !g_addr->coll_is_in_list ||
       !g_addr->coll_add_soft || !g_addr->grapple_render ||
       !g_addr->ordnance_bullet_render || !g_addr->grapple_hook_failure ||
       !g_addr->trigger_update || !g_addr->grapple_set_property ||
       !g_addr->grapple_platform_init || !g_addr->grapple_platform_cleanup ||
       !g_addr->grapple_check_fire || !g_addr->soldier_enter_controllable)
      return;

   // Fix 1.  PlatformInit only reads sTextureHash, and it runs later in startup
   // than this does, so a plain write is enough - no hook needed.  Both statics
   // live in .data, so the ODF overrides can write them at level load too.
   g_cableTextureHash = (uint32_t*)resolve(exe_base, g_addr->grapple_cable_texture_hash);
   *g_cableTextureHash = pbl_hash(kCableTexture);

   // Fix 14.  .text is writable during install (dllmain re-protects afterwards).
   // Leave it alone unless every byte is the one we reverse engineered.
   if (g_addr->grapple_collide_type_test) {
      uint8_t* site = (uint8_t*)resolve(exe_base, g_addr->grapple_collide_type_test);
      if (memcmp(site, kCollideOrig, sizeof(kCollideOrig)) == 0) {
         memcpy(site, kCollidePatch, sizeof(kCollidePatch));
         g_collideSite = site;
      }
   }

   fn_PlatformInit    = (fn_PlatformFn_t)resolve(exe_base, g_addr->grapple_platform_init);
   fn_PlatformCleanup = (fn_PlatformFn_t)resolve(exe_base, g_addr->grapple_platform_cleanup);

   fn_IsInCollisionList =
      (fn_IsInCollisionList_t)resolve(exe_base, g_addr->coll_is_in_list);
   fn_AddSoftBody    = (fn_AddSoftBody_t)resolve(exe_base, g_addr->coll_add_soft);
   g_soldierRttiHash = (uint32_t*)resolve(exe_base, g_addr->grapple_rtti_hash);

   original_Update      = (fn_Update_t)resolve(exe_base, g_addr->grapple_update);
   original_Dtor        = (fn_Dtor_t)resolve(exe_base, g_addr->grapple_dtor);
   original_MoveSoldier =
      (fn_MoveSoldier_t)resolve(exe_base, g_addr->grapple_move_soldier);
   original_Render = (fn_Render_t)resolve(exe_base, g_addr->grapple_render);
   fn_BulletRender = (fn_Render_t)resolve(exe_base, g_addr->ordnance_bullet_render);
   fn_HookFailure  = (fn_HookFailure_t)resolve(exe_base, g_addr->grapple_hook_failure);
   original_TriggerUpdate =
      (fn_TriggerUpdate_t)resolve(exe_base, g_addr->trigger_update);
   original_SetProperty =
      (fn_SetProperty_t)resolve(exe_base, g_addr->grapple_set_property);
   original_CheckFire = (fn_CheckFire_t)resolve(exe_base, g_addr->grapple_check_fire);
   original_EnterControllable =
      (fn_EnterControllable_t)resolve(exe_base, g_addr->soldier_enter_controllable);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_Update,      hooked_Update);
   DetourAttach(&(PVOID&)original_Dtor,        hooked_Dtor);
   DetourAttach(&(PVOID&)original_MoveSoldier, hooked_MoveSoldier);
   DetourAttach(&(PVOID&)original_Render,      hooked_Render);
   DetourAttach(&(PVOID&)original_TriggerUpdate, hooked_TriggerUpdate);
   DetourAttach(&(PVOID&)original_SetProperty,   hooked_SetProperty);
   DetourAttach(&(PVOID&)original_CheckFire,     hooked_CheckFire);
   DetourAttach(&(PVOID&)original_EnterControllable, hooked_EnterControllable);
   DetourTransactionCommit();

   g_installed = true;
}

void grapple_uninstall()
{
   // Sections are re-protected by the time this runs, so the restore cannot be a
   // plain write.
   if (g_collideSite) {
      protected_write(g_collideSite, kCollideOrig, sizeof(kCollideOrig));
      g_collideSite = nullptr;
   }

   if (!g_installed) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_Update,      hooked_Update);
   DetourDetach(&(PVOID&)original_Dtor,        hooked_Dtor);
   DetourDetach(&(PVOID&)original_MoveSoldier, hooked_MoveSoldier);
   DetourDetach(&(PVOID&)original_Render,      hooked_Render);
   DetourDetach(&(PVOID&)original_TriggerUpdate, hooked_TriggerUpdate);
   DetourDetach(&(PVOID&)original_SetProperty,   hooked_SetProperty);
   DetourDetach(&(PVOID&)original_CheckFire,     hooked_CheckFire);
   DetourDetach(&(PVOID&)original_EnterControllable, hooked_EnterControllable);
   DetourTransactionCommit();

   g_installed = false;
}
