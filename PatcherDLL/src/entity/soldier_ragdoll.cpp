#include "pch.h"
#include "soldier_ragdoll.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/ray_hit.hpp"

#include <cmath>
#include <cstring>
#include <detours.h>

// =============================================================================
// Soldier ragdoll - Verlet bone simulation for dead soldiers.
//
// Hook point
// ----------
// SoldierAnimator::ApplyProceduralAnimationAndBuildWorldMatrices
// (modtools 0x00579f10, __thiscall(SoldierAnimator*, float dt), RET 4) is the
// single place the soldier's per-joint world matrices are produced.  Its tail
// is literally
//
//     00579f57: PUSH 0xcf6830              ; &gMatrixIdentity
//     00579f5c: LEA  ECX,[EBX + 0xd0]      ; &this->mZephyrSkeleton
//     00579f62: CALL 0x0082c390            ; ZephyrSkeleton<32>::BuildWorldMatrices
//
// so we let the original run and then overwrite what it produced.  This has to
// happen every frame, not once: the animator rebuilds the pose from the death
// clip on each call and would otherwise stomp the simulation straight back out
// (same shape as the droideka death-anim bug, where Update re-issues its input
// every frame).
//
// Note the root matrix is built against gMatrixIdentity, so m_pWorldMatrices are
// in MODEL space, not world space.  That is what keeps this cheap: the pose the
// engine hands us needs no world query to interpret.
//
// The sim itself runs in WORLD space so the body has inertia of its own, which
// model space cannot express: there the corpse entity's motion is invisible, so
// a body could only ever be dragged rigidly along by it.  seed() moves the pose
// out to world space and takes its starting velocity from EntitySoldier::
// mVelocity, where DirectionKill has just deposited the death impulse; the
// entity is not consulted again after that, and write_back() brings the result
// back to model space.  Contact is a single plane in world space, sampled with
// one downward CollisionManager::RayHit so it carries the real surface height
// and tilt, and re-sampled from the body's own centroid whenever the body has
// travelled far enough for the old sample to be stale - see kGroundReprobeDist
// for why it follows the body and never the corpse entity.
//
// Data layout (build-invariant: these are PODs read straight out of the .zaabin
// skeleton, and ZephyrSkeleton<32> is 0x810 bytes on debug and release alike -
// cross-checked against the modtools EntityFlyer struct dump in
// docs/RE/ghidra-fixup-plan.md, which lists mZephyrSkeleton as 0x810).
//
//   SoldierAnimator      +0x50  EntitySoldier* mOwner  (struct_base, NOT entity)
//                        +0xD0  ZephyrSkeleton<32> mZephyrSkeleton
//   ZephyrSkeleton<32>   +0x00  ZephyrSkeletonShared* m_pShared
//                        +0x04  ZephyrPoseStatic<32>* m_pPoseFinal
//                        +0x10  PblMatrix m_pWorldMatrices[32]
//   ZephyrSkeletonShared +0x00  ZephyrJointShared* m_pJoints
//                        +0x04  uint m_uNumJoints
//   ZephyrJointShared    112 bytes; +0x60 m_iParent, +0x61 m_iChild (signed,
//                        -1 = none).  Confirmed live: BuildWorldMatrices reads
//                        *(char*)(*m_pShared + 0x61) to start its child walk.
//
// Death test
// ----------
// EntitySoldier derives Damageable at struct_base+0x140 (data +0x144); its
// 188-byte block ends with mHealthFlags at +184, i.e. struct_base+0x1FC, packed
// mHealthType:3 / mIsAlive:1 / mVanishing:1 / mIsShielded:1.  So alive =
// bit 3.  Independently confirmed in the Phantom PDB build, where
// EntitySoldier::DirectionKill reads exactly `*(uint*)(x + 0x1fc) >> 3 & 1`.
//
// The matrices this writes are consumed by RedSkeleton::GetSkinMatrices for the
// skinned mesh, so nothing else needs patching to make it visible.
// =============================================================================

bool g_soldierRagdollEnabled = false;
bool g_soldierRagdollDebug   = false;

// One-shot guard for the seed dump, cleared per level by soldier_ragdoll_reset().
static bool g_loggedSeed = false;

// ---------------------------------------------------------------------------
// Offsets
// ---------------------------------------------------------------------------

static constexpr int kSAOwner          = 0x50;  // SoldierAnimator::mOwner (struct_base)
static constexpr int kSAZephyrSkeleton = 0xD0;  // SoldierAnimator::mZephyrSkeleton

static constexpr int kZSShared         = 0x00;  // ZephyrSkeleton::m_pShared
static constexpr int kZSWorldMatrices  = 0x10;  // ZephyrSkeleton::m_pWorldMatrices[32]

static constexpr int kZSSJoints        = 0x00;  // ZephyrSkeletonShared::m_pJoints
static constexpr int kZSSNumJoints     = 0x04;  // ZephyrSkeletonShared::m_uNumJoints

static constexpr int kJointStride      = 112;   // sizeof(ZephyrJointShared)
static constexpr int kJointParent      = 0x60;  // char m_iParent
static constexpr int kJointChild       = 0x61;  // char m_iChild

static constexpr int kHealthFlags      = 0x1FC; // EntitySoldier struct_base + mHealthFlags
static constexpr int kIsAliveBit       = 3;

// EntitySoldier::mMatrix - the corpse's world transform (PblMatrix: right, up,
// forward, trans).  Sits between the RedSceneObject block and mModel at +0x130.
static constexpr int kEntityMatrix     = 0xF0;

// EntitySoldier::mVelocity - the corpse's world-space velocity, and the field
// the death impulse lands in.  This is what gives the body its momentum; without
// it the ragdoll is born at rest and an explosion cannot throw it.
//
// Confirmed by DISASSEMBLY, not by the struct dump.  mt EntitySoldier::
// DirectionKill (0x0054c020) normalises (soldierPos - blastPos) and hands it to
// 0x005458b0, which writes three consecutive floats at this+0x4EC:
//   - normal path:        += dir * 5.0
//   - state 0x11 / 0x12:   = normalize(dir, biased up by 0.4 + rand*0.3) * 9.0
//     followed by SetState(9)
//
// The dump puts EntitySoldier_data at 0x424, which would make +0x4EC the middle
// of mPitchOffset / mPitchVelocity / mKickScale - three unrelated scalars, not an
// impulse target.  The real base is 0x434, and two fields agree on it
// independently: mVelocity at +184 lands on 0x4EC (the write above), and mState
// at +800 lands on 0x754, which is exactly the field 0x005458b0 tests against
// SoldierState 0x11/0x12.  This is the same 0x10 skew the dump has elsewhere -
// see the warning in [[soldier-ragdoll-feature]] about sub-struct bases.
//
// The earlier attempt at this used struct+0x334, taking Controllable_data's base
// (0x27C) with EntitySoldier_data's field index (+184) - two different
// sub-structs. That is why corpses launched at clamp speed from a standing kill.
static constexpr int kEntityVelocity   = 0x4EC;

// Sanity ceiling on the seeded speed.  A real death impulse is 5-9 m/s, and a
// sprinting soldier adds a few more; anything past this means we are reading a
// field that is not mVelocity, and the body should collapse in place rather than
// be flung off the map.  Logged when it trips so a bad offset stays visible
// instead of being silently absorbed.
static constexpr float kMaxSeedSpeed   = 30.0f;

// PblMatrix is D3DX row-major with row-vector convention: rows at +0x00/+0x10/
// +0x20 and the translation in row 3 at +0x30 (matches Chunk::mMatrix, whose
// position is read at field_0x30).
static constexpr int kMatrixFloats     = 16;
static constexpr int kMatTransX        = 12;    // float index of row3.x

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

static constexpr int   kMaxJoints    = 32;     // ZephyrSkeleton<32>
static constexpr int   kMaxRagdolls  = 24;     // simultaneous simulated corpses

static constexpr float kGravity      = -9.81f; // model units are metres
static constexpr float kFixedStep    = 1.0f / 60.0f;
static constexpr int   kMaxSubsteps  = 4;      // clamps a long frame / hitch
static constexpr int   kIterations   = 10;     // constraint relaxation passes
static constexpr float kDamping      = 0.985f; // per-step velocity retention
static constexpr float kGroundFric   = 0.55f;  // tangential velocity killed on contact
static constexpr float kSettleTime   = 10.0f;  // seconds before the sim is parked

// Constraint network.  Parent-child links alone are a 1D string: they fix bone
// LENGTHS and nothing else, so the body folds up and heaps into a pile, and
// joints are free to converge in x/z because nothing holds them apart.  Instead
// every pair of joints within kConstraintDepth hops of each other gets a
// distance constraint measured off the seed pose, which turns the skeleton into
// a truss.  A link to a joint 2 hops away is exactly an angle spring on the
// joint between them, so this supplies joint limits as well as rigidity - which
// matters most across the shoulder girdle, where j07/j12/j18 are coincident and
// the torso would otherwise have no width at all.
//
// Stiffness falls off with hop distance: bones are rigid, the rest are springs
// pulling back towards the pose the soldier died in.
static constexpr int   kConstraintDepth = 3;
static constexpr int   kMaxConstraints  = 256;
static constexpr int   kMaxNeighbors    = 12;
static constexpr float kStiffByDepth[kConstraintDepth + 1] = {
   0.0f,   // unused
   1.0f,   // bone: does not stretch
   0.55f,  // angle spring across one joint
   0.30f,  // broad shape retention
};

