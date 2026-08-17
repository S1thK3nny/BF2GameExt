#include "pch.h"
#include "flyer_boost_animation.hpp"
#include "core/resolve.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"

#include <cstring>
#include <detours.h>

// =============================================================================
// Flyer Boost Animation
//
// Hooks InitAnimations to find "boost" in the animation bank.
// Before Render, computes the base pose (takeoff at progress=1.0), then
// blends the boost animation on top using boostRatio.  This handles partial
// animations (fewer joints than the skeleton) correctly — un-keyed joints
// keep the base flying pose instead of snapping to bind pose.
// =============================================================================

// ---------------------------------------------------------------------------
// Offsets (from struct_base = renderThis - 0x94)
// ---------------------------------------------------------------------------

// Per build.  Every Steam value below was read out of the disassembly, not
// inferred: the struct_base ones from EntityFlyer::TakeOff 0x4b3c60 and
// EntityCarrier::UpdateLandedHeight 0x4974b0, and the renderThis / class ones
// from EntityFlyer::Render 0x4AB040 at the exact points modtools' Render
// 0x4f6970 touches the corresponding field.  They happen to follow the usual
// split (instance -0x40, class -0xC8) but that was the check, not the source.
static uintptr_t kSB_FlightState = 0x5A4;   // Steam 0x564  Land/TakeOff cmp 2/1/3
static uintptr_t kSB_Progress    = 0x5A8;   // Steam 0x568  render [ESI+0x4d4]+0x94
static uintptr_t kSB_Flags       = 0x5F4;   // Steam 0x5B4  TakeOff TEST [EDI+0x5b4],8
static uintptr_t kSB_ClassPtr    = 0x66C;   // Steam 0x62C  render [ESI+0x598]+0x94

// Offsets from renderThis
static uintptr_t kRT_AnimGate    = 0x5B8;   // Steam 0x578   CMP [ESI+0x578],0 @004ab0d3
static uintptr_t kRT_ClassPtr    = 0x5D8;   // Steam 0x598   MOV EDX,[ESI+0x598] @004ab088
static uintptr_t kRT_Skeleton    = 0x64C;   // Steam 0x60C   ADD ESI,0x60c @004ab256
static uintptr_t kRT_PoseDyn     = 0xE5C;   // Steam 0xE1C   LEA EDI,[ESI+0xe1c] @004ab20a
static uintptr_t kRT_Ref17dc     = 0x17DC;  // Steam 0x179C  MOV EAX,[ESI+0x179c] @004ab225
static uintptr_t kRT_RedPose     = 0x180C;  // Steam 0x17CC  LEA ECX,[ESI+0x17cc] @004ab45d

// Class offsets
static uintptr_t kCls_AnimObj     = 0x878;  // Steam 0x7B0  MOV [ESI+0x7b0],EDI @004b9748
static uintptr_t kCls_TakeoffAnim = 0x87C;  // Steam 0x7B4  MOV [ESI+0x7b4],EAX @004b9780

// The clip's own length sets the transition time: BF2 animations are munged at
// a fixed 30 fps, so (frames-1)/30 is the authored duration.  A one-frame clip
// has no duration to read, and anything shorter than the floor would blend in a
// single render tick, so both fall back to kMinTransitionTime.
static constexpr float kAnimFps = 30.0f;
static constexpr float kMinTransitionTime = 0.05f;
static constexpr uintptr_t kRenderThisToBase = 0x94;

// Global identity matrix used by Skeleton::Finalize
static void* g_identityMatrix = nullptr;  // 0x00CF6830 relocated

// GameLoop::sPauseMode — true when game is ESC-paused
static uint8_t* g_pauseMode = nullptr;

// ---------------------------------------------------------------------------
// Engine function types (all from Render disassembly)
// ---------------------------------------------------------------------------

// ZephyrPoseDyn<32>::SetAnimation — thiscall(poseDyn, anim, fps)
using fn_SetAnimation_t = void(__fastcall*)(void* ecx, void* edx, void* anim, float fps);

// SetAnimTime — thiscall(poseDyn, time)  [FUN_0082a9c0]
using fn_SetAnimTime_t = void(__fastcall*)(void* ecx, void* edx, float time);

// ZephyrPoseStatic<32>::ctor/dtor — thiscall(this)
using fn_PoseStaticCtor_t = void(__fastcall*)(void* ecx, void* edx);
using fn_PoseStaticDtor_t = void(__fastcall*)(void* ecx, void* edx);

