#include "pch.h"
#include "lightsaber_illumination.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>

// =============================================================================
// Lightsaber illumination
//
// A lightsaber in the stock game emits no light. WeaponMelee::Render walks the
// weapon class's blade table and, for each blade that is ignited and visible,
// calls _RenderLightSabre - which submits two additive PIT_LASER particles to
// the particle renderer and nothing else. The blade is a billboard; the world
// around it is lit exactly as if the saber were switched off.
//
// The engine itself has no trouble with moving lights. LightFlash (the flash an
// explosion makes) is a RedOmniLight + lifetime on a per-frame thread, and
// flaglight.odf is a non-Static EntityLight that rides the CTF flag around on
// whoever is carrying it. Both go through RedLight::Activate, which splices the
// light into the global lists that EntityGeometry::SetupLightingState gathers
// from per drawn object. So all that is missing for sabers is somebody creating
// the light and keeping it on the blade.
//
// WHERE WE HOOK
//   _RenderLightSabre is the ideal site: it is called once per ignited, visible
//   blade per frame, and its arguments are already everything a light needs -
//   `base` at the hilt, `dir` the blade axis, and `length` premultiplied by
//   GetLightSaberLengthFactor, so it is 0 while the blade is retracted and grows
//   as it ignites. What it does NOT carry is the weapon, and therefore the blade
//   colour. So WeaponMelee::Render and WeaponMeleeClass::Render are hooked as
//   well, purely to publish the WeaponMeleeClass for the blade loop that runs
//   inside them; _RenderLightSabre then finds its own blade in that class's
//   table by matching the texture handle it was passed.
//
// COLOUR
//   Blade entry +0x30 is the blade's RedColor, set from LightSaberTrailColor in
//   the ODF (e.g. "7 85 255 128"). The engine's own "is this a saber blade"
//   test is that colour's alpha byte being non-zero - a plain melee weapon such
//   as a vibroblade leaves it 0 and gets no trail. We reuse both facts: alpha
//   gates whether a blade is lit at all, and the RGB is the light colour. That
//   means every stock saber is correctly coloured with no ODF edits.
//
//   RedColor is a D3DCOLOR, so the ODF's "R G B A" lands in memory as B,G,R,A.
//   Reading it as R,G,B,A swaps red and blue, which is invisible on the two
//   colours where they match or are both zero (purple, green) and obvious on
//   the rest - a blue blade lit orange, a red one blue.
//
// LIFETIME
//   Lights live in a small fixed pool of our own static storage, constructed
//   lazily with the engine's ctor so they carry the real RedOmniLight vtable.
//   Ownership stays with us - the engine never frees a light it did not
//   allocate, it only ever unlinks one on Deactivate. Each slot records the
//   frame it was last written; sweep() (driven from Snd::Engine::Update, which
//   runs every frame whether or not anything is rendering) deactivates any slot
//   that a blade did not refresh. That is what turns the light off when a saber
//   is holstered, when its wielder dies, when the weapon is culled, and across
//   a level change.
//
// THE 4-LIGHT CEILING
//   RedLightingState holds pOmniLights[4] and the vs_1_1 constant registers
//   behind it are packed solid (POINT0-3 at c25-c32, bones at c51-c95), so a
//   drawn object can receive four omni lights and no more. Saber lights are by
//   construction the nearest lights to the duel, so they win the per-object
//   sort and can push a room's own lighting out of the set. Keeping the radius
//   small is what keeps that local; hence the modest default, and hence the
//   pool cap - past a handful of blades we are only fighting ourselves.
// =============================================================================

// ---------------------------------------------------------------------------
// Config (read from the INI in dllmain)
// ---------------------------------------------------------------------------

bool  g_lightsaberIlluminationEnabled = true;
float g_lightsaberLightRadius         = 4.0f;
float g_lightsaberLightIntensity      = 1.0f;

// Fraction of the blade length at which the light sits. A saber is a line light
// and this is a point light, so there is no correct answer; sitting a little
// below halfway keeps the hilt end from going dark without throwing the whole
// pool of light off the tip.
static constexpr float kBladeLightFraction = 0.45f;

// More concurrent blades than this and the per-object cap has already made the
// result meaningless. Four heroes with two blades each is the realistic worst
// case in a duel.
static constexpr int kMaxLights = 8;

