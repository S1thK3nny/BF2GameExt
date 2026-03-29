#include "pch.h"
#include "flyer_boost_animation.hpp"
#include "core/resolve.hpp"
#include "core/game_addrs.hpp"

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

static constexpr uintptr_t kSB_FlightState = 0x5A4;
static constexpr uintptr_t kSB_Progress    = 0x5A8;
static constexpr uintptr_t kSB_Flags       = 0x5F4;
static constexpr uintptr_t kSB_ClassPtr    = 0x66C;

// Offsets from renderThis
static constexpr uintptr_t kRT_AnimGate    = 0x5B8;   // int: if 0, skip animation
static constexpr uintptr_t kRT_ClassPtr    = 0x5D8;   // EntityFlyerClass*
static constexpr uintptr_t kRT_Skeleton    = 0x64C;   // ZephyrSkeleton<32>
static constexpr uintptr_t kRT_PoseDyn     = 0xE5C;   // ZephyrPoseDyn<32>
static constexpr uintptr_t kRT_Ref17dc     = 0x17DC;  // void*: anim ref (for nFrames)
static constexpr uintptr_t kRT_RedPose     = 0x180C;  // RedPose output

// Class offsets
static constexpr uintptr_t kCls_AnimObj     = 0x878;
static constexpr uintptr_t kCls_TakeoffAnim = 0x87C;

static constexpr float kTransitionTime = 0.6f;
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

// ZephyrAnimBank::Find
using fn_AnimBankFind_t = void*(__fastcall*)(void* ecx, void* edx, const char* name);

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
static fn_AnimBankFind_t    fn_AnimBankFind    = nullptr;

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

   void* boostAnim = fn_AnimBankFind(bank, nullptr, "boost");
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

      // Update ratio (freeze during pause)
      DWORD now = GetTickCount();
      float dt = 0.0f;
      if (!g_pauseMode || !*g_pauseMode) {
         dt = (float)(now - inst->lastTickMs) / 1000.0f;
         if (dt > 0.5f) dt = 0.5f;
      }
      inst->lastTickMs = now;

      float target = isBoosting ? 1.0f : 0.0f;
      float step = dt / kTransitionTime;
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

      // Get frame counts
      uint16_t tkFrames = *(uint16_t*)((char*)takeoffAnim + 8);
      uint16_t bsFrames = *(uint16_t*)((char*)cls->animBoost + 8);

      // Step 1: Compute base pose (takeoff at full progress = flying pose)
      fn_SetAnimation(poseDyn, nullptr, takeoffAnim, 30.0f);
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

      fn_SetAnimation(poseDyn, nullptr, cls->animBoost, 30.0f);
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
   using namespace game_addrs::modtools;

   fn_AnimBankFind         = (fn_AnimBankFind_t)    resolve(exe_base, zephyr_anim_bank_find);
   original_InitAnimations = (fn_InitAnimations_t)  resolve(exe_base, flyer_init_animations);

   fn_SetAnimation     = (fn_SetAnimation_t)    resolve(exe_base, zephyr_pose_dyn_set_anim);
   fn_SetAnimTime      = (fn_SetAnimTime_t)     resolve(exe_base, zephyr_pose_dyn_set_time);
   fn_PoseStaticCtor   = (fn_PoseStaticCtor_t)  resolve(exe_base, zephyr_pose_static_ctor);
   fn_PoseStaticDtor   = (fn_PoseStaticDtor_t)  resolve(exe_base, zephyr_pose_static_dtor);
   fn_SkeletonOpen     = (fn_SkeletonOpen_t)    resolve(exe_base, zephyr_skeleton_open);
   fn_PoseStaticOpen   = (fn_PoseStaticOpen_t)  resolve(exe_base, zephyr_pose_static_open);
   fn_PoseStaticSet    = (fn_PoseStaticSet_t)   resolve(exe_base, zephyr_pose_static_set);
   fn_PoseStaticBlend  = (fn_PoseStaticBlend_t) resolve(exe_base, zephyr_pose_static_blend);
   fn_SkeletonFinalize = (fn_SkeletonFinalize_t)resolve(exe_base, zephyr_skeleton_finalize);
   fn_ConvertPose      = (fn_ConvertPose_t)     resolve(exe_base, red_pose_convert_skel32);
   g_identityMatrix    = (void*)                resolve(exe_base, g_identity_matrix);
   g_pauseMode         = (uint8_t*)             resolve(exe_base, gameloop_pause_mode);

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