// Joint thickness.  Contacts stop a radius off the plane rather than exactly on the
// plane, so the corpse has volume instead of every joint converging on one
// height (which is the other half of the flattening).
// ---------------------------------------------------------------------------
// Ground probe
// ---------------------------------------------------------------------------
// The contact plane used to be flat and taken from the height the soldier died
// at, which is exact on level ground and wrong everywhere else: on a slope the
// body lies horizontally with half of it buried and half floating.  One
// CollisionManager::RayHit straight down at seed replaces that with the real
// surface AND its normal, so the plane can tilt.  It is still frozen at seed and
// still a single plane - see the note in seed() for why it is not tracked - so
// this costs one raycast per corpse and nothing per frame.
//
// The ray starts above the body and looks down.  A soldier is standing on the
// ground when it dies, so the hit we want is at roughly the entity's own height;
// a hit noticeably ABOVE that is the corpse's own collision volume, which the
// ray enters on the way down.  Rather than build an exclude list (which needs
// the GameObject* behind the entity, the same reason barrel_fire_origin pushes
// its ray start forward instead), restart the ray just below such a hit and try
// again.  Two restarts is plenty for a body inside its own cylinder.
static constexpr float kGroundRayStart   = 2.5f;   // above the entity origin, clear of the body
static constexpr float kGroundRayDist    = 12.0f;  // downward search length
static constexpr float kGroundSelfHitEps = 0.30f;  // above the feet by this much = not the floor
static constexpr float kGroundRetryStep  = 0.25f;  // how far below a self-hit the next ray starts
static constexpr int   kGroundRayTries   = 8;      // 8 x 0.25 m clears a standing soldier's cylinder

// The same filter has to be looser when the plane is being re-probed under a
// moving body (see kGroundReprobeDist): there the reference height is the OLD
// plane, and ground that is genuinely rising is genuinely above it.  At the
// re-probe interval this allows terrain to climb at ~60 degrees before a real
// hit is mistaken for the corpse's own collision volume, which is still well
// clear of the ~1.8 m cylinder cap that the filter exists to reject.
static constexpr float kGroundReprobeRise = 0.60f;

// How far below the measured (death-height) plane a probe result is allowed to
// sit.  This bounds the damage a bad hit can do, and deliberately leaves long
// falls alone: the sim's world positions are converted back through the entity
// transform at write-back, so a body falling 20 m to a plane the entity is not
// falling towards detaches from its own corpse.  Slopes, kerbs, steps and small
// ledges are all well inside this; real falls are environment collision's
// problem, not the contact plane's.
static constexpr float kGroundMaxDrop    = 3.0f;

// How far the body may travel horizontally before the plane is probed again.
//
// A single plane frozen at the death point is only the real surface NEAR that
// point, and a soldier killed while moving does not stay near it: the death
// impulse throws the body several metres, and it spends that whole flight with a
// contact plane sampled somewhere behind it.  Land on rising ground and the body
// goes into the hillside, because the plane it landed on is where the terrain
// used to be.
//
// So the plane follows the BODY - re-probed from its own centroid, not from the
// entity.  That distinction is the whole reason the original design froze it:
// the entity keeps sliding under its own death impulse and the two integrations
// do not agree, so tracking the entity drags the floor out from under a body
// that is not going there.  The body's own centroid has no such problem.
//
// Most re-probes land while the body is still airborne, where moving the plane
// cannot disturb anything because nothing is touching it.  Once it is down,
// contact friction kills the slide within a few steps, so re-probes become rare
// exactly when a moving plane would be most visible.
static constexpr float kGroundReprobeDist = 0.35f;

// Below this the surface is a wall, not a floor, and the body would slide along
// it forever.  Fall back to flat rather than tilt onto something near-vertical.
static constexpr float kGroundMinNormalY = 0.5f;

static constexpr float kJointRadius  = 0.09f;

// ---------------------------------------------------------------------------
// Self-collision
// ---------------------------------------------------------------------------
// Bones are capsules and collide pairwise, because joint spheres do not solve
// the thing you actually see: two bones can cross clean through each other with
// both endpoints still comfortably apart.  The test is segment-segment closest
// approach, which is the capsule test once both radii are added.
//
// Radii are NOT authored.  A stock human skeleton has no thickness data, and
// inventing per-bone numbers would be per-rig tuning that breaks on every modded
// unit - the same reason rest lengths are measured off the live pose rather than
// read from m_kBaseTransform.  Instead every bone starts at a fraction of the
// median bone length, then each is thinned until it does not already overlap its
// nearest non-adjacent neighbour IN THE DEATH POSE.
//
// That clamp is the load-bearing part.  A death pose already has bones touching -
// arms against the torso, the three coincident shoulder joints - and a solver
// told to separate things that start overlapped injects energy on frame one and
// detonates the body.  Measuring at seed means the constraint can only ever
// prevent NEW interpenetration, never fight the pose it was born in.
//
// Thinning to half the nearest seed distance is conservative: a bone that starts
// close to one neighbour ends up thin along its whole length.  The alternative,
// a per-pair distance table, costs ~500 floats per corpse to fix a case that
// only shows up as slightly-too-permissive contact between two limbs.
static constexpr float kSelfRadiusScale = 0.30f;  // of the median bone length
static constexpr float kSelfSeedSlack   = 0.90f;  // keep a gap at seed, so no jitter
static constexpr float kSelfMinRadius   = 0.015f; // thinner than this = do not collide
static constexpr float kSelfStiffness   = 0.50f;  // share of the overlap resolved per step

// Joint classification (see seed()).  kCoincidentEps is "sits on its parent";
// kOutlierFactor is how many median bone lengths a LEAF may sit from its parent
// before it is treated as an aim/hardpoint node rather than a bone.  On a stock
// human skeleton the real bones run 0.13-0.48 against a median of ~0.24, and the
// three aim nodes sit at 1.79-1.84, so the margin either side is wide.
static constexpr float kCoincidentEps = 0.01f;
static constexpr float kOutlierFactor = 2.5f;

// Below this many simulated joints there is no body left to simulate and we
// leave the animated pose alone.
static constexpr int   kMinSimJoints  = 4;

// ---------------------------------------------------------------------------
// Small vector helpers (kept local - no D3DX dependency)
// ---------------------------------------------------------------------------

struct Vec3 {
   float x, y, z;
};