// ---------------------------------------------------------------------------
// Engine layout
// ---------------------------------------------------------------------------
// All verified in the modtools disassembly of WeaponMelee::Render (0x00636e30)
// and the RedOmniLight ctor (0x00845c70); see game_addrs.hpp for the citations.

static constexpr int kOmniLightSize = 0x120;

// RedLight list membership. Activate (0x0082f7c0) sets flags |= 0x400 and links
// the node at light+0x30 as {+0x0 marker, +0x4 prev, +0x8 next, +0xC owner},
// with owner pointing back at the light itself; Deactivate also unlinks a
// second node at light+0x40 when it is non-null.
static constexpr int      kLight_Flags     = 0x04;
static constexpr unsigned kLightLinkedBit  = 0x400;
static constexpr int      kLight_Node      = 0x30;
static constexpr int      kLight_Node2     = 0x40;
static constexpr int      kNode_Owner      = 0x0C;

// WeaponMelee -> WeaponMeleeClass*. Same slot on every build: modtools
// WeaponMelee::Render reads [ECX+0x68], Steam reads [ESI+0x68] at 0x0068A0B9.
static constexpr int kMelee_mClass = 0x68;

// WeaponMeleeClass -> blade table: a POINTER to the entries, with the entry
// count in the dword immediately before it.
//
// THE OFFSETS ARE NOT THE SAME ACROSS BUILDS and getting this wrong is an
// instant access violation inside find_blade, because the count is then garbage
// and the walk runs off into nothing. modtools puts the pair at 0x3D8/0x3DC,
// the two retail builds at 0x2C4/0x2C8 — visible directly in Steam's
// WeaponMelee::Render as `CMP ECX,[EDI+0x2C4]` next to an `ADD EAX,0x34`, and
// the same split anim_textures.cpp already carries for its own blade lookup.
//
// The entry layout below is genuinely shared: 0x34 bytes on all three, with the
// texture handle at +0x28 on all three.
static int s_class_bladeCount = 0x3D8;
static int s_class_bladeArray = 0x3DC;

static constexpr int kBladeStride  = 0x34;
static constexpr int kBlade_Length = 0x20; // float, the ODF LightSaberLength
static constexpr int kBlade_Tex    = 0x28; // uint texture handle
// RedColor is a D3DCOLOR (0xAARRGGBB), so its bytes in memory are B,G,R,A —
// NOT the R,G,B,A order the ODF line is written in. Indices below are named for
// what the byte actually holds. The engine agrees: the LightFlash ctor
// (0x00603bb0) builds its RedColorValue as {c[2],c[1],c[0],c[3]}/255.
static constexpr int kBlade_Color = 0x30; // RedColor from LightSaberTrailColor
static constexpr int kColor_B = 0;
static constexpr int kColor_G = 1;
static constexpr int kColor_R = 2;
static constexpr int kColor_A = 3;

// ---------------------------------------------------------------------------
// Game function types
// ---------------------------------------------------------------------------

// __cdecl(base, dir, tex, glowTex, length, width, flags)
using fn_render_light_sabre_t = void(__cdecl*)(const float* base, const float* dir,
                                               unsigned tex, unsigned glowTex,
                                               float length, float width, unsigned flags);

// WeaponMelee::Render — __thiscall, 5 stack args.
using fn_melee_render_t = void(__fastcall*)(void* ecx, void* edx, void* mtx, void* pose,
                                            void* color, unsigned flags, int useCollision);

// WeaponMeleeClass::Render — __thiscall, 6 stack args (leading float).
using fn_melee_class_render_t = void(__fastcall*)(void* ecx, void* edx, float t, void* mtx,
                                                  void* pose, void* color, unsigned flags,
                                                  int useCollision);

// Snd::Engine::Update — our per-frame heartbeat. __cdecl(float dt, char full):
// Ghidra labels it Snd::EngineBase::Update and infers __thiscall, but the body
// at 0x008827b0 opens `FLD [ESP+4]` / `MOV AL,[ESP+8]` and never touches ECX,
// so it is a free function and the caller cleans. Same shape the loading screen
// already calls it with.
using fn_snd_update_t = void(__cdecl*)(float dt, char full);