// ZephyrSkeleton<32>::Open — thiscall(skeleton, shared, &staticPose)
using fn_SkeletonOpen_t = void(__fastcall*)(void* ecx, void* edx, void* shared, void* staticPose);

// ZephyrPoseStatic<32>::Open — thiscall(staticPose, skeleton)
using fn_PoseStaticOpen_t = void(__fastcall*)(void* ecx, void* edx, void* skeleton);

// ZephyrPoseStatic<32>::Set — thiscall(staticPose, poseDyn, bool)
using fn_PoseStaticSet_t = void(__fastcall*)(void* ecx, void* edx, void* poseDyn, int bFull);

// PoseStatic::Blend — thiscall(staticPose, poseDyn, factor)
using fn_PoseStaticBlend_t = void(__fastcall*)(void* ecx, void* edx, void* poseDyn, float factor);

// Skeleton::Finalize — thiscall(skeleton, identityMatrix)
using fn_SkeletonFinalize_t = void(__fastcall*)(void* ecx, void* edx, void* identityMtx);

// RedPose::ConvertFromZephyrPose_Skel32 — thiscall(redPose, skeleton)
using fn_ConvertPose_t = void(__fastcall*)(void* ecx, void* edx, void* skeleton);

// EntityFlyerClass::InitAnimations
using fn_InitAnimations_t = int(__fastcall*)(void* ecx, void* edx, const char* bankName);

// Animation-bank lookup.  THE TWO BUILDS EXPOSE DIFFERENT LAYERS OF THIS CALL,
// so both the `this` pointer and the key differ:
//
//   modtools 0x803750 is the WRAPPER — __thiscall(RedAnimation*, const char*).
//     It does `[this+0x14]` to reach the ZephyrAnimBank and hashes the name
//     itself (`call 0x7e1c10`) before delegating to the real find (0x85c300).
//
//   Steam 0x72ba40 / GOG 0x72cb10 are the INNER find — __thiscall(
//     ZephyrAnimBank*, uint32 hash).  Release inlined the wrapper away, so the
//     caller must do the +0x14 step AND pre-hash the name.  Confirmed straight
//     out of EntityFlyerClass::InitAnimations itself, which looks up "takeoff"
//     and "fins" as:
//         push <name>; call pbl_temp_hash; mov ecx,[edi+0x14]; push eax; call find
//
// Getting either half wrong just returns null, silently, with no error — the
// feature then no-ops.  anim_bank_append.cpp already does both correctly
// (kRA_ZephyrAnimBank = 0x14 plus a hash), which is the model followed here.
using fn_AnimBankFindName_t = void*(__fastcall*)(void* ecx, void* edx, const char* name);
using fn_AnimBankFindHash_t = void*(__fastcall*)(void* ecx, void* edx, uint32_t hash);

// RedAnimation -> ZephyrAnimBank (same constant as anim_bank_append.cpp).
static constexpr int kRA_ZephyrAnimBank = 0x14;

// PblHash temp-hash helper — __cdecl(const char*), hash returned in EAX.
// This is the exact function InitAnimations uses for its own lookups.
using fn_TempHash_t = uint32_t(__cdecl*)(const char*);

// ---------------------------------------------------------------------------
// Resolved function pointers
// ---------------------------------------------------------------------------

static fn_SetAnimation_t    fn_SetAnimation    = nullptr;
static fn_SetAnimTime_t     fn_SetAnimTime     = nullptr;
static fn_PoseStaticCtor_t  fn_PoseStaticCtor  = nullptr;
static fn_PoseStaticDtor_t  fn_PoseStaticDtor  = nullptr;
static fn_SkeletonOpen_t    fn_SkeletonOpen    = nullptr;
static fn_PoseStaticOpen_t  fn_PoseStaticOpen  = nullptr;
static fn_PoseStaticSet_t   fn_PoseStaticSet   = nullptr;
static fn_PoseStaticBlend_t fn_PoseStaticBlend = nullptr;
static fn_SkeletonFinalize_t fn_SkeletonFinalize = nullptr;
static fn_ConvertPose_t     fn_ConvertPose     = nullptr;
static fn_InitAnimations_t  original_InitAnimations = nullptr;
static void*                fn_AnimBankFind    = nullptr;
static fn_TempHash_t        fn_TempHash        = nullptr;