static inline Vec3 v_sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline float v_dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 v_cross(const Vec3& a, const Vec3& b)
{
   return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static inline float v_len(const Vec3& a) { return std::sqrt(v_dot(a, a)); }

// PblMatrix is row-major with row vectors, so a point transforms as p * M with
// the translation living in row 3.
static inline Vec3 xform_point(const float* M, const Vec3& p)
{
   return {p.x * M[0] + p.y * M[4] + p.z * M[8]  + M[12],
           p.x * M[1] + p.y * M[5] + p.z * M[9]  + M[13],
           p.x * M[2] + p.y * M[6] + p.z * M[10] + M[14]};
}

// Inverse of the above for a rigid (optionally scaled) transform: subtract the
// translation, then project onto each basis row and divide by its squared
// length, which undoes any scale baked into the rows as well as the rotation.
static inline Vec3 xform_point_inv(const float* M, const Vec3& p)
{
   const float d[3] = {p.x - M[12], p.y - M[13], p.z - M[14]};
   Vec3 out{};
   float* o = &out.x;
   for (int r = 0; r < 3; r++) {
      const float* row = M + r * 4;
      const float sq = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
      o[r] = (sq > 1e-12f)
                ? (d[0] * row[0] + d[1] * row[1] + d[2] * row[2]) / sq
                : 0.0f;
   }
   return out;
}

static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Closest points between segments p1->q1 and p2->q2, as parameters along each
// plus the points themselves.  Standard clamped-parameter solve; the degenerate
// branches matter here because a Zephyr skeleton is full of near-zero-length
// links and a raw division would produce NaN and poison the whole body.
static void closest_seg_seg(const Vec3& p1, const Vec3& q1,
                            const Vec3& p2, const Vec3& q2,
                            float* outS, float* outT, Vec3* outC1, Vec3* outC2)
{
   const Vec3  d1 = v_sub(q1, p1);
   const Vec3  d2 = v_sub(q2, p2);
   const Vec3  r  = v_sub(p1, p2);
   const float a  = v_dot(d1, d1);
   const float e  = v_dot(d2, d2);
   const float f  = v_dot(d2, r);
   const float kEps = 1e-8f;

   float s, t;

   if (a <= kEps && e <= kEps) {
      s = t = 0.0f;                       // both degenerate: point vs point
   } else if (a <= kEps) {
      s = 0.0f; t = clamp01(f / e);       // first degenerate: point vs segment
   } else {
      const float c = v_dot(d1, r);
      if (e <= kEps) {
         t = 0.0f; s = clamp01(-c / a);   // second degenerate
      } else {
         const float b     = v_dot(d1, d2);
         const float denom = a * e - b * b;
         s = (denom > kEps) ? clamp01((b * f - c * e) / denom) : 0.0f;
         t = (b * s + f) / e;
         // t out of range: clamp it and re-solve s against the clamped t.
         if (t < 0.0f)      { t = 0.0f; s = clamp01(-c / a); }
         else if (t > 1.0f) { t = 1.0f; s = clamp01((b - c) / a); }
      }
   }

   *outS  = s;
   *outT  = t;
   *outC1 = {p1.x + d1.x * s, p1.y + d1.y * s, p1.z + d1.z * s};
   *outC2 = {p2.x + d2.x * t, p2.y + d2.y * t, p2.z + d2.z * t};
}

// A soldier's world transform is upright yaw, so a sane matrix has a near-
// vertical up row and a unit-ish scale.  Anything else means we are reading a
// torn-down or not-yet-initialised entity and should stay in model space.
static inline bool matrix_is_sane(const float* M)
{
   const float sq = M[4] * M[4] + M[5] * M[5] + M[6] * M[6];
   return sq > 0.25f && sq < 4.0f;
}

// Casts down from above `refY` at (x, z) looking for the surface the corpse is
// on.  `refY` is where the caller believes the ground to be - the soldier's feet
// at seed, the current plane on a re-probe - and both the self-hit filter and
// the drop bound are measured against it.  `selfHitEps` is how far above refY a
// hit may sit before it is treated as the body rather than the floor.
// Returns false when nothing usable was found, leaving the caller's plane alone.
// outP/outN are only written on success.
static bool probe_ground(float x, float z, float refY, float selfHitEps,
                         bool trace, Vec3* outP, Vec3* outN)
{
   RayHit_t rayHit = ray_hit_get();
   if (!rayHit) {
      if (g_soldierRagdollDebug) {
         auto fn_log = get_gamelog();
         fn_log("[Ragdoll] probe: RayHit unresolved for this build\n");
      }
      return false;
   }

   const Vec3 down{0.0f, -1.0f, 0.0f};
   float      startY = refY + kGroundRayStart;

   for (int attempt = 0; attempt < kGroundRayTries; attempt++) {
      const Vec3 start{x, startY, z};

      void* hitObj = nullptr;   // written on entry by RayHit; must never be null

      // Zero rather than (0,1,0) so "RayHit left this alone" is distinguishable
      // from "RayHit says the surface is flat" - the two want different handling
      // below, and a sentinel that is already a legal answer hides the difference.
      Vec3 normal{0.0f, 0.0f, 0.0f};

      const float frac = rayHit(&start, &down, kGroundRayDist, &hitObj, &normal,
                                nullptr, 0, kRayFlagsAim, 1);
      const float hitY = startY - frac * kGroundRayDist;

      // Per-attempt trace.  Two guesses at why the probe was failing have now
      // both been wrong, so this reports the raw answer rather than a verdict:
      // frac == 1 means the mask/address/convention never found a surface, while
      // a small frac with a high hitY means the self-hit filter is eating a real
      // hit.  Those want opposite fixes and are indistinguishable from outside.
      if (g_soldierRagdollDebug && trace) {
         auto fn_log = get_gamelog();
         fn_log("[Ragdoll] probe try%d startY=%.3f refY=%.3f frac=%.4f hitY=%.3f "
                "obj=%p n=(%.3f, %.3f, %.3f)\n",
                attempt, startY, refY, frac, hitY, hitObj,
                normal.x, normal.y, normal.z);
      }

      if (!(frac >= 0.0f) || frac >= 1.0f) return false;   // also rejects NaN

      // A zero-length hit means the ray STARTED inside a collision volume, which
      // is what a ray dropped through a corpse does - confirmed in the log, where
      // three consecutive tries came back frac=0.0000 against the same object
      // with junk normals (one pointing straight down).  Such a hit carries no
      // usable surface, and accepting one once the retry has walked down near
      // refY would plant the plane inside the body and float it.  Always step
      // past it.
      if (frac <= 0.0f) {
         startY -= kGroundRetryStep;
         continue;
      }

      // Above the reference height by more than the caller's tolerance: this is
      // the body's own collision volume, or something resting on it.  Drop below
      // the hit and look again.
      //
      // The step down has to be a real gap, not an epsilon.  A collision system
      // that reports a t == 0 hit for a ray starting INSIDE a volume will hand
      // back the same surface every attempt, and a 1 cm step would never clear
      // the ~1.8 m of a soldier's own cylinder before the retries ran out.
      if (hitY > refY + selfHitEps) {
         startY = hitY - kGroundRetryStep;
         continue;
      }

      if (hitY < refY - kGroundMaxDrop) return false;      // long fall; keep the current plane

      const float len = v_len(normal);
      if (len > 0.9f && len < 1.1f) {
         normal.x /= len; normal.y /= len; normal.z /= len;
         // Too steep to be a floor: the body would slide down it forever.  Take
         // the height, which is still an improvement, but stay flat.
         if (normal.y < kGroundMinNormalY) normal = Vec3{0.0f, 1.0f, 0.0f};
      } else {
         // No usable normal came back for this surface.  Correcting the height
         // is most of the win; inventing a tilt from nothing is not.
         normal = Vec3{0.0f, 1.0f, 0.0f};
      }

      *outP = Vec3{x, hitY, z};
      *outN = normal;
      return true;
   }

   return false;   // never got past the body
}

// ---------------------------------------------------------------------------
// Per-corpse state
// ---------------------------------------------------------------------------

struct Constraint {
   signed char a, b;
   float       len;
   float       stiff;
};

// One capsule of the body: the segment from joint a to joint b (b is a's
// constraint parent) with a radius measured at seed.  radius == 0 means the bone
// was thinned past usefulness by a neighbour and is excluded from the test.
struct Bone {
   signed char a, b;
   float       radius;
   // Bones this one must not be tested against, as a bitmask of bone indices.
   // Bones that meet - by sharing a joint, or by having endpoints that sit on
   // top of each other - touch by definition and are excluded rather than
   // thinned.  See build_bones() for why the difference is load-bearing.
   uint32_t    skip;
};

static_assert(kMaxJoints <= 32, "Bone::skip is a 32-bit mask of bone indices");

struct RagdollState {
   void*       animator;              // key; nullptr = free slot
   uint32_t    touchTick;             // for LRU eviction
   int         numJoints;
   int         simCount;              // joints that survived classification
   bool        seeded;
   float       accum;                 // fixed-step accumulator
   float       age;                   // seconds since death
   // Contact plane, world space, frozen at seed.  groundP is a point on it and
   // groundN its unit normal; a flat plane is groundN = (0,1,0), which is what
   // the measured-height fallback produces and is bit-identical to the old
   // height-only test.
   Vec3        groundP;
   Vec3        groundN;
   bool        groundProbed;           // true = plane came from a raycast, not the fallback
   bool        sim[kMaxJoints];       // false = scaffolding / non-physical node
   signed char parent[kMaxJoints];    // raw m_iParent, as authored
   signed char cparent[kMaxJoints];   // constraint parent after pass-through, -1 = free root
   signed char dirChild[kMaxJoints];  // joint this one aims at for its swing, -1 = none
   // Second joint used to pin down ROLL about the dirChild axis.  One direction
   // fixes two of three degrees of freedom; without this the twist is simply
   // unrepresentable.  Chosen once at seed - see pick_roll_reference().
   signed char refJoint[kMaxJoints];
   signed char follow[kMaxJoints];    // dropped node copies this joint's matrix, -1 = none
   Vec3        pos[kMaxJoints];
   Vec3        prev[kMaxJoints];
   float       restLen[kMaxJoints];   // distance to cparent, measured off the live pose
   Constraint  cons[kMaxConstraints];
   int         consCount;
   Bone        bones[kMaxJoints];     // at most one per joint (its link to cparent)
   int         boneCount;
};

static RagdollState g_ragdolls[kMaxRagdolls] = {};
static uint32_t     g_tick                   = 0;

static RagdollState* find_state(void* animator)
{
   for (int i = 0; i < kMaxRagdolls; i++)
      if (g_ragdolls[i].animator == animator) return &g_ragdolls[i];
   return nullptr;
}

static void release_state(void* animator)
{
   RagdollState* s = find_state(animator);
   if (s) s->animator = nullptr;
}

// Takes a free slot, else evicts the least recently touched one.  Corpses
// despawn on their own, so eviction only bites when more than kMaxRagdolls are
// visible at once - the oldest is the one nearest to vanishing anyway.
static RagdollState* acquire_state(void* animator)
{
   RagdollState* victim = nullptr;
   for (int i = 0; i < kMaxRagdolls; i++) {
      if (g_ragdolls[i].animator == nullptr) { victim = &g_ragdolls[i]; break; }
      if (!victim || g_ragdolls[i].touchTick < victim->touchTick) victim = &g_ragdolls[i];
   }
   if (!victim) return nullptr;

   std::memset(victim, 0, sizeof(*victim));
   victim->animator = animator;
   return victim;
}

// ---------------------------------------------------------------------------
// Seed - snapshot the animated pose as the ragdoll's starting configuration
// ---------------------------------------------------------------------------
// Rest lengths come from the LIVE pose rather than ZephyrJointShared's bind
// transform: measuring what the skeleton actually is right now costs one
// subtract per bone and is automatically correct for per-joint scale and for
// whatever non-standard skeleton a mod ships.

static void seed(RagdollState* s, const float* world, const uint8_t* joints, int n,
                 const float* entityMat, const Vec3& vel)
{
   s->numJoints = n;

   for (int i = 0; i < n; i++) {
      const float* m = world + i * kMatrixFloats;
      s->pos[i]  = {m[kMatTransX], m[kMatTransX + 1], m[kMatTransX + 2]};
      s->prev[i] = s->pos[i]; // start at rest; gravity does the rest
      s->parent[i] = (signed char)joints[i * kJointStride + kJointParent];
   }

   // ---- Classify joints -----------------------------------------------------
   // Not every entry in a Zephyr skeleton is a bone.  Simulating the ones that
   // are not is what wrecks the pose, and no amount of solver tuning fixes it.
   //
   //  a) COINCIDENT nodes sit exactly on their parent - BuildWorldMatrices
   //     copies the parent's matrix wholesale for these (the kuiDummyRootHash
   //     branch).  They are branch/pass-through markers, not bones.
   //  b) The ROOT sits at the model origin.  The link from it up to the pelvis
   //     is the rig's root offset, not anatomy: constraining it pins the pelvis
   //     to a fixed distance from a point on the ground, so the corpse can never
   //     lie down.  Anything hanging directly off the root becomes a free root.
   //  c) OUTLIER LEAVES are aim / hardpoint nodes positioned by the procedural
   //     pass rather than the hierarchy - on a human they show up as leaves
   //     ~1.8m from their parent against a 0.48m longest real bone.  Judged
   //     against the median bone so this holds for non-human skeletons too, and
   //     restricted to leaves so it can never sever the body.

   bool coincident[kMaxJoints] = {};
   for (int i = 0; i < n; i++) {
      const int p = s->parent[i];
      coincident[i] = (p >= 0 && p < n) &&
                      v_len(v_sub(s->pos[i], s->pos[p])) < kCoincidentEps;
   }

   // Median of the non-degenerate parent distances, for the outlier test.
   float lens[kMaxJoints];
   int   lenCount = 0;
   for (int i = 0; i < n; i++) {
      const int p = s->parent[i];
      if (p < 0 || p >= n) continue;
      const float d = v_len(v_sub(s->pos[i], s->pos[p]));
      if (d >= kCoincidentEps) lens[lenCount++] = d;
   }
   for (int a = 1; a < lenCount; a++) { // insertion sort, <= 32 elements
      const float v = lens[a];
      int b = a - 1;
      while (b >= 0 && lens[b] > v) { lens[b + 1] = lens[b]; b--; }
      lens[b + 1] = v;
   }
   const float medianLen  = lenCount ? lens[lenCount / 2] : 0.0f;
   const float outlierLen = medianLen * kOutlierFactor;

   // Pass through coincident scaffolding to the first real ancestor.  Kept in
   // its own array because a joint's effective parent is very often a HIGHER
   // index (the pelvis is j25 while its children are j04/j05/j11/j21/j22), so
   // nothing here may read a per-joint result that the same loop is still
   // producing - see the two-pass split below.
   signed char eff[kMaxJoints];
   for (int i = 0; i < n; i++) {
      int p = s->parent[i];
      int guard = 0;
      while (p >= 0 && p < n && coincident[p] && guard++ < kMaxJoints)
         p = s->parent[p];
      eff[i] = (signed char)((p >= 0 && p < n) ? p : -1);
   }

   // Pass 1 - classify every joint.  Depends only on geometry and the raw
   // hierarchy, never on another joint's classification.
   for (int i = 0; i < n; i++) {
      const bool isRoot = (s->parent[i] < 0);
      const bool isLeaf = ((signed char)joints[i * kJointStride + kJointChild]) < 0;
      const int  p      = eff[i];
      const float d     = (p >= 0) ? v_len(v_sub(s->pos[i], s->pos[p])) : 0.0f;

      s->sim[i] = !isRoot && !coincident[i] &&
                  !(isLeaf && outlierLen > 0.0f && d > outlierLen);
   }

   // Pass 2 - link constraints, now that every sim[] is known.  Constrain only
   // to another simulated joint; anything hanging off the root (or off dropped
   // scaffolding) becomes a free root of the body.
   for (int i = 0; i < n; i++) {
      const int  p      = eff[i];
      const bool linked = s->sim[i] && p >= 0 && s->sim[p];
      s->cparent[i] = (signed char)(linked ? p : -1);
      s->restLen[i] = linked ? v_len(v_sub(s->pos[i], s->pos[p])) : 0.0f;
   }

   // Direction child - the joint each one aims at to derive its swing.  Cannot
   // just be m_iChild: the pelvis's m_iChild is coincident scaffolding, which is
   // dropped, so the pelvis would have no bone direction and would keep the
   // death clip's orientation forever while its position simulated.  Since it is
   // also the free root it has no ancestor to inherit from either, and the root
   // of the visible torso not rotating reads as the hips detaching.  Fall back to
   // any simulated joint that constrains to this one (for the pelvis, the spine).
   for (int i = 0; i < n; i++) {
      s->dirChild[i] = -1;
      if (!s->sim[i]) continue;

      const int c = (signed char)joints[i * kJointStride + kJointChild];
      if (c >= 0 && c < n && s->sim[c]) { s->dirChild[i] = (signed char)c; continue; }

      for (int j = 0; j < n; j++)
         if (s->sim[j] && s->cparent[j] == i) { s->dirChild[i] = (signed char)j; break; }
   }

   // Coincident scaffolding has to be carried along with the joint it collapsed
   // into.  The engine copies the parent's matrix into these nodes itself (the
   // kuiDummyRootHash branch of BuildWorldMatrices), and we run after that, so
   // leaving them alone strands any mesh skinned to them at the animated pose
   // while the joint they sit on moves away.
   for (int i = 0; i < n; i++) {
      const int e = eff[i];
      s->follow[i] = (signed char)((!s->sim[i] && coincident[i] && e >= 0 && s->sim[e])
                                      ? e : -1);
   }

   // Pass 3 - build the truss.  Undirected adjacency from the constraint links,
   // then a bounded BFS out of every joint; each pair found within
   // kConstraintDepth hops becomes one distance constraint at its seed length.
   signed char nb[kMaxJoints][kMaxNeighbors];
   int         nbCount[kMaxJoints] = {};

   for (int i = 0; i < n; i++) {
      const int p = s->cparent[i];
      if (p < 0 || p >= n) continue;
      if (nbCount[i] < kMaxNeighbors) nb[i][nbCount[i]++] = (signed char)p;
      if (nbCount[p] < kMaxNeighbors) nb[p][nbCount[p]++] = (signed char)i;
   }

   s->consCount = 0;
   for (int src = 0; src < n; src++) {
      if (!s->sim[src]) continue;

      signed char dist[kMaxJoints];
      std::memset(dist, -1, sizeof(dist));
      signed char queue[kMaxJoints];
      int head = 0, tail = 0;

      dist[src] = 0;
      queue[tail++] = (signed char)src;

      while (head < tail) {
         const int cur = queue[head++];
         if (dist[cur] >= kConstraintDepth) continue;

         for (int e = 0; e < nbCount[cur]; e++) {
            const int nxt = nb[cur][e];
            if (dist[nxt] >= 0) continue;
            dist[nxt] = (signed char)(dist[cur] + 1);
            queue[tail++] = (signed char)nxt;

            // Emit each pair once, and skip degenerate coincident pairs (the
            // three shoulder branch joints sit on top of each other).
            if (nxt <= src) continue;
            const float len = v_len(v_sub(s->pos[src], s->pos[nxt]));
            if (len < kCoincidentEps) continue;
            if (s->consCount >= kMaxConstraints) break;

            Constraint& c = s->cons[s->consCount++];
            c.a     = (signed char)src;
            c.b     = (signed char)nxt;
            c.len   = len;
            c.stiff = kStiffByDepth[dist[nxt]];
         }
      }
   }

   // ---- Roll references -----------------------------------------------------
   // For each joint, a second joint off the dirChild axis, so write_back can
   // build a full orientation frame instead of just a direction.
   //
   // Candidates are restricted to joints this one shares a truss constraint with.
   // Those are the ones held rigidly relative to it, so the frame they define
   // does not wobble as the body flexes; an arbitrary distant joint would make
   // the head's roll depend on where a foot happened to end up.
   //
   // Scored on how perpendicular the candidate sits to the primary axis - a
   // nearly-collinear reference determines roll about as well as no reference at
   // all - with distance as the tie-breaker, preferring local structure.
   for (int i = 0; i < n; i++) {
      s->refJoint[i] = -1;
      if (!s->sim[i]) continue;

      const int c = s->dirChild[i];
      if (c < 0 || c >= n) continue;

      Vec3        axis = v_sub(s->pos[c], s->pos[i]);
      const float alen = v_len(axis);
      if (alen < kCoincidentEps) continue;
      axis.x /= alen; axis.y /= alen; axis.z /= alen;

      float bestScore = 0.0f;
      int   best      = -1;

      for (int ci = 0; ci < s->consCount; ci++) {
         const Constraint& con = s->cons[ci];
         int k = -1;
         if (con.a == i) k = con.b;
         else if (con.b == i) k = con.a;
         else continue;

         if (k == c || k < 0 || k >= n || !s->sim[k]) continue;

         Vec3        d    = v_sub(s->pos[k], s->pos[i]);
         const float dlen = v_len(d);
         if (dlen < kCoincidentEps) continue;
         d.x /= dlen; d.y /= dlen; d.z /= dlen;

         // sin of the angle to the primary axis: 1 = ideal, 0 = useless.
         const float dot  = v_dot(d, axis);
         const float perp = 1.0f - dot * dot;
         if (perp < 0.10f) continue;   // within ~18 degrees of the axis

         const float score = perp / (1.0f + dlen / (medianLen > 0.0f ? medianLen : 1.0f));
         if (score > bestScore) { bestScore = score; best = k; }
      }

      s->refJoint[i] = (signed char)best;
   }

   // ---- Self-collision capsules ---------------------------------------------
   // One capsule per constraint link, then thinned so nothing starts overlapped.
   // Runs while everything is still in MODEL space, which is fine: these are all
   // distances, and the transform out to world is rigid.
   s->boneCount = 0;
   for (int i = 0; i < n; i++) {
      if (!s->sim[i]) continue;
      const int p = s->cparent[i];
      if (p < 0 || p >= n) continue;
      if (v_len(v_sub(s->pos[i], s->pos[p])) < kCoincidentEps) continue;

      Bone& b  = s->bones[s->boneCount++];
      b.a      = (signed char)i;
      b.b      = (signed char)p;
      b.radius = medianLen * kSelfRadiusScale;
      b.skip   = 0;
   }

   // Decide, per pair, between EXCLUDING it and THINNING for it.  Getting this
   // split wrong is what a first pass here got wrong, and the log said so:
   // bones=21 selfColliding=15, with the arms among the six switched off.
   //
   // A Zephyr shoulder girdle is a single point - j07, j12 and j18 all sit at
   // the same position - so both upper arms and the neck emanate from one place.
   // Index-based adjacency does not see that they meet, so the thinning pass read
   // their zero closest-approach as "already overlapped", thinned all three to
   // nothing, and dropped precisely the bones that most need to collide with the
   // torso.  Bones that MEET must be excluded; only bones that merely pass CLOSE
   // should thin each other.
   for (int i = 0; i < s->boneCount; i++) {
      for (int j = i + 1; j < s->boneCount; j++) {
         Bone& bi = s->bones[i];
         Bone& bj = s->bones[j];

         bool meets = (bi.a == bj.a || bi.a == bj.b || bi.b == bj.a || bi.b == bj.b);
         if (!meets) {
            const signed char ei[2] = {bi.a, bi.b};
            const signed char ej[2] = {bj.a, bj.b};
            for (int u = 0; u < 2 && !meets; u++)
               for (int v = 0; v < 2 && !meets; v++)
                  meets = v_len(v_sub(s->pos[ei[u]], s->pos[ej[v]])) < kCoincidentEps;
         }

         if (meets) {
            bi.skip |= 1u << j;
            bj.skip |= 1u << i;
            continue;
         }

         float sT, tT;
         Vec3  c1, c2;
         closest_seg_seg(s->pos[bi.a], s->pos[bi.b], s->pos[bj.a], s->pos[bj.b],
                         &sT, &tT, &c1, &c2);

         const float half = v_len(v_sub(c1, c2)) * 0.5f * kSelfSeedSlack;
         if (bi.radius > half) bi.radius = half;
         if (bj.radius > half) bj.radius = half;
      }
   }

   // A bone thinned below the useful minimum contributes nothing but work, and
   // flooring it back up would re-introduce the seed overlap the pass just
   // removed.  Drop it from the test instead.
   int selfActive = 0;
   for (int i = 0; i < s->boneCount; i++) {
      if (s->bones[i].radius < kSelfMinRadius) s->bones[i].radius = 0.0f;
      else selfActive++;
   }

   // How many joints ended up with a full orientation frame rather than a
   // swing-only fallback.  This is the number that decides whether twist is
   // representable at all, so it is worth having in the log.
   int rollRefs = 0;
   for (int i = 0; i < n; i++)
      if (s->sim[i] && s->refJoint[i] >= 0) rollRefs++;

   // Contact plane.  A soldier is standing on the ground at the instant it dies,
   // so its lowest joint IS ground level - measuring it removes the assumption
   // that the model origin sits exactly at the feet.  Clamped to <= 0 so a
   // soldier killed in mid-air still falls to its own origin plane, which the
   // engine keeps on the terrain as the corpse entity drops.  Only simulated
   // joints count: the dropped aim nodes sit wherever the procedural pass put
   // them (one of them hangs 1.8m below the head) and would drag the floor down
   // with them.
   float lowest = 0.0f;
   for (int i = 0; i < n; i++)
      if (s->sim[i] && s->pos[i].y < lowest) lowest = s->pos[i].y;
   const float measuredGroundY = lowest;

   int simCount = 0;
   for (int i = 0; i < n; i++) if (s->sim[i]) simCount++;
   s->simCount = simCount;

   // Everything above ran in MODEL space, which is the space the world matrices
   // are built in and the space the classification and rest lengths belong to.
   // The simulation itself runs in WORLD space so the body has inertia against
   // the corpse entity's own motion - in model space that motion is invisible,
   // so a corpse sliding downhill was dragged along rigidly instead of tumbling.
   // Rest lengths and the truss survive the move unchanged because the entity
   // transform is rigid.
   // The contact plane is frozen HERE, in world space, rather than tracked off
   // the entity every frame.  The entity keeps sliding under its own death
   // impulse and the body now travels under its own, but the two are integrated
   // separately and will not agree; tracking the entity would drag the floor
   // around under a body that is not following it.  Freezing makes them
   // independent, so entity drift cannot reach us.
   // The plane is the surface under where the soldier DIED, not under where the
   // body ends up.  Exact on flat ground and close enough for the metre or two a
   // body travels; a corpse thrown clean off a ledge still stops at the old
   // height, which is environment collision's problem rather than this one's.
   const float measuredWorldY = measuredGroundY + entityMat[13];

   // Default: the flat, measured plane.  probe_ground() upgrades it to the real
   // surface and its normal when the raycast agrees, which is what lets a body
   // lie ALONG a slope instead of horizontally through it.
   s->groundP      = Vec3{entityMat[12], measuredWorldY, entityMat[14]};
   s->groundN      = Vec3{0.0f, 1.0f, 0.0f};
   s->groundProbed = probe_ground(entityMat[12], entityMat[14], measuredWorldY,
                                  kGroundSelfHitEps, true, &s->groundP, &s->groundN);

   // Seed the body's momentum from the corpse entity's own velocity, which is
   // where EntitySoldier::DirectionKill has just deposited the death impulse.
   // Verlet carries velocity as the gap between pos and prev, and step() scales
   // that gap by kDamping before applying it, so the gap has to be pre-divided
   // to make the first step come out at exactly v.  Without this the body is
   // born at rest and no explosion can throw it, however hard it hit.
   float speed = v_len(vel);
   Vec3  v     = vel;
   if (speed > kMaxSeedSpeed) {
      const float k = kMaxSeedSpeed / speed;
      v.x *= k; v.y *= k; v.z *= k;
   }
   const float gap = kFixedStep / kDamping;

   for (int i = 0; i < n; i++) {
      s->pos[i]  = xform_point(entityMat, s->pos[i]);
      s->prev[i] = {s->pos[i].x - v.x * gap,
                    s->pos[i].y - v.y * gap,
                    s->pos[i].z - v.z * gap};
   }

   s->seeded = true;

   // Logged per corpse, not once per level: only some deaths are explosions, so
   // a single sample cannot tell a correct mVelocity from a wrong one.  Across a
   // few kills this should read ~0 for a standing shot, a few m/s along the
   // travel direction for a running one, and 5-9 m/s biased upward and away from
   // the blast for an explosion.  Anything constant, huge, or unrelated to how
   // the soldier died means kEntityVelocity is pointing at the wrong field.
   if (g_soldierRagdollDebug) {
      auto fn_log = get_gamelog();
      fn_log("[Ragdoll] seed vel=(%.3f, %.3f, %.3f) speed=%.3f%s  ground=%s "
             "y=%.3f (measured %.3f) slope=%.1fdeg\n",
             vel.x, vel.y, vel.z, speed,
             speed > kMaxSeedSpeed ? "  CLAMPED - suspect offset" : "",
             s->groundProbed ? "raycast" : "measured-flat",
             s->groundP.y, measuredWorldY,
             std::acos(s->groundN.y < 1.0f ? s->groundN.y : 1.0f) * 57.2957795f);
   }

   // One-shot dump of the seed configuration.  The model-space frame is the one
   // thing here that cannot be settled by reading the disassembly - whether the
   // skeleton root sits at the feet, the pelvis or somewhere else decides where
   // the contact plane belongs, and this answers it directly.  Logged for the
   // first corpse per level only; [Features] SoldierRagdollDebug=1.
   if (g_soldierRagdollDebug && !g_loggedSeed) {
      g_loggedSeed = true;
      auto fn_log = get_gamelog();
      fn_log("[Ragdoll] seed: numJoints=%d simulated=%d constraints=%d "
             "bones=%d selfColliding=%d rollRefs=%d "
             "medianBone=%.4f outlierOver=%.4f ground=%s p=(%.3f, %.3f, %.3f) "
             "n=(%.3f, %.3f, %.3f) slope=%.1fdeg\n",
             n, s->simCount, s->consCount, s->boneCount, selfActive, rollRefs,
             medianLen, outlierLen,
             s->groundProbed ? "raycast" : "measured-flat",
             s->groundP.x, s->groundP.y, s->groundP.z,
             s->groundN.x, s->groundN.y, s->groundN.z,
             std::acos(s->groundN.y < 1.0f ? s->groundN.y : 1.0f) * 57.2957795f);
      for (int i = 0; i < n; i++) {
         fn_log("[Ragdoll]   j%02d %s parent=%-3d cparent=%-3d child=%-3d "
                "pos=(%.3f, %.3f, %.3f) restLen=%.4f\n",
                i, s->sim[i] ? "sim " : "DROP", (int)s->parent[i],
                (int)s->cparent[i],
                (int)(signed char)joints[i * kJointStride + kJointChild],
                s->pos[i].x, s->pos[i].y, s->pos[i].z, s->restLen[i]);
      }
   }
}

// Height of the contact plane directly above/below (x, z).
static inline float plane_height_at(const Vec3& P, const Vec3& N, float x, float z)
{
   if (N.y < 1e-4f) return P.y;   // near-vertical; treat as flat to avoid a blowup
   return P.y - ((x - P.x) * N.x + (z - P.z) * N.z) / N.y;
}

// Re-probes the contact plane once the body has travelled far enough from where
// the current one was sampled.  See kGroundReprobeDist for why this follows the
// body's centroid rather than the corpse entity.
static void reprobe_ground(RagdollState* s)
{
   if (!s->groundProbed) return;   // never got a raycast; nothing to refine

   const int n = s->numJoints;

   Vec3 c{0.0f, 0.0f, 0.0f};
   int  count = 0;
   for (int i = 0; i < n; i++) {
      if (!s->sim[i]) continue;
      c.x += s->pos[i].x; c.z += s->pos[i].z;
      count++;
   }
   if (count == 0) return;
   c.x /= (float)count; c.z /= (float)count;

   const float dx = c.x - s->groundP.x;
   const float dz = c.z - s->groundP.z;
   if (dx * dx + dz * dz < kGroundReprobeDist * kGroundReprobeDist) return;

   // Where the CURRENT plane sits under the body.  This is both the reference
   // height for the probe (so its self-hit filter and drop bound stay relative to
   // where the body actually is) and the fallback anchor below.
   const float planeY = plane_height_at(s->groundP, s->groundN, c.x, c.z);

   Vec3 p, nrm;
   if (probe_ground(c.x, c.z, planeY, kGroundReprobeRise, false, &p, &nrm)) {
      // Logged because the failure this exists to fix is invisible from the
      // outside: a body sinking into a hillside and a body lying on it look the
      // same in a log that only records the seed.  dy is the correction the old
      // frozen plane would have got wrong by.
      if (g_soldierRagdollDebug) {
         const float dy = p.y - planeY;
         if (dy > 0.05f || dy < -0.05f) {
            auto fn_log = get_gamelog();
            fn_log("[Ragdoll] reprobe at (%.2f, %.2f) dy=%+.3f slope=%.1fdeg\n",
                   c.x, c.z, dy,
                   std::acos(nrm.y < 1.0f ? nrm.y : 1.0f) * 57.2957795f);
         }
      }
      s->groundP = p;
      s->groundN = nrm;
      return;
   }

   // Probe failed - keep the plane exactly as it is, but slide the anchor along
   // it to the body's new position.  Without this every subsequent step re-probes
   // and fails again; with it, the next attempt comes after another
   // kGroundReprobeDist of travel.
   s->groundP = Vec3{c.x, planeY, c.z};
}

// Pushes overlapping bone capsules apart.  Runs ONCE per step, after the truss
// has converged and before ground contact - the same reasoning that keeps
// contacts out of the relaxation loop.  Inside it, ten passes of separation
// would out-vote the distance constraints and inflate the body; after it, the
// truss gets the next step to reabsorb the correction.  Ground goes last because
// it is the harder constraint: a limb pushed off another may not end up buried.
static void solve_self_collision(RagdollState* s)
{
   // Broad phase: a bounding sphere per bone, so the full segment solve only
   // runs for pairs that could possibly touch.  Most of a skeleton's ~200 bone
   // pairs are nowhere near each other, and this loop runs for every corpse on
   // every substep - up to four times a frame.
   Vec3  mid[kMaxJoints];
   float reach[kMaxJoints];
   for (int i = 0; i < s->boneCount; i++) {
      const Bone& b = s->bones[i];
      const Vec3& pa = s->pos[b.a];
      const Vec3& pb = s->pos[b.b];
      mid[i]   = {(pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f, (pa.z + pb.z) * 0.5f};
      reach[i] = v_len(v_sub(pb, pa)) * 0.5f + b.radius;
   }

   for (int i = 0; i < s->boneCount; i++) {
      const Bone& bi = s->bones[i];
      if (bi.radius <= 0.0f) continue;

      for (int j = i + 1; j < s->boneCount; j++) {
         const Bone& bj = s->bones[j];
         if (bj.radius <= 0.0f) continue;

         // Bones that meet - by index or by coincident endpoints - touch by
         // definition; the mask was resolved once at seed.
         if (bi.skip & (1u << j)) continue;

         const Vec3  sep    = v_sub(mid[i], mid[j]);
         const float reachS = reach[i] + reach[j];
         if (v_dot(sep, sep) > reachS * reachS) continue;

         const float want = bi.radius + bj.radius;

         float sT, tT;
         Vec3  c1, c2;
         closest_seg_seg(s->pos[bi.a], s->pos[bi.b], s->pos[bj.a], s->pos[bj.b],
                         &sT, &tT, &c1, &c2);

         Vec3        d   = v_sub(c1, c2);
         const float len = v_len(d);
         if (len >= want || len < 1e-6f) continue;

         const float push = (want - len) * kSelfStiffness * 0.5f;
         d.x = d.x / len * push; d.y = d.y / len * push; d.z = d.z / len * push;

         // The contact point sits at parameter s along the bone, so moving it by
         // d means moving the endpoints by d weighted (1-s) and s.  Dividing by
         // the squared weights makes the weighted result come out to exactly d
         // rather than a fraction of it.
         const float w0i = 1.0f - sT, w1i = sT;
         const float di  = w0i * w0i + w1i * w1i;
         const float w0j = 1.0f - tT, w1j = tT;
         const float dj  = w0j * w0j + w1j * w1j;

         // closest_seg_seg was handed (pos[a], pos[b]), so a is parameter 0 and
         // b is parameter 1 - weights must follow that order, not the struct's.
         if (di > 1e-6f) {
            Vec3& p0 = s->pos[bi.a];
            Vec3& p1 = s->pos[bi.b];
            p0.x += d.x * w0i / di; p0.y += d.y * w0i / di; p0.z += d.z * w0i / di;
            p1.x += d.x * w1i / di; p1.y += d.y * w1i / di; p1.z += d.z * w1i / di;
         }
         if (dj > 1e-6f) {
            Vec3& p0 = s->pos[bj.a];
            Vec3& p1 = s->pos[bj.b];
            p0.x -= d.x * w0j / dj; p0.y -= d.y * w0j / dj; p0.z -= d.z * w0j / dj;
            p1.x -= d.x * w1j / dj; p1.y -= d.y * w1j / dj; p1.z -= d.z * w1j / dj;
         }
      }
   }
}

// ---------------------------------------------------------------------------
// Solver
// ---------------------------------------------------------------------------

static void step(RagdollState* s, float dt)
{
   const int n = s->numJoints;

   // Verlet integration.  The root is simulated too - the whole body collapses
   // towards the ground plane, which in model space is where the feet were.
   const float gdt2 = kGravity * dt * dt;
   for (int i = 0; i < n; i++) {
      if (!s->sim[i]) continue;
      const Vec3 cur = s->pos[i];
      s->pos[i].x += (cur.x - s->prev[i].x) * kDamping;
      s->pos[i].y += (cur.y - s->prev[i].y) * kDamping + gdt2;
      s->pos[i].z += (cur.z - s->prev[i].z) * kDamping;
      s->prev[i] = cur;
   }

   // Gauss-Seidel relaxation, same structure as EntityCloth::SatisfyConstraints.
   // Both endpoints move half the error, so the chain converges without a pinned
   // root.  Bone constraints run at full stiffness (a bone does not stretch);
   // bend constraints run soft so the body resists folding flat without going
   // rigid.
   for (int it = 0; it < kIterations; it++) {
      for (int ci = 0; ci < s->consCount; ci++) {
         const Constraint& c = s->cons[ci];

         Vec3 d = v_sub(s->pos[c.a], s->pos[c.b]);
         const float len = v_len(d);
         if (len < 1e-6f) continue;

         const float k = ((len - c.len) / len) * 0.5f * c.stiff;
         d.x *= k; d.y *= k; d.z *= k;
         s->pos[c.a].x -= d.x; s->pos[c.a].y -= d.y; s->pos[c.a].z -= d.z;
         s->pos[c.b].x += d.x; s->pos[c.b].y += d.y; s->pos[c.b].z += d.z;
      }
   }

   // Keep the body out of itself before the floor gets the final say.
   solve_self_collision(s);

   // Contacts are resolved ONCE, after the relaxation, not inside it.  Clamping
   // every iteration lets the floor dominate the solve: each pass shoves joints
   // back onto the plane and the constraints never recover the bone lengths, so
   // the whole skeleton converges onto a single height.  Each joint stops a
   // radius above the plane so the body keeps some thickness.
   //
   // The plane may be tilted (see probe_ground), so this is a signed-distance
   // test along its normal rather than a comparison of y.  For the flat fallback
   // normal (0,1,0) it reduces exactly to the old `pos.y < groundY + radius`.
   const Vec3& P = s->groundP;
   const Vec3& N = s->groundN;

   for (int i = 0; i < n; i++) {
      if (!s->sim[i]) continue;

      const float sd = v_dot(v_sub(s->pos[i], P), N);
      if (sd >= kJointRadius) continue;

      // Push straight out along the normal to the contact offset.
      const float push = kJointRadius - sd;
      s->pos[i].x += N.x * push;
      s->pos[i].y += N.y * push;
      s->pos[i].z += N.z * push;

      // Contact friction: bleed off the velocity that runs ALONG the surface by
      // dragging the previous position towards the current one, leaving the
      // normal component alone.  On flat ground the tangent is the xz plane, so
      // this is the same x/z drag as before.
      const Vec3  vel = v_sub(s->pos[i], s->prev[i]);
      const float vn  = v_dot(vel, N);
      const Vec3  vt{vel.x - N.x * vn, vel.y - N.y * vn, vel.z - N.z * vn};
      s->prev[i].x += vt.x * kGroundFric;
      s->prev[i].y += vt.y * kGroundFric;
      s->prev[i].z += vt.z * kGroundFric;

      // And do not let the previous position sit inside the plane, or the
      // implicit velocity drives the joint straight back through it next step.
      const float sdPrev = v_dot(v_sub(s->prev[i], P), N);
      if (sdPrev < kJointRadius) {
         const float fix = kJointRadius - sdPrev;
         s->prev[i].x += N.x * fix;
         s->prev[i].y += N.y * fix;
         s->prev[i].z += N.z * fix;
      }
   }
}

// ---------------------------------------------------------------------------
// Write-back - turn particle positions into joint world matrices
// ---------------------------------------------------------------------------
// Translation is just the particle.  For rotation we keep the animated pose's
// orientation and apply the minimal ("swing") rotation that takes the bone's
// original direction onto its simulated one.  That preserves the clip's roll
// about the bone axis, which a basis rebuilt from the direction alone would
// throw away - the classic cause of limbs spinning about themselves.
//
// A leaf joint has no direction of its own, so it inherits its parent's
// orientation unchanged.

static void axis_angle_to_mat3(const Vec3& axis, float s, float c, float out[9])
{
   const float t = 1.0f - c;
   const float x = axis.x, y = axis.y, z = axis.z;
   // D3DXMatrixRotationAxis form (row-vector convention: v' = v * R).
   out[0] = c + t * x * x;     out[1] = t * x * y + s * z; out[2] = t * x * z - s * y;
   out[3] = t * x * y - s * z; out[4] = c + t * y * y;     out[5] = t * y * z + s * x;
   out[6] = t * x * z + s * y; out[7] = t * y * z - s * x; out[8] = c + t * z * z;
}

// Row-vector convention throughout: v' = v * M, so composing "apply A then B" is
// the product A * B.
static inline Vec3 mat3_xform(const Vec3& v, const float* M)
{
   return {v.x * M[0] + v.y * M[3] + v.z * M[6],
           v.x * M[1] + v.y * M[4] + v.z * M[7],
           v.x * M[2] + v.y * M[5] + v.z * M[8]};
}

// out = A * B.  out must not alias either input.
static inline void mat3_mul(const float* A, const float* B, float* out)
{
   for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
         out[i * 3 + j] = A[i * 3 + 0] * B[0 * 3 + j] +
                          A[i * 3 + 1] * B[1 * 3 + j] +
                          A[i * 3 + 2] * B[2 * 3 + j];
}

// Some unit vector perpendicular to v, chosen so the result never degenerates
// and varies continuously with v - which is what makes the 180 degree case below
// stable from frame to frame instead of flickering.
static inline Vec3 any_perpendicular(const Vec3& v)
{
   const Vec3 seed = (std::fabs(v.x) < 0.9f) ? Vec3{1.0f, 0.0f, 0.0f}
                                             : Vec3{0.0f, 1.0f, 0.0f};
   Vec3        p = v_cross(v, seed);
   const float l = v_len(p);
   if (l < 1e-6f) return {0.0f, 1.0f, 0.0f};
   p.x /= l; p.y /= l; p.z /= l;
   return p;
}

static const float kIdentity3[9] = {1.0f, 0.0f, 0.0f,
                                    0.0f, 1.0f, 0.0f,
                                    0.0f, 0.0f, 1.0f};

// Orthonormal frame from a primary direction and a secondary reference, rows
// e1/e2/e3.  Gram-Schmidt: e1 is kept exactly, the reference only resolves the
// roll about it.  Returns false if the two are collinear, leaving the frame
// undetermined.
static bool build_frame(const Vec3& primary, const Vec3& reference, float* out)
{
   const float lp = v_len(primary);
   if (lp < 1e-6f) return false;
   const Vec3 e1{primary.x / lp, primary.y / lp, primary.z / lp};

   const float d = v_dot(reference, e1);
   Vec3        e2{reference.x - e1.x * d, reference.y - e1.y * d, reference.z - e1.z * d};
   const float l2 = v_len(e2);
   if (l2 < 1e-4f) return false;   // collinear: no roll information
   e2.x /= l2; e2.y /= l2; e2.z /= l2;

   const Vec3 e3 = v_cross(e1, e2);

   out[0] = e1.x; out[1] = e1.y; out[2] = e1.z;
   out[3] = e2.x; out[4] = e2.y; out[5] = e2.z;
   out[6] = e3.x; out[7] = e3.y; out[8] = e3.z;
   return true;
}

static void write_back(RagdollState* s, float* world, const uint8_t* joints,
                       const float* entityMat)
{
   const int n = s->numJoints;

   // The solver works in world space but the matrices we are writing are model
   // space, so bring the simulated positions back first.  Everything below then
   // stays entirely model-space, which is what keeps the swing math correct: it
   // compares an animated direction against a simulated one, and those two have
   // to be in the same space.
   Vec3 simT[kMaxJoints];
   for (int i = 0; i < n; i++)
      simT[i] = s->sim[i] ? xform_point_inv(entityMat, s->pos[i]) : Vec3{};

   // Snapshot the animated translations too: the write loop overwrites row 3 in
   // place, and a joint's swing needs its child's ORIGINAL position.
   Vec3 origT[kMaxJoints];
   for (int i = 0; i < n; i++) {
      const float* m = world + i * kMatrixFloats;
      origT[i] = {m[kMatTransX], m[kMatTransX + 1], m[kMatTransX + 2]};
   }

   // Pass 1 - orientation.
   //
   // A joint's frame CANNOT be recovered from its bone direction alone.  One
   // direction fixes two of three degrees of freedom, and the missing one is
   // exactly the twist about that direction, so a swing-only reconstruction
   // reports zero rotation for a body rolling about its own spine even though
   // every simulated limb has swung right around it.  The torso mesh then keeps
   // facing the way it died while the arms and legs do not, which reads as the
   // whole body being twisted - and at a half turn it reads as 180 degrees,
   // because that is what it is.
   //
   // Two earlier attempts here treated the symptom.  Guarding the degenerate
   // cross(a, b) near a reversal only removes the flicker, not the wrong roll;
   // inheriting the parent's rotation only makes neighbouring joints AGREE on a
   // roll that is uniformly wrong, since it ultimately derives from a root swing
   // that carries no twist either.  Neither can work, because the information
   // simply is not present in a single direction.
   //
   // So each joint gets a second reference joint off its bone axis (picked at
   // seed, see pick_roll_reference in seed()), giving a full orthonormal frame in
   // both the animated and simulated poses.  The rotation between those two
   // frames is the joint's absolute orientation change, twist included.
   //
   // Two fallbacks remain, in order:
   //   - no usable roll reference: swing only, as a residual on the parent's
   //     rotation, so the roll is at least consistent down the chain
   //   - no bone direction at all (the head; the hands, whose only child is a
   //     dropped aim node): keep the parent's rotation outright
   //
   // The parent's rotation must therefore be resolved before the child's, and
   // cparent is very often a HIGHER index than the child (the pelvis is j25 with
   // children j04/j05/j11/j21/j22), so index order is not hierarchy order.
   float rot[kMaxJoints][9];
   bool  hasRot[kMaxJoints] = {};

   // Sweep repeatedly, resolving whatever has a resolved parent, until nothing
   // moves.
   for (int pass = 0; pass < kMaxJoints; pass++) {
      bool progress = false;

      for (int i = 0; i < n; i++) {
         if (!s->sim[i] || hasRot[i]) continue;

         const int    p  = s->cparent[i];
         const bool   isFreeRoot = (p < 0 || p >= n || !s->sim[p]);
         if (!isFreeRoot && !hasRot[p]) continue;   // parent not resolved yet

         const float* Rp = isFreeRoot ? kIdentity3 : rot[p];

         // Default: move exactly as the parent did.
         std::memcpy(rot[i], Rp, sizeof(rot[i]));

         const int c = s->dirChild[i];
         const int r = s->refJoint[i];

         // Preferred path: a full frame, animated and simulated, from the bone
         // direction plus the roll reference.  R is then the absolute rotation
         // between the two frames and captures TWIST, which is the whole reason
         // the reference exists - a body rolling about its own spine barely
         // changes any bone direction, so a swing-only reconstruction reports no
         // rotation at all while the simulated limbs have swung right around.
         bool framed = false;
         if (c >= 0 && c < n && r >= 0 && r < n) {
            float A[9], B[9];
            if (build_frame(v_sub(origT[c], origT[i]), v_sub(origT[r], origT[i]), A) &&
                build_frame(v_sub(simT[c], simT[i]), v_sub(simT[r], simT[i]), B)) {
               // R = transpose(A) * B: decompose into the animated basis, then
               // rebuild in the simulated one.
               const float At[9] = {A[0], A[3], A[6],
                                    A[1], A[4], A[7],
                                    A[2], A[5], A[8]};
               float composed[9];
               mat3_mul(At, B, composed);
               std::memcpy(rot[i], composed, sizeof(rot[i]));
               framed = true;
            }
         }

         // Fallback: no usable roll reference (a joint whose neighbours are all
         // strung out along its own axis).  Solve the swing only, as a residual
         // on the parent's rotation so it at least inherits a consistent roll.
         if (!framed && c >= 0 && c < n) {
            Vec3        a  = v_sub(origT[c], origT[i]);
            Vec3        b  = v_sub(simT[c], simT[i]);
            const float la = v_len(a), lb = v_len(b);

            if (la > 1e-6f && lb > 1e-6f) {
               a.x /= la; a.y /= la; a.z /= la;
               b.x /= lb; b.y /= lb; b.z /= lb;

               // Where the parent's rotation alone would have put this bone.
               const Vec3 ap = mat3_xform(a, Rp);

               Vec3        axis = v_cross(ap, b);
               const float sn   = v_len(axis);
               const float cs   = v_dot(ap, b);

               float dR[9];
               bool  haveDelta = false;

               // The axis is cross(ap, b) divided by sn, so a small sn amplifies
               // any noise in the cross product by 1/sn.  The threshold is the
               // point past which that amplification stops being tolerable, not
               // just the exact singularity.
               if (sn > 1e-3f) {
                  axis.x /= sn; axis.y /= sn; axis.z /= sn;
                  axis_angle_to_mat3(axis, sn, cs, dR);
                  haveDelta = true;
               } else if (cs < 0.0f) {
                  // Exactly reversed: no axis is implied, but leaving it gives a
                  // bone pointing the wrong way.  Turn it about a perpendicular
                  // derived from the direction itself, which is deterministic and
                  // moves continuously with the bone instead of flickering.
                  const Vec3 perp = any_perpendicular(ap);
                  axis_angle_to_mat3(perp, 0.0f, -1.0f, dR);
                  haveDelta = true;
               }
               // else: sn ~ 0 with cs > 0, the bone is already where the parent
               // put it, so the inherited rotation is already correct.

               if (haveDelta) {
                  float composed[9];
                  mat3_mul(Rp, dR, composed);       // parent first, then the bend
                  std::memcpy(rot[i], composed, sizeof(rot[i]));
               }
            }
         }

         hasRot[i] = true;
         progress  = true;
      }

      if (!progress) break;
   }

   // Pass 3 - apply.
   for (int i = 0; i < n; i++) {
      // Scaffolding and aim nodes keep whatever the animator produced: they are
      // not simulated, so there is no simulated position to move them to.
      if (!s->sim[i]) continue;

      float* m = world + i * kMatrixFloats;

      if (hasRot[i]) {
         const float* r = rot[i];
         // 3x3 rotation part only: M_new = M_orig * R.  Row 3 is set below, so
         // the translation is never run through R.
         for (int row = 0; row < 3; row++) {
            const float e0 = m[row * 4 + 0], e1 = m[row * 4 + 1], e2 = m[row * 4 + 2];
            m[row * 4 + 0] = e0 * r[0] + e1 * r[3] + e2 * r[6];
            m[row * 4 + 1] = e0 * r[1] + e1 * r[4] + e2 * r[7];
            m[row * 4 + 2] = e0 * r[2] + e1 * r[5] + e2 * r[8];
         }
      }

      m[kMatTransX + 0] = simT[i].x;
      m[kMatTransX + 1] = simT[i].y;
      m[kMatTransX + 2] = simT[i].z;
   }

   // Pass 4 - drag the collapsed scaffolding along, exactly as the engine's own
   // dummy-root branch does.
   for (int i = 0; i < n; i++) {
      const int f = s->follow[i];
      if (f < 0 || f >= n) continue;
      std::memcpy(world + i * kMatrixFloats, world + f * kMatrixFloats,
                  kMatrixFloats * sizeof(float));
   }
}

// ---------------------------------------------------------------------------
// Hook: SoldierAnimator::ApplyProceduralAnimationAndBuildWorldMatrices
// ---------------------------------------------------------------------------
// __thiscall(SoldierAnimator*, float dt) - the float rides on the stack, so the
// __fastcall spelling with a dummy EDX matches it exactly.

using fn_ApplyProcedural_t = void(__fastcall*)(void* ecx, void* edx, float dt);

static fn_ApplyProcedural_t original_ApplyProcedural = nullptr;

static void __fastcall hooked_ApplyProcedural(void* ecx, void* /*edx*/, float dt)
{
   original_ApplyProcedural(ecx, nullptr, dt);

   if (!g_soldierRagdollEnabled || !ecx) return;

   __try {
      uint8_t* animator = (uint8_t*)ecx;

      uint8_t* owner = *(uint8_t**)(animator + kSAOwner);
      if (!owner) return;

      const uint8_t healthFlags = *(uint8_t*)(owner + kHealthFlags);
      if ((healthFlags >> kIsAliveBit) & 1) {
         // Alive: drop any state so a respawn into the same pooled animator
         // does not resume the previous corpse's simulation.
         release_state(ecx);
         return;
      }

      // The corpse's own world transform.  Used to move the sim in and out of
      // world space; the contact plane is taken from it once, at seed, and then
      // frozen (see seed()) rather than tracked per frame.
      const float* entityMat = (const float*)(owner + kEntityMatrix);
      if (!matrix_is_sane(entityMat)) return;

      uint8_t* skel   = animator + kSAZephyrSkeleton;
      uint8_t* shared = *(uint8_t**)(skel + kZSShared);
      if (!shared) return;

      uint8_t* joints = *(uint8_t**)(shared + kZSSJoints);
      int      n      = (int)*(uint32_t*)(shared + kZSSNumJoints);
      if (!joints || n <= 1) return;
      if (n > kMaxJoints) n = kMaxJoints;

      float* world = (float*)(skel + kZSWorldMatrices);

      RagdollState* s = find_state(ecx);
      if (!s) {
         s = acquire_state(ecx);
         if (!s) return;
      }
      s->touchTick = ++g_tick;

      if (!s->seeded) {
         // Read on the first dead frame, which is the frame after DirectionKill
         // wrote the impulse, so this still carries the blast.  Read at seed and
         // never again: from here the body is on its own, and the entity's later
         // motion must not feed back into a sim it is no longer driving.
         const float* ev = (const float*)(owner + kEntityVelocity);
         seed(s, world, joints, n, entityMat, Vec3{ev[0], ev[1], ev[2]});
      }
      if (s->numJoints != n) return;        // skeleton swapped under us
      if (s->simCount < kMinSimJoints) return; // nothing bone-like to simulate

      // Park the sim once the body has settled, but keep writing the final pose
      // so the death clip cannot creep back in underneath it.
      if (s->age < kSettleTime) {
         // Keep the contact plane under the body before integrating against it,
         // so a corpse thrown across uneven ground lands on the surface it is
         // actually over rather than the one it died on.
         reprobe_ground(s);

         s->age += dt;
         s->accum += dt;
         int steps = 0;
         while (s->accum >= kFixedStep && steps < kMaxSubsteps) {
            step(s, kFixedStep);
            s->accum -= kFixedStep;
            steps++;
         }
         if (s->accum > kFixedStep * kMaxSubsteps) s->accum = 0.0f; // hitch guard
      }

      write_back(s, world, joints, entityMat);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A torn-down animator mid-frame must never take the game with it.
      release_state(ecx);
   }
}

// ---------------------------------------------------------------------------
// Install / Uninstall / Reset
// ---------------------------------------------------------------------------

void soldier_ragdoll_install(uintptr_t exe_base)
{
   if (!g_soldierRagdollEnabled) return;

   // Modtools only for now - the Steam/GOG address is not ported yet, and the
   // generated table leaves it 0 there.
   if (g_addr->soldier_apply_procedural == 0) return;

   original_ApplyProcedural =
      (fn_ApplyProcedural_t)resolve(exe_base, g_addr->soldier_apply_procedural);

   // Optional: without it probe_ground() fails and every corpse falls back to the
   // flat measured plane, which is what this feature did before.
   ray_hit_init(exe_base);

   soldier_ragdoll_reset();

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_ApplyProcedural, hooked_ApplyProcedural);
   DetourTransactionCommit();
}

void soldier_ragdoll_uninstall()
{
   if (!original_ApplyProcedural) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_ApplyProcedural, hooked_ApplyProcedural);
   DetourTransactionCommit();
}

void soldier_ragdoll_reset()
{
   std::memset(g_ragdolls, 0, sizeof(g_ragdolls));
   g_tick = 0;
   g_loggedSeed = false;
}