// RedOmniLight / RedLight.
using fn_omni_ctor_t   = void*(__fastcall*)(void* ecx, void* edx, const float* pos,
                                            float radius, const float* rgba);
using fn_omni_set_pos_t    = void(__fastcall*)(void* ecx, void* edx, const float* pos);
using fn_omni_set_radius_t = void(__fastcall*)(void* ecx, void* edx, float radius);
using fn_omni_set_color_t  = void(__fastcall*)(void* ecx, void* edx, const float* rgba);
using fn_light_toggle_t    = void(__fastcall*)(void* ecx, void* edx);

// ---------------------------------------------------------------------------
// Resolved pointers / trampolines
// ---------------------------------------------------------------------------

static fn_render_light_sabre_t original_RenderLightSabre = nullptr;
static fn_melee_render_t       original_MeleeRender      = nullptr;
static fn_melee_class_render_t original_MeleeClassRender = nullptr;
static fn_snd_update_t         original_SndUpdate        = nullptr;

static fn_omni_ctor_t       fn_omni_ctor       = nullptr;
static fn_omni_set_pos_t    fn_omni_set_pos    = nullptr;
static fn_omni_set_radius_t fn_omni_set_radius = nullptr;
static fn_omni_set_color_t  fn_omni_set_color  = nullptr;
static fn_light_toggle_t    fn_light_activate  = nullptr;
static fn_light_toggle_t    fn_light_deactivate = nullptr;

// Per-frame tick.
//
// This used to read RedRenderer's own frame counter, but the value is never
// compared against anything the engine owns — the render hook stamps a slot
// with it and the Snd::Engine::Update hook sweeps against it, and those are
// both ours. Any monotonic per-frame number does the job, so we keep our own
// and drop an engine address we would otherwise have to derive per build.
//
// Starts at 1 so that a zero-initialised LightSlot::frame can never be mistaken
// for "refreshed on tick 0".
static unsigned g_frameTick = 1;

// ---------------------------------------------------------------------------
// Light pool
// ---------------------------------------------------------------------------
// Static storage, not the engine's MemoryPool: these outlive individual sabers
// and we want the addresses stable, so growing a pool every frame is exactly
// what we are avoiding. alignas(16) because the object embeds a D3DXMATRIX.

struct LightSlot {
   alignas(16) unsigned char obj[kOmniLightSize];
   bool     constructed;
   bool     active;
   unsigned frame;     // frame number this slot was last written
};

static LightSlot g_lights[kMaxLights] = {};

// Did the engine pull this light out from under us?
//
// RedLight::InitSys and RedLight::DeinitSys (from FLRenderer::Init / Cleanup)
// force-drain all four global light lists through FUN_0082f770. That drain
// splices every node out and zeroes node+0xC, but it does NOT clear the light's
// own 0x400 "linked" flag, and it leaves prev/next dangling. The engine's own
// lights do not care because they are destroyed at teardown; ours are static
// and outlive it. Trusting our own bookkeeping across that boundary makes
// Deactivate write through the stale prev/next into freed memory and corrupt
// the list - which shows up later as an AV in whatever walks it next.
//
// Activate is the only thing that writes owner = the light itself, and the
// drain is the only thing that clears it, so this is an exact test.
static bool still_linked(const LightSlot& s)
{
   return *(void* const*)(s.obj + kLight_Node + kNode_Owner) == (const void*)s.obj;
}

// Drop membership without touching the links. Only ever called once we have
// established the engine already unlinked us, so the list counters it
// maintains have been reconciled by the drain itself; clearing the second node
// too is safe for the same reason (InitSys/DeinitSys drain all four lists in
// the same call).
static void forget(LightSlot& s)
{
   *(unsigned*)(s.obj + kLight_Flags) &= ~kLightLinkedBit;
   unsigned* n1 = (unsigned*)(s.obj + kLight_Node);
   unsigned* n2 = (unsigned*)(s.obj + kLight_Node2);
   n1[0] = n1[1] = n1[2] = n1[3] = 0;
   n2[0] = n2[1] = n2[2] = n2[3] = 0;
   s.active = false;
}

// Take a light out of the lists, by whichever route is actually safe.
static void release(LightSlot& s)
{
   if (!s.active) return;
   if (still_linked(s)) {
      fn_light_deactivate(s.obj, nullptr);
      s.active = false;
   } else {
      forget(s);
   }
}