// true on Steam/GOG: fn_AnimBankFind is the inner find (ZephyrAnimBank + hash).
static bool                 s_bankFindIsInner  = false;
// Hash of "boost", computed lazily — see note at the use site.
static uint32_t             s_boostHash = 0;

// ---------------------------------------------------------------------------
// Class sidecar
// ---------------------------------------------------------------------------

static constexpr int kMaxClasses = 32;

struct BoostClassEntry {
   void* classPtr;
   void* animBoost;
};

static BoostClassEntry g_cls[kMaxClasses] = {};
static int             g_clsCount = 0;

// ---------------------------------------------------------------------------
// Instance sidecar
// ---------------------------------------------------------------------------

static constexpr int kMaxInstances = 32;

struct BoostInstance {
   void*  structBase;
   float  boostRatio;
   DWORD  lastTickMs;
};

static BoostInstance g_inst[kMaxInstances] = {};

// ---------------------------------------------------------------------------
// Per-render saved state
// ---------------------------------------------------------------------------

static struct {
   int*  animGateSlot;
   int   savedAnimGate;
   bool  active;
} g_saved = {};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static BoostClassEntry* findClass(void* classPtr)
{
   for (int i = 0; i < g_clsCount; i++)
      if (g_cls[i].classPtr == classPtr) return &g_cls[i];
   return nullptr;
}

static BoostInstance* findOrCreateInst(void* structBase)
{
   for (int i = 0; i < kMaxInstances; i++)
      if (g_inst[i].structBase == structBase) return &g_inst[i];
   for (int i = 0; i < kMaxInstances; i++) {
      if (!g_inst[i].structBase) {
         g_inst[i] = { structBase, 0.0f, GetTickCount() };
         return &g_inst[i];
      }
   }
   return nullptr;
}

// ---------------------------------------------------------------------------
// Hook: EntityFlyerClass::InitAnimations
// ---------------------------------------------------------------------------

static int __fastcall hooked_InitAnimations(void* ecx, void* edx, const char* bankName)
{
   int ret = original_InitAnimations(ecx, edx, bankName);

   void* bank = *(void**)((char*)ecx + kCls_AnimObj);
   if (!bank) return ret;

   void* boostAnim = nullptr;
   if (s_bankFindIsInner) {
      // Release: do what the modtools wrapper did internally — step to the
      // ZephyrAnimBank, then look up a pre-computed hash.
      void* zephyrBank = *(void**)((char*)bank + kRA_ZephyrAnimBank);
      if (!zephyrBank || !fn_TempHash) return ret;

      // Hash lazily, never at install: install runs from dllmain with the exe
      // sections mapped PAGE_READWRITE (non-executable), so calling a game
      // function there faults under DEP.  This hook only runs at runtime.
      if (s_boostHash == 0) s_boostHash = fn_TempHash("boost");

      boostAnim = ((fn_AnimBankFindHash_t)fn_AnimBankFind)(zephyrBank, nullptr, s_boostHash);
   }
   else {
      boostAnim = ((fn_AnimBankFindName_t)fn_AnimBankFind)(bank, nullptr, "boost");
   }
   if (!boostAnim) return ret;

   BoostClassEntry* entry = findClass(ecx);
   if (!entry) {
      if (g_clsCount >= kMaxClasses) return ret;
      entry = &g_cls[g_clsCount++];
      entry->classPtr = ecx;
   }
   entry->animBoost = boostAnim;
   return ret;
}

// ---------------------------------------------------------------------------
// Render integration — compute blended pose, disable vanilla animation
// ---------------------------------------------------------------------------

// ZephyrPoseStatic<32> is 0x388 bytes (from Render stack frame analysis)
static constexpr int kPoseStaticSize = 0x388;