// Claim a slot for this frame. A slot already written this frame belongs to
// another blade; anything else is fair game, and reusing a still-active one in
// place saves an unlink/relink pair in the common steady state.
static LightSlot* acquire(unsigned frame)
{
   for (int i = 0; i < kMaxLights; i++)
      if (!g_lights[i].active) return &g_lights[i];
   for (int i = 0; i < kMaxLights; i++)
      if (g_lights[i].frame != frame) return &g_lights[i];
   return nullptr;
}

// Turn off every light no blade refreshed this frame. Order-independent with
// respect to the render pass: if the sound update runs first the slots still
// hold the previous frame's number and get unlinked before being reclaimed, and
// if it runs last they hold the current one and survive untouched.
static void sweep(unsigned frame)
{
   for (int i = 0; i < kMaxLights; i++) {
      LightSlot& s = g_lights[i];
      if (!s.active || s.frame == frame) continue;
      release(s);
   }
}

static void deactivate_all()
{
   for (int i = 0; i < kMaxLights; i++) release(g_lights[i]);
}

// ---------------------------------------------------------------------------
// Current weapon class, published by the Render hooks
// ---------------------------------------------------------------------------
// Single-threaded render, and the blade loop is wholly inside Render, so a bare
// global is enough. Saved/restored rather than cleared because a saber weapon
// can hold a WeaponMeleeThrow whose own render nests inside its owner's.

static const unsigned char* g_curClass = nullptr;

// Find the blade this _RenderLightSabre call is drawing. The texture handle is
// the only per-blade value passed down, and two blades sharing a texture share
// a colour anyway, so matching on it is exact for our purposes.
static const unsigned char* find_blade(const unsigned char* cls, unsigned tex)
{
   if (!cls || !tex) return nullptr;

   const int count = *(const int*)(cls + s_class_bladeCount);
   const unsigned char* blades = *(const unsigned char* const*)(cls + s_class_bladeArray);
   // A wrong per-build offset shows up here as a wild count, so bound it rather
   // than trusting the class blindly — no saber has hundreds of blades.
   if (!blades || count <= 0 || count > 64) return nullptr;

   for (int i = 0; i < count; i++) {
      const unsigned char* b = blades + i * kBladeStride;
      if (*(const unsigned*)(b + kBlade_Tex) == tex) return b;
   }
   return nullptr;
}

// ---------------------------------------------------------------------------
// Hook: _RenderLightSabre
// ---------------------------------------------------------------------------

// The bookkeeping half, split out from the hook itself so the plain __cdecl
// hook (modtools) and the naked LTCG thunk (Steam/GOG) can share one body. It
// touches no argument the two conventions disagree about.
static void __cdecl saber_light_update(const float* base, const float* dir, unsigned tex,
                                       float length, float width)
{
   // Mirror the original's own early-out: no texture or a zero-size blade means
   // nothing is drawn, so there is nothing to light either.
   if (g_lightsaberIlluminationEnabled && base && dir && tex &&
       length > 0.0f && width > 0.0f && g_lightsaberLightRadius > 0.0f) {

      if (const unsigned char* blade = find_blade(g_curClass, tex)) {
         const unsigned char* col = blade + kBlade_Color;

         // Alpha is the engine's own "this is a lightsaber blade" flag (see the
         // trail check in WeaponMelee::RenderLightsaberTrail). A melee weapon
         // with no LightSaberTrailColor leaves it 0 and stays unlit.
         if (col[kColor_A] != 0) {
            // length is already lengthFactor * LightSaberLength, so this ratio
            // is the ignition curve: the light swells with the blade instead of
            // popping on at full strength.
            const float full   = *(const float*)(blade + kBlade_Length);
            float       factor = (full > 0.0f) ? (length / full) : 1.0f;
            if (factor > 1.0f) factor = 1.0f;

            const float radius = g_lightsaberLightRadius * factor;
            if (radius > 0.0f) {
               const unsigned frame = g_frameTick;
               if (LightSlot* s = acquire(frame)) {
                  const float pos[3] = {
                     base[0] + dir[0] * length * kBladeLightFraction,
                     base[1] + dir[1] * length * kBladeLightFraction,
                     base[2] + dir[2] * length * kBladeLightFraction,
                  };
                  // Alpha rides the trail's transparency, which has nothing to
                  // do with how bright the blade glows - drive the light at
                  // full alpha and let the INI scale the RGB.
                  // RedOmniLight::SetColor multiplies the diffuse it hands D3D
                  // by (radius/2)^2, so without this the radius knob would also
                  // be a brightness knob: doubling the reach quadruples the
                  // core and saturates it to white, which makes the light look
                  // like a harder spot rather than a softer one. Dividing that
                  // back out makes radius mean reach and intensity mean
                  // brightness. The scale deliberately uses the configured
                  // radius, not this frame's, so the blade still brightens as
                  // it ignites.
                  //
                  // kRefRadius is a FIXED reference that defines what intensity
                  // 1.0 looks like — it is not the default radius and must not
                  // be moved to track it, or every existing tuning shifts.
                  static constexpr float kRefRadius = 2.5f;
                  const float rScale = (g_lightsaberLightRadius > 0.0f)
                                          ? (kRefRadius / g_lightsaberLightRadius)
                                          : 1.0f;
                  const float k = g_lightsaberLightIntensity * rScale * rScale
                                  * (1.0f / 255.0f);
                  const float rgba[4] = { col[kColor_R] * k, col[kColor_G] * k,
                                          col[kColor_B] * k, 1.0f };

                  if (!s->constructed) {
                     fn_omni_ctor(s->obj, nullptr, pos, radius, rgba);
                     s->constructed = true;
                  } else {
                     fn_omni_set_pos(s->obj, nullptr, pos);
                     // SetColor bakes (radius*0.5)^2 into the diffuse, so the
                     // radius has to land before the colour does.
                     fn_omni_set_radius(s->obj, nullptr, radius);
                     fn_omni_set_color(s->obj, nullptr, rgba);
                  }

                  // If the engine drained the lists since we last touched this
                  // slot, our "active" is a lie and the 0x400 flag would make
                  // Activate a no-op, leaving the light silently dark. Reconcile
                  // first so it relinks cleanly.
                  if (s->active && !still_linked(*s)) forget(*s);

                  if (!s->active) {
                     fn_light_activate(s->obj, nullptr);
                     s->active = true;
                  }
                  s->frame = frame;
               }
            }
         }
      }
   }

}

// modtools: _RenderLightSabre is plain __cdecl, every argument on the stack.
static void __cdecl hooked_RenderLightSabre(const float* base, const float* dir,
                                            unsigned tex, unsigned glowTex,
                                            float length, float width, unsigned flags)
{
   saber_light_update(base, dir, tex, length, width);
   original_RenderLightSabre(base, dir, tex, glowTex, length, width, flags);
}

// ---------------------------------------------------------------------------
// Hooks: the two Render functions, publishing the weapon class
// ---------------------------------------------------------------------------

static void __fastcall hooked_MeleeRender(void* ecx, void* /*edx*/, void* mtx, void* pose,
                                          void* color, unsigned flags, int useCollision)
{
   const unsigned char* prev = g_curClass;
   if (ecx) g_curClass = *(const unsigned char* const*)((const unsigned char*)ecx + kMelee_mClass);

   original_MeleeRender(ecx, nullptr, mtx, pose, color, flags, useCollision);

   g_curClass = prev;
}

static void __fastcall hooked_MeleeClassRender(void* ecx, void* /*edx*/, float t, void* mtx,
                                               void* pose, void* color, unsigned flags,
                                               int useCollision)
{
   const unsigned char* prev = g_curClass;
   g_curClass = (const unsigned char*)ecx;   // ECX is already the class here

   original_MeleeClassRender(ecx, nullptr, t, mtx, pose, color, flags, useCollision);

   g_curClass = prev;
}

// ---------------------------------------------------------------------------
// Steam / GOG: LTCG calling conventions
// ---------------------------------------------------------------------------
// Two of the three targets do not use a convention the compiler can express, so
// they get naked thunks. Both were read off the disassembly, not inferred:
//
//   _RenderLightSabre (steam 0x0068F260)
//     ECX = base, EDX = dir, then 5 stack dwords (tex, glowTex, length, width,
//     flags), ending in a bare RET — register arguments with CALLER cleanup.
//     That is not __fastcall, which would be RET 0x14, so declaring it as one
//     would unbalance the stack.
//
//   WeaponMeleeClass::Render (steam 0x0068E8F0)
//     ECX = this, the float in XMM1, exactly one stack dword, RET 0x4. The
//     remaining arguments are in registers we have not identified, which is
//     fine: we never read them.
//
// WeaponMelee::Render is ordinary __thiscall with RET 0x14 on every build and
// keeps the plain C hook above.