bool flyer_boost_anim_render_prepare(char* structBase)
{
   g_saved.active = false;
   if (g_clsCount == 0) return false;

   __try {
      char* renderThis = structBase + kRenderThisToBase;

      void* classPtr = *(void**)(structBase + kSB_ClassPtr);
      if (!classPtr) return false;

      BoostClassEntry* cls = findClass(classPtr);
      if (!cls || !cls->animBoost) return false;

      int state = *(int*)(structBase + kSB_FlightState);
      uint8_t flags = *(uint8_t*)(structBase + kSB_Flags);
      bool isBoosting = (flags & 0x04) != 0 && state == 2;

      BoostInstance* inst = findOrCreateInst(structBase);
      if (!inst) return false;

      uint16_t bsFrames = *(uint16_t*)((char*)cls->animBoost + 8);
      float transitionTime = (float)(bsFrames - 1) / kAnimFps;
      if (!(transitionTime > kMinTransitionTime)) transitionTime = kMinTransitionTime;

      // Update ratio (freeze during pause)
      DWORD now = GetTickCount();
      float dt = 0.0f;
      if (!g_pauseMode || !*g_pauseMode) {
         dt = (float)(now - inst->lastTickMs) / 1000.0f;
         if (dt > 0.5f) dt = 0.5f;
      }
      inst->lastTickMs = now;

      float target = isBoosting ? 1.0f : 0.0f;
      float step = dt / transitionTime;
      if (inst->boostRatio < target) {
         inst->boostRatio += step;
         if (inst->boostRatio > target) inst->boostRatio = target;
      } else if (inst->boostRatio > target) {
         inst->boostRatio -= step;
         if (inst->boostRatio < target) inst->boostRatio = target;
      }

      if (state == 5) {
         inst->structBase = nullptr;
         inst->boostRatio = 0.0f;
         return false;
      }

      if (inst->boostRatio < 0.001f) return false;

      // --- Compute blended pose ---

      void* poseDyn  = renderThis + kRT_PoseDyn;
      void* skeleton = renderThis + kRT_Skeleton;
      void* skelShared = *(void**)(renderThis + kRT_Skeleton);  // first field = m_pShared
      void* redPose  = renderThis + kRT_RedPose;

      void* takeoffAnim = *(void**)((char*)classPtr + kCls_TakeoffAnim);
      if (!takeoffAnim) return false;  // need takeoff as base

      uint16_t tkFrames = *(uint16_t*)((char*)takeoffAnim + 8);

      // Step 1: Compute base pose (takeoff at full progress = flying pose)
      fn_SetAnimation(poseDyn, nullptr, takeoffAnim, kAnimFps);
      float baseTime = (float)(tkFrames - 1) * *(float*)(structBase + kSB_Progress);
      fn_SetAnimTime(poseDyn, nullptr, baseTime);

      // Create ZephyrPoseStatic<32> on stack (aligned to 16 bytes)
      __declspec(align(16)) char poseStaticBuf[kPoseStaticSize];
      void* staticPose = poseStaticBuf;

      fn_PoseStaticCtor(staticPose, nullptr);
      fn_SkeletonOpen(skeleton, nullptr, skelShared, staticPose);
      fn_PoseStaticOpen(staticPose, nullptr, skeleton);
      fn_PoseStaticSet(staticPose, nullptr, poseDyn, 1);

      // Step 2: Blend boost animation on top (smoothstep for ease-in/out)
      float t = inst->boostRatio;
      float smooth = t * t * (3.0f - 2.0f * t);

      fn_SetAnimation(poseDyn, nullptr, cls->animBoost, kAnimFps);
      float boostTime = (float)(bsFrames - 1) * smooth;
      fn_SetAnimTime(poseDyn, nullptr, boostTime);
      fn_PoseStaticBlend(staticPose, nullptr, poseDyn, smooth);

      // Step 3: Finalize and write to RedPose
      fn_SkeletonFinalize(skeleton, nullptr, g_identityMatrix);
      fn_ConvertPose(redPose, nullptr, skeleton);

      // Cleanup
      fn_PoseStaticDtor(staticPose, nullptr);

      // Step 4: Disable vanilla animation — it will use our pre-computed RedPose
      g_saved.animGateSlot = (int*)(renderThis + kRT_AnimGate);
      g_saved.savedAnimGate = *g_saved.animGateSlot;
      *g_saved.animGateSlot = 0;
      g_saved.active = true;
      return true;

   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

void flyer_boost_anim_render_restore(char* structBase)
{
   if (!g_saved.active) return;
   __try {
      *g_saved.animGateSlot = g_saved.savedAnimGate;
   } __except (EXCEPTION_EXECUTE_HANDLER) {}
   g_saved.active = false;
}

// ---------------------------------------------------------------------------
// Install / Uninstall / Reset
// ---------------------------------------------------------------------------

void flyer_boost_anim_install(uintptr_t exe_base)
{
   // Both retail builds share these offsets (same source, same toolchain).
   if (g_build == GameBuild::Steam || g_build == GameBuild::GOG) {
      kSB_FlightState  = 0x564;
      kSB_Progress     = 0x568;
      kSB_Flags        = 0x5B4;
      kSB_ClassPtr     = 0x62C;
      kRT_AnimGate     = 0x578;
      kRT_ClassPtr     = 0x598;
      kRT_Skeleton     = 0x60C;
      kRT_PoseDyn      = 0xE1C;
      kRT_Ref17dc      = 0x179C;
      kRT_RedPose      = 0x17CC;
      kCls_AnimObj     = 0x7B0;
      kCls_TakeoffAnim = 0x7B4;
   }

   // Everything this feature needs is in the per-build table; bail on any build
   // where a piece is unmapped rather than resolving a zero into a call target.
   if (!g_addr->zephyr_anim_bank_find || !g_addr->flyer_init_animations ||
       !g_addr->zephyr_pose_dyn_set_anim || !g_addr->zephyr_pose_dyn_set_time ||
       !g_addr->zephyr_pose_static_ctor || !g_addr->zephyr_pose_static_dtor ||
       !g_addr->zephyr_skeleton_open || !g_addr->zephyr_pose_static_open ||
       !g_addr->zephyr_pose_static_set || !g_addr->zephyr_pose_static_blend ||
       !g_addr->zephyr_skeleton_finalize || !g_addr->red_pose_convert_skel32 ||
       !g_addr->g_identity_matrix || !g_addr->gameloop_pause_mode)
      return;

   fn_AnimBankFind         = (void*)                resolve(exe_base, g_addr->zephyr_anim_bank_find);

   // Release builds expose the inner find (ZephyrAnimBank* + hash) rather than
   // modtools' name-taking wrapper, so they also need the temp-hash helper.
   s_bankFindIsInner = (g_build != GameBuild::Modtools);
   s_boostHash       = 0;
   if (s_bankFindIsInner) {
      if (!g_addr->pbl_temp_hash) return;
      fn_TempHash = (fn_TempHash_t)resolve(exe_base, g_addr->pbl_temp_hash);
   }

   original_InitAnimations = (fn_InitAnimations_t)  resolve(exe_base, g_addr->flyer_init_animations);

   fn_SetAnimation     = (fn_SetAnimation_t)    resolve(exe_base, g_addr->zephyr_pose_dyn_set_anim);
   fn_SetAnimTime      = (fn_SetAnimTime_t)     resolve(exe_base, g_addr->zephyr_pose_dyn_set_time);
   fn_PoseStaticCtor   = (fn_PoseStaticCtor_t)  resolve(exe_base, g_addr->zephyr_pose_static_ctor);
   fn_PoseStaticDtor   = (fn_PoseStaticDtor_t)  resolve(exe_base, g_addr->zephyr_pose_static_dtor);
   fn_SkeletonOpen     = (fn_SkeletonOpen_t)    resolve(exe_base, g_addr->zephyr_skeleton_open);
   fn_PoseStaticOpen   = (fn_PoseStaticOpen_t)  resolve(exe_base, g_addr->zephyr_pose_static_open);
   fn_PoseStaticSet    = (fn_PoseStaticSet_t)   resolve(exe_base, g_addr->zephyr_pose_static_set);
   fn_PoseStaticBlend  = (fn_PoseStaticBlend_t) resolve(exe_base, g_addr->zephyr_pose_static_blend);
   fn_SkeletonFinalize = (fn_SkeletonFinalize_t)resolve(exe_base, g_addr->zephyr_skeleton_finalize);
   fn_ConvertPose      = (fn_ConvertPose_t)     resolve(exe_base, g_addr->red_pose_convert_skel32);
   g_identityMatrix    = (void*)                resolve(exe_base, g_addr->g_identity_matrix);
   g_pauseMode         = (uint8_t*)             resolve(exe_base, g_addr->gameloop_pause_mode);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_InitAnimations, hooked_InitAnimations);
   DetourTransactionCommit();
}

void flyer_boost_anim_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_InitAnimations)
      DetourDetach(&(PVOID&)original_InitAnimations, hooked_InitAnimations);
   DetourTransactionCommit();
}

void flyer_boost_anim_reset()
{
   memset(g_cls, 0, sizeof(g_cls));
   g_clsCount = 0;
   memset(g_inst, 0, sizeof(g_inst));
}