static void* original_RenderLightSabre_raw = nullptr;
static void* original_MeleeClassRender_raw = nullptr;

// Publish the class and fall straight through. A tail JMP leaves the frame
// exactly as the original expects, so its own RET 0x4 returns to our caller and
// we never have to know what is in the other registers.
//
// Nothing is restored afterwards, and that is deliberate: _RenderLightSabre is
// only ever reached from inside one of the two Render functions, so a stale
// g_curClass is always overwritten on the way in before anything can read it.
static __declspec(naked) void hooked_MeleeClassRender_ltcg()
{
   __asm {
      mov  g_curClass, ecx
      jmp  dword ptr [original_MeleeClassRender_raw]
   }
}

// Here we do have to call into C, so every register has to be saved and put
// back before falling through to the original.
//
// ALL EIGHT XMM registers are saved, not just the ones the callee obviously
// uses. On 32-bit MSVC every XMM register is caller-saved, so the C helper is
// free to clobber all of them — but the code around us is LTCG, which means the
// compiler knew exactly which registers the real _RenderLightSabre touches and
// was free to keep live floats in the rest across the call. Saving only xmm0-3
// leaves those live values to be destroyed, which corrupts whatever the caller
// computes next (blade width, for one).
//
// Stack map, with E = ESP on entry:
//   [E+0x00] return address
//   [E+0x04] tex   [E+0x08] glowTex   [E+0x0C] length
//   [E+0x10] width [E+0x14] flags
// After PUSHAD (-0x20) and the XMM save area (-0x80) those sit at +0xA4 onward.
static __declspec(naked) void hooked_RenderLightSabre_ltcg()
{
   __asm {
      pushad
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7

      // saber_light_update(base, dir, tex, length, width) — __cdecl, so the
      // arguments go on in reverse. Each PUSH shifts ESP, hence the offsets
      // below are not all the same.
      push   dword ptr [esp + 0xB0]   // width
      push   dword ptr [esp + 0xB0]   // length
      push   dword ptr [esp + 0xAC]   // tex
      push   edx                      // dir   (untouched since entry)
      push   ecx                      // base  (untouched since entry)
      call   saber_light_update
      add    esp, 20

      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      add    esp, 0x80
      popad

      // ESP is back to E, so the original sees its own arguments and its bare
      // RET returns straight to our caller.
      jmp    dword ptr [original_RenderLightSabre_raw]
   }
}

// ---------------------------------------------------------------------------
// Hook: Snd::Engine::Update — per-frame heartbeat for the sweep
// ---------------------------------------------------------------------------
// Chosen because it runs every frame regardless of what is on screen, which is
// exactly the case a render-driven sweep cannot cover: the frame a saber's
// wielder dies, no Render call happens and the light would otherwise stay lit
// at its last position forever. It is also pumped by hand during loading, where
// the sweep is a harmless no-op because no light is ever active.

static bool g_loggedStartup = false;

static void __cdecl hooked_SndUpdate(float dt, char full)
{
   // Deferred from install: dllmain installs with the exe sections mapped RW and
   // non-executable, so calling into the engine's logger there is an access
   // violation on a DEP-enabled build. By the time a frame ticks, .text is back.
   if (!g_loggedStartup) {
      g_loggedStartup = true;
      get_gamelog()("[LightsaberIllumination] enabled (radius %.2f, intensity %.2f)\n",
                    g_lightsaberLightRadius, g_lightsaberLightIntensity);
   }

   // Sweep on the tick that is closing, THEN advance. Doing it the other way
   // round would retire everything drawn during the interval that just ended,
   // because those slots carry the old tick.
   sweep(g_frameTick);
   ++g_frameTick;

   original_SndUpdate(dt, full);
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void lightsaber_illumination_install(uintptr_t exe_base)
{
   if (!g_lightsaberIlluminationEnabled) return;

   // A build we have no address set for stays off rather than half-installing.
   if (g_addr->render_light_sabre == 0 || g_addr->weapon_melee_render == 0 ||
       g_addr->weapon_melee_class_render == 0 || g_addr->snd_engine_update == 0 ||
       g_addr->red_omni_light_ctor == 0 || g_addr->red_omni_light_set_position == 0 ||
       g_addr->red_omni_light_set_radius == 0 || g_addr->red_omni_light_set_color == 0 ||
       g_addr->red_light_activate == 0 || g_addr->red_light_deactivate == 0) {
      return; // nothing to log to yet — see the deferred line in hooked_SndUpdate
   }

   fn_omni_ctor        = (fn_omni_ctor_t)      resolve(exe_base, g_addr->red_omni_light_ctor);
   fn_omni_set_pos     = (fn_omni_set_pos_t)   resolve(exe_base, g_addr->red_omni_light_set_position);
   fn_omni_set_radius  = (fn_omni_set_radius_t)resolve(exe_base, g_addr->red_omni_light_set_radius);
   fn_omni_set_color   = (fn_omni_set_color_t) resolve(exe_base, g_addr->red_omni_light_set_color);
   fn_light_activate   = (fn_light_toggle_t)   resolve(exe_base, g_addr->red_light_activate);
   fn_light_deactivate = (fn_light_toggle_t)   resolve(exe_base, g_addr->red_light_deactivate);

   original_MeleeRender = (fn_melee_render_t)resolve(exe_base, g_addr->weapon_melee_render);
   original_SndUpdate   = (fn_snd_update_t)  resolve(exe_base, g_addr->snd_engine_update);

   // modtools compiles these two the way the compiler would; the retail builds
   // do not, and get naked thunks instead. See the LTCG section above.
   const bool ltcg = (g_build != GameBuild::Modtools);

   // Blade table moved between modtools and retail; see the constants above.
   if (ltcg) {
      s_class_bladeCount = 0x2C4;
      s_class_bladeArray = 0x2C8;
   }

   if (ltcg) {
      original_RenderLightSabre_raw = (void*)resolve(exe_base, g_addr->render_light_sabre);
      original_MeleeClassRender_raw = (void*)resolve(exe_base, g_addr->weapon_melee_class_render);
   } else {
      original_RenderLightSabre = (fn_render_light_sabre_t)resolve(exe_base, g_addr->render_light_sabre);
      original_MeleeClassRender = (fn_melee_class_render_t)resolve(exe_base, g_addr->weapon_melee_class_render);
   }

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (ltcg) {
      DetourAttach(&original_RenderLightSabre_raw, hooked_RenderLightSabre_ltcg);
      DetourAttach(&original_MeleeClassRender_raw, hooked_MeleeClassRender_ltcg);
   } else {
      DetourAttach(&(PVOID&)original_RenderLightSabre, hooked_RenderLightSabre);
      DetourAttach(&(PVOID&)original_MeleeClassRender, hooked_MeleeClassRender);
   }
   DetourAttach(&(PVOID&)original_MeleeRender, hooked_MeleeRender);
   DetourAttach(&(PVOID&)original_SndUpdate,   hooked_SndUpdate);
   DetourTransactionCommit();
}

void lightsaber_illumination_uninstall()
{
   // Unlink before the hooks go, or a light stays spliced into a global list
   // that nothing will ever walk us out of.
   if (fn_light_deactivate) deactivate_all();

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_RenderLightSabre) DetourDetach(&(PVOID&)original_RenderLightSabre, hooked_RenderLightSabre);
   if (original_MeleeClassRender) DetourDetach(&(PVOID&)original_MeleeClassRender, hooked_MeleeClassRender);
   if (original_RenderLightSabre_raw) DetourDetach(&original_RenderLightSabre_raw, hooked_RenderLightSabre_ltcg);
   if (original_MeleeClassRender_raw) DetourDetach(&original_MeleeClassRender_raw, hooked_MeleeClassRender_ltcg);
   if (original_MeleeRender)      DetourDetach(&(PVOID&)original_MeleeRender,      hooked_MeleeRender);
   if (original_SndUpdate)        DetourDetach(&(PVOID&)original_SndUpdate,        hooked_SndUpdate);
   DetourTransactionCommit();
}
