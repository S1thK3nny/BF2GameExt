#include "pch.h"
#include "controller_rumble.hpp"
#include "core/resolve.hpp"

#include <detours.h>
#include <cmath>

// TODO: Rumble is a damn piece of work, still doesn't work when taking damage.

bool  g_rumbleEnabled = false;
float g_rumbleScale   = 1.0f;

// ---------------------------------------------------------------------------
// GameLog
// ---------------------------------------------------------------------------

static GameLog_t g_log = nullptr;

// ---------------------------------------------------------------------------
// Weapon / Controllable raw offset constants (replaces game type headers)
// ---------------------------------------------------------------------------

// Weapon instance offsets
static constexpr unsigned kWpn_mClass     = 0x064;  // WeaponClass* (current)
static constexpr unsigned kWpn_mOwner     = 0x06C;  // Controllable*
static constexpr unsigned kWpn_mState     = 0x0B0;  // int WeaponState enum
static constexpr unsigned kWpn_mHeat      = 0x118;  // float (heat accumulator)
static constexpr unsigned kWpn_mOverheat  = 0x11C;  // overheat flag

// Controllable offsets
static constexpr unsigned kCtrl_mPlayerId = 0x0D4;  // int

// Entity layout: Controllable is at +0x240 from entity base.
// Damageable is at entity_base + 0x140.
// mCurHealth = Damageable + 0x04, mMaxHealth = Damageable + 0x08.
static constexpr unsigned kCtrl_to_entity_base = 0x240;
static constexpr unsigned kDamageable_offset   = 0x140;
static constexpr unsigned kDmg_mCurHealth      = 0x04;
static constexpr unsigned kDmg_mMaxHealth      = 0x08;

// WeaponState enum values
static constexpr int WPN_CHARGE = 3;

// ---------------------------------------------------------------------------
// XInput dynamic loading -- xinput1_3.dll (broadest compat), then 1_4, then 9_1_0
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct XINPUT_VIBRATION {
   WORD wLeftMotorSpeed;
   WORD wRightMotorSpeed;
};
#pragma pack(pop)

typedef DWORD(WINAPI* PFN_XInputSetState)(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration);

static HMODULE           s_xinputDll      = nullptr;
static PFN_XInputSetState s_XInputSetState = nullptr;

static bool load_xinput()
{
   const char* dllNames[] = { "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll" };
   for (auto name : dllNames) {
      s_xinputDll = LoadLibraryA(name);
      if (s_xinputDll) {
         s_XInputSetState = (PFN_XInputSetState)GetProcAddress(s_xinputDll, "XInputSetState");
         if (s_XInputSetState) {
            if (g_log) g_log("[Rumble] Loaded %s\n", name);
            return true;
         }
         FreeLibrary(s_xinputDll);
         s_xinputDll = nullptr;
      }
   }
   if (g_log) g_log("[Rumble] No XInput DLL found\n");
   return false;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static inline float read_float(void* base, unsigned offset)
{
   return *(float*)((char*)base + offset);
}

static inline int read_int(void* base, unsigned offset)
{
   return *(int*)((char*)base + offset);
}

static inline void* read_ptr(void* base, unsigned offset)
{
   return *(void**)((char*)base + offset);
}

// High-resolution timer
static LARGE_INTEGER s_perfFreq;

static inline float seconds_since(const LARGE_INTEGER& start)
{
   LARGE_INTEGER now;
   QueryPerformanceCounter(&now);
   if (s_perfFreq.QuadPart == 0) return 0.0f;
   return (float)(now.QuadPart - start.QuadPart) / (float)s_perfFreq.QuadPart;
}

// ---------------------------------------------------------------------------
// WeaponClass rumble ODF field offsets (from PDB + Xbox RE, identical PC/Xbox)
// ---------------------------------------------------------------------------

struct WeaponRumbleOffsets {
   // Recoil (per-shot)
   static constexpr unsigned recoilStrengthHeavy      = 0xB4;
   static constexpr unsigned recoilStrengthLight      = 0xB8;
   static constexpr unsigned recoilLengthLight        = 0xBC;
   static constexpr unsigned recoilLengthHeavy        = 0xC0;
   static constexpr unsigned recoilDelayLight         = 0xC4;
   static constexpr unsigned recoilDelayHeavy         = 0xC8;
   static constexpr unsigned recoilDecayLight         = 0xCC;
   static constexpr unsigned recoilDecayHeavy         = 0xD0;
   // Charge (per-frame)
   static constexpr unsigned chargeRateLight          = 0xD4;
   static constexpr unsigned chargeRateHeavy          = 0xD8;
   static constexpr unsigned maxChargeStrengthHeavy   = 0xDC;
   static constexpr unsigned maxChargeStrengthLight   = 0xE0;
   static constexpr unsigned chargeDelayLight         = 0xE4;
   static constexpr unsigned chargeDelayHeavy         = 0xE8;
   static constexpr unsigned timeAtMaxCharge          = 0xEC;
};

// Xbox heavy motor scaling constant
static constexpr float HEAVY_MOTOR_SCALE = 0.7f;

// ---------------------------------------------------------------------------
// Recoil state -- one-shot pulse, set by rumble_on_signal_fire()
// Matching Xbox Rumble_SetOneShotState: intensity decays by ODF decay rate
// over ODF duration, with per-motor delay before vibration starts.
// ---------------------------------------------------------------------------

static float s_recoilLight          = 0.0f;   // initial intensity (light motor)
static float s_recoilHeavy          = 0.0f;   // initial intensity (heavy motor, already * 0.7)
static float s_recoilDecayRateLight = 0.0f;   // ODF recoilDecayLight
static float s_recoilDecayRateHeavy = 0.0f;   // ODF recoilDecayHeavy
static float s_recoilDurLight       = 0.0f;   // ODF recoilLengthLight
static float s_recoilDurHeavy       = 0.0f;   // ODF recoilLengthHeavy
static float s_recoilDelayLight     = 0.0f;   // ODF recoilDelayLight
static float s_recoilDelayHeavy     = 0.0f;   // ODF recoilDelayHeavy
static LARGE_INTEGER s_recoilStart  = {};
static bool  s_recoilActive         = false;

// ---------------------------------------------------------------------------
// Charge state -- sustained per-frame accumulation during WPN_CHARGE
// Matching Xbox Weapon_UpdateChargeRumble + Rumble_SetSustainedState
// ---------------------------------------------------------------------------

static void*  s_chargingWeapon       = nullptr;
static float  s_chargeLight          = 0.0f;
static float  s_chargeHeavy          = 0.0f;
static float  s_chargeDelayLight     = 0.0f;   // countdown timer (subtract dt)
static float  s_chargeDelayHeavy     = 0.0f;
static float  s_chargeScaleLight     = 1.0f;   // 1.0 normal, -1.0 after timeAtMaxCharge
static float  s_chargeScaleHeavy     = 1.0f;
static float  s_timeAtMaxCountdown   = -1.0f;
static LARGE_INTEGER s_chargeLastSeen = {};     // staleness check (death mid-charge)

// ---------------------------------------------------------------------------
// Vanilla rumble state -- from hooked output stubs / dispatch
// ---------------------------------------------------------------------------

static float s_vanillaLight          = 0.0f;
static float s_vanillaHeavy          = 0.0f;
static LARGE_INTEGER s_vanillaLastUpdate = {};

// ---------------------------------------------------------------------------
// Damage rumble state -- one-shot pulse when local player takes damage
// Matching Xbox FUN_00085e20 -> Rumble_SetSustainedState (flat pulse, no decay)
// ---------------------------------------------------------------------------

static float s_damageLight       = 0.0f;   // light motor intensity (ratio * 2)
static float s_damageHeavy       = 0.0f;   // heavy motor intensity (ratio * 0.7)
static float s_damageDurLight    = 0.0f;   // light motor duration
static float s_damageDurHeavy    = 0.0f;   // heavy motor duration
static LARGE_INTEGER s_damageStart = {};
static bool  s_damageActive      = false;

// Health tracking for per-frame damage detection
static float s_prevPlayerHealth  = -1.0f;
static void* s_lastHealthOwner   = nullptr;

// Previous XInput output (idle optimization)
static WORD s_prevLeft  = 0;
static WORD s_prevRight = 0;

// Staleness safety: timestamp of last output_vibration call
static LARGE_INTEGER s_lastOutputTime = {};

// Game-over rumble decay -- resolved pointer to sGameOver global
static volatile BYTE* s_pGameOver = nullptr;
static bool  s_gameOverDecaying   = false;
static float s_gameOverDecayLeft  = 0.0f;   // left motor level at decay start
static float s_gameOverDecayRight = 0.0f;   // right motor level at decay start
static LARGE_INTEGER s_gameOverDecayStart = {};
static constexpr float GAMEOVER_DECAY_SECS = 0.3f;

// ---------------------------------------------------------------------------
// rumble_on_damage -- trigger damage pulse (Xbox FUN_00085e20 formula)
// ---------------------------------------------------------------------------

static void rumble_on_damage(float damage, float maxHealth)
{
   if (maxHealth <= 0.0f || damage <= 0.0f) return;

   float ratio = damage / maxHealth;
   if (ratio > 2.0f) ratio = 2.0f;

   // Xbox: SetSustainedState(ratio, ratio*0.833, ratio, ratio*2, 0, 0)
   // Param mapping: (lightDur, heavyDur, heavyIntensity*0.7, lightIntensity, lightDelay, heavyDelay)
   s_damageLight    = ratio * 2.0f;                   // light motor intensity
   s_damageHeavy    = ratio * HEAVY_MOTOR_SCALE;      // heavy motor intensity (ratio * 0.7)
   s_damageDurLight = ratio;                           // light motor duration (seconds)
   s_damageDurHeavy = ratio * 0.8333333f;             // heavy motor duration (seconds)

   // Minimum duration so even small hits are felt
   if (s_damageDurLight < 0.08f) s_damageDurLight = 0.08f;
   if (s_damageDurHeavy < 0.06f) s_damageDurHeavy = 0.06f;

   QueryPerformanceCounter(&s_damageStart);
   s_damageActive = true;
}

// ---------------------------------------------------------------------------
// output_vibration -- idempotent, computes from time-based state
// ---------------------------------------------------------------------------

static void output_vibration()
{
   if (!s_XInputSetState) return;


   // --- Recoil: one-shot pulse with ODF delay + decay ---
   float recoilL = 0.0f, recoilH = 0.0f;
   if (s_recoilActive) {
      float elapsed = seconds_since(s_recoilStart);

      // Light motor
      if (elapsed < s_recoilDelayLight) {
         recoilL = 0.0f;  // still in delay period
      } else if (elapsed < s_recoilDelayLight + s_recoilDurLight) {
         float activeTime = elapsed - s_recoilDelayLight;
         recoilL = s_recoilLight - s_recoilDecayRateLight * activeTime;
         if (recoilL < 0.0f) recoilL = 0.0f;
      }

      // Heavy motor
      if (elapsed < s_recoilDelayHeavy) {
         recoilH = 0.0f;
      } else if (elapsed < s_recoilDelayHeavy + s_recoilDurHeavy) {
         float activeTime = elapsed - s_recoilDelayHeavy;
         recoilH = s_recoilHeavy - s_recoilDecayRateHeavy * activeTime;
         if (recoilH < 0.0f) recoilH = 0.0f;
      }

      // Check if both motors expired
      if (elapsed >= s_recoilDelayLight + s_recoilDurLight &&
          elapsed >= s_recoilDelayHeavy + s_recoilDurHeavy) {
         s_recoilActive = false;
      }
   }

   // --- Charge: sustained vibration only while actively charging ---
   // Xbox: SetSustainedState with 0.1s duration expires when UpdateChargeRumble stops refreshing
   // Staleness check: if charging weapon died/was destroyed, clear after 100ms of no updates
   float chargeL = 0.0f, chargeH = 0.0f;
   if (s_chargingWeapon) {
      if (seconds_since(s_chargeLastSeen) > 0.1f) {
         s_chargingWeapon     = nullptr;
         s_chargeLight        = 0.0f;
         s_chargeHeavy        = 0.0f;
         s_chargeDelayLight   = 0.0f;
         s_chargeDelayHeavy   = 0.0f;
         s_chargeScaleLight   = 1.0f;
         s_chargeScaleHeavy   = 1.0f;
         s_timeAtMaxCountdown = -1.0f;
      } else {
         chargeL = s_chargeLight;
         chargeH = s_chargeHeavy * HEAVY_MOTOR_SCALE;
      }
   }

   // --- Vanilla: time-based decay after 100ms of no updates ---
   float vanillaL = s_vanillaLight;
   float vanillaH = s_vanillaHeavy;
   if (vanillaL > 0.0f || vanillaH > 0.0f) {
      float sinceVanilla = seconds_since(s_vanillaLastUpdate);
      if (sinceVanilla > 0.1f) {
         float decay = 1.0f - (sinceVanilla - 0.1f) * 3.0f;
         if (decay <= 0.0f) {
            vanillaL = vanillaH = 0.0f;
            s_vanillaLight = s_vanillaHeavy = 0.0f;
         } else {
            vanillaL *= decay;
            vanillaH *= decay;
         }
      }
   }

   // --- Damage: flat pulse for duration (no decay) ---
   float damageL = 0.0f, damageH = 0.0f;
   if (s_damageActive) {
      float elapsed = seconds_since(s_damageStart);
      if (elapsed < s_damageDurLight) damageL = s_damageLight;
      if (elapsed < s_damageDurHeavy) damageH = s_damageHeavy;
      if (elapsed >= s_damageDurLight && elapsed >= s_damageDurHeavy) {
         s_damageActive = false;
      }
   }

   // --- Combine: weapon (recoil + charge) vs vanilla vs damage, max-wins ---
   float weaponLeft  = clamp01(recoilH + chargeH);
   float weaponRight = clamp01(recoilL + chargeL);
   float left  = clamp01(fmaxf(fmaxf(vanillaH, weaponLeft), damageH) * g_rumbleScale);
   float right = clamp01(fmaxf(fmaxf(vanillaL, weaponRight), damageL) * g_rumbleScale);

   WORD wLeft  = (WORD)(left  * 65535.0f);
   WORD wRight = (WORD)(right * 65535.0f);

   // Idle optimization: skip XInput call if no change
   if (wLeft == 0 && wRight == 0 && s_prevLeft == 0 && s_prevRight == 0)
      return;

   XINPUT_VIBRATION vib;
   vib.wLeftMotorSpeed  = wLeft;
   vib.wRightMotorSpeed = wRight;
   s_XInputSetState(0, &vib);
   s_prevLeft  = wLeft;
   s_prevRight = wRight;
   QueryPerformanceCounter(&s_lastOutputTime);
}

// ---------------------------------------------------------------------------
// Vanilla rumble output hooks (unchanged -- route vanilla intensity to XInput)
// ---------------------------------------------------------------------------

typedef void (__stdcall* fn_rumble_output)(int intensity_bits);

static fn_rumble_output original_light_output = nullptr;
static fn_rumble_output original_heavy_output = nullptr;

static void __stdcall hooked_light_output(int intensity_bits)
{
   s_vanillaLight = *(float*)&intensity_bits;
   QueryPerformanceCounter(&s_vanillaLastUpdate);
   output_vibration();
}

static void __stdcall hooked_heavy_output(int intensity_bits)
{
   s_vanillaHeavy = *(float*)&intensity_bits;
   QueryPerformanceCounter(&s_vanillaLastUpdate);
   output_vibration();
}

// ---------------------------------------------------------------------------
// Vanilla rumble state setup hook (modtools -- populates from regions)
// ---------------------------------------------------------------------------

typedef void (__stdcall* fn_rumble_state_setup)(int playerIdx, float* data);
static fn_rumble_state_setup original_state_setup = nullptr;

static void __stdcall hooked_rumble_state_setup(int playerIdx, float* data)
{
   __try {
      float lightIntensity = data[2];
      float heavyIntensity = data[3];

      if (lightIntensity > s_vanillaLight)
         s_vanillaLight = lightIntensity;
      if (heavyIntensity > s_vanillaHeavy)
         s_vanillaHeavy = heavyIntensity;

      QueryPerformanceCounter(&s_vanillaLastUpdate);
      output_vibration();
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// Vanilla rumble dispatch hook (Steam -- DISABLED)
// ---------------------------------------------------------------------------
// On Steam the state setup stub was stripped, so we'd need to hook the dispatch
// function instead. However, the dispatch uses a non-standard calling convention:
// ECX/EDX as implicit register args + one __cdecl stack param.
// A standard C hook clobbers EDX before we can call through to the original,
// and __try/__except blocks prevent inline asm in the same function.
// This hook only provides region rumble (environmental trigger zones) -- recoil,
// charge, and damage rumble all work through Weapon::Update and SignalFire hooks.

static void* original_dispatch = nullptr;

// ---------------------------------------------------------------------------
// rumble_on_signal_fire -- per-shot recoil pulse (Xbox Weapon_TriggerRecoilRumble)
// ---------------------------------------------------------------------------

void rumble_on_signal_fire(void* weapon)
{
   if (!g_rumbleEnabled || !s_XInputSetState) return;

   __try {
      // Read owner Controllable* from weapon
      void* owner = read_ptr(weapon, kWpn_mOwner);
      if (!owner) return;
      // Only process for local player (playerId == 0)
      if (read_int(owner, kCtrl_mPlayerId) != 0) return;

      void* weaponClass = read_ptr(weapon, kWpn_mClass);
      if (!weaponClass) return;

      using O = WeaponRumbleOffsets;

      float strengthLight = read_float(weaponClass, O::recoilStrengthLight);
      float strengthHeavy = read_float(weaponClass, O::recoilStrengthHeavy);

      // No recoil configured -- skip
      if (strengthLight <= 0.0f && strengthHeavy <= 0.0f) return;

      // Xbox formula: intensity = recoilStrength + chargeAccum
      float intensityLight = strengthLight + s_chargeLight;
      float intensityHeavy = (strengthHeavy + s_chargeHeavy) * HEAVY_MOTOR_SCALE;

      // Set one-shot state (matching Rumble_SetOneShotState)
      s_recoilLight          = intensityLight;
      s_recoilHeavy          = intensityHeavy;
      s_recoilDurLight       = read_float(weaponClass, O::recoilLengthLight);
      s_recoilDurHeavy       = read_float(weaponClass, O::recoilLengthHeavy);
      s_recoilDelayLight     = read_float(weaponClass, O::recoilDelayLight);
      s_recoilDelayHeavy     = read_float(weaponClass, O::recoilDelayHeavy);
      s_recoilDecayRateLight = read_float(weaponClass, O::recoilDecayLight);
      s_recoilDecayRateHeavy = read_float(weaponClass, O::recoilDecayHeavy);

      // Ensure minimum duration so pulses are always felt
      if (s_recoilDurLight <= 0.0f) s_recoilDurLight = 0.1f;
      if (s_recoilDurHeavy <= 0.0f) s_recoilDurHeavy = 0.1f;

      QueryPerformanceCounter(&s_recoilStart);
      s_recoilActive = true;

      // Charge accums persist (Xbox behavior -- not cleared on fire)

      output_vibration();
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// Weapon::SignalFire Detours hook -- triggers recoil rumble per shot
// ---------------------------------------------------------------------------

typedef void (__thiscall* fn_signal_fire)(void*);
static fn_signal_fire original_signal_fire = nullptr;

static void __fastcall hooked_signal_fire(void* weapon, void* /*edx*/)
{
   original_signal_fire(weapon);
   rumble_on_signal_fire(weapon);
}

// ---------------------------------------------------------------------------
// Weapon::Update Detours hook -- charge rumble only (recoil moved to SignalFire)
// ---------------------------------------------------------------------------

typedef bool (__thiscall* fn_weapon_update)(void*, float);
static fn_weapon_update original_weapon_update = nullptr;

static bool __fastcall hooked_weapon_update(void* weapon, void* /*edx*/, float dt)
{
   bool result = original_weapon_update(weapon, dt);

   if (!g_rumbleEnabled || !s_XInputSetState) return result;

   // Game-over decay: smoothly ramp motors to zero over GAMEOVER_DECAY_SECS.
   // Checked before dt guard so it runs even if game pauses time.
   if (s_pGameOver && *s_pGameOver) {
      if (!s_gameOverDecaying) {
         // Capture current motor levels and start decay
         s_gameOverDecaying   = true;
         s_gameOverDecayLeft  = s_prevLeft  / 65535.0f;
         s_gameOverDecayRight = s_prevRight / 65535.0f;
         QueryPerformanceCounter(&s_gameOverDecayStart);
         // Kill all rumble sources so they don't restart
         s_recoilActive   = false;
         s_damageActive   = false;
         s_chargingWeapon = nullptr;
         s_chargeLight = s_chargeHeavy = 0.0f;
         s_vanillaLight = s_vanillaHeavy = 0.0f;
      }
      float t = seconds_since(s_gameOverDecayStart) / GAMEOVER_DECAY_SECS;
      if (t >= 1.0f) {
         // Decay complete -- zero motors
         if (s_prevLeft != 0 || s_prevRight != 0) {
            XINPUT_VIBRATION vib = { 0, 0 };
            s_XInputSetState(0, &vib);
            s_prevLeft = s_prevRight = 0;
         }
      } else {
         float scale = 1.0f - t;
         WORD wL = (WORD)(s_gameOverDecayLeft  * scale * 65535.0f);
         WORD wR = (WORD)(s_gameOverDecayRight * scale * 65535.0f);
         XINPUT_VIBRATION vib = { wL, wR };
         s_XInputSetState(0, &vib);
         s_prevLeft = wL;
         s_prevRight = wR;
      }
      return result;
   }

   // sGameOver cleared (new round) -- reset decay state
   if (s_gameOverDecaying) {
      s_gameOverDecaying = false;
      s_prevPlayerHealth = -1.0f;
      s_lastHealthOwner = nullptr;
   }

   if (dt <= 0.0f || dt > 0.5f) return result;

   // Safety: if motors are stuck on but output_vibration stopped being called
   // (e.g. player died, weapons destroyed), force zero after 300ms.
   if ((s_prevLeft != 0 || s_prevRight != 0) && seconds_since(s_lastOutputTime) > 0.3f) {
      XINPUT_VIBRATION vib = { 0, 0 };
      s_XInputSetState(0, &vib);
      s_prevLeft = s_prevRight = 0;
      s_damageActive = false;
      s_recoilActive = false;
      s_prevPlayerHealth = -1.0f;
      s_lastHealthOwner = nullptr;
   }

   // Only process rumble for local player's weapons
   __try {
      void* owner = read_ptr(weapon, kWpn_mOwner);
      if (!owner) return result;
      if (read_int(owner, kCtrl_mPlayerId) != 0) return result;
   } __except (EXCEPTION_EXECUTE_HANDLER) { return result; }

   __try {
      // --- CHARGE state: accumulate charge rumble (Xbox Weapon_UpdateChargeRumble) ---
      int weaponState = read_int(weapon, kWpn_mState);
      if (weaponState == WPN_CHARGE) {
         bool newChargeCycle = (s_chargingWeapon != weapon);
         s_chargingWeapon = weapon;
         QueryPerformanceCounter(&s_chargeLastSeen);

         void* weaponClass = read_ptr(weapon, kWpn_mClass);
         if (!weaponClass) { output_vibration(); return result; }

         using O = WeaponRumbleOffsets;

         float chargeRateLight  = read_float(weaponClass, O::chargeRateLight);
         float chargeRateHeavy  = read_float(weaponClass, O::chargeRateHeavy);
         float chargeMaxLight   = read_float(weaponClass, O::maxChargeStrengthLight);
         float chargeMaxHeavy   = read_float(weaponClass, O::maxChargeStrengthHeavy);
         float timeAtMaxCharge  = read_float(weaponClass, O::timeAtMaxCharge);

         // Initialize delay countdowns from ODF on first frame of a new charge cycle
         if (newChargeCycle) {
            s_chargeDelayLight   = read_float(weaponClass, O::chargeDelayLight);
            s_chargeDelayHeavy   = read_float(weaponClass, O::chargeDelayHeavy);
            s_chargeScaleLight   = 1.0f;
            s_chargeScaleHeavy   = 1.0f;
            s_timeAtMaxCountdown = -1.0f;
         }

         // Per-motor delay countdowns
         if (s_chargeDelayLight > 0.0f) {
            s_chargeDelayLight -= dt;
         } else {
            if (chargeRateLight > 0.0f) {
               s_chargeLight += chargeRateLight * s_chargeScaleLight * dt;
               if (s_chargeLight > chargeMaxLight && chargeMaxLight > 0.0f)
                  s_chargeLight = chargeMaxLight;
               if (s_chargeLight < 0.0f) s_chargeLight = 0.0f;
            }
         }

         if (s_chargeDelayHeavy > 0.0f) {
            s_chargeDelayHeavy -= dt;
         } else {
            if (chargeRateHeavy > 0.0f) {
               s_chargeHeavy += chargeRateHeavy * s_chargeScaleHeavy * dt;
               if (s_chargeHeavy > chargeMaxHeavy && chargeMaxHeavy > 0.0f)
                  s_chargeHeavy = chargeMaxHeavy;
               if (s_chargeHeavy < 0.0f) s_chargeHeavy = 0.0f;
            }
         }

         // timeAtMaxCharge: when both motors hit max, count down then flip scale to decay
         bool atMax = (chargeMaxLight > 0.0f ? s_chargeLight >= chargeMaxLight : true)
                   && (chargeMaxHeavy > 0.0f ? s_chargeHeavy >= chargeMaxHeavy : true);

         if (atMax && timeAtMaxCharge > 0.0f) {
            if (s_timeAtMaxCountdown < 0.0f)
               s_timeAtMaxCountdown = timeAtMaxCharge;

            s_timeAtMaxCountdown -= dt;
            if (s_timeAtMaxCountdown <= 0.0f) {
               s_chargeScaleLight = -1.0f;
               s_chargeScaleHeavy = -1.0f;
            }
         }

      } else if (weapon == s_chargingWeapon) {
         // Weapon left CHARGE state -- reset charge tracking
         // Charge accums persist (Xbox behavior: consumed when next fire adds them to recoil)
         s_chargingWeapon     = nullptr;
         s_chargeDelayLight   = 0.0f;
         s_chargeDelayHeavy   = 0.0f;
         s_chargeScaleLight   = 1.0f;
         s_chargeScaleHeavy   = 1.0f;
         s_timeAtMaxCountdown = -1.0f;
      }

      // --- Health polling for damage rumble ---
      {
         void* owner = read_ptr(weapon, kWpn_mOwner);
         if (owner) {
            uintptr_t entity_base = (uintptr_t)owner - kCtrl_to_entity_base;
            uintptr_t damageable  = entity_base + kDamageable_offset;

            if (owner != s_lastHealthOwner) {
               s_lastHealthOwner = owner;
               s_prevPlayerHealth = *(float*)(damageable + kDmg_mCurHealth);
            } else {
               float curHealth = *(float*)(damageable + kDmg_mCurHealth);
               float maxHealth = *(float*)(damageable + kDmg_mMaxHealth);

               if (maxHealth > 0.0f && maxHealth < 100000.0f &&
                   s_prevPlayerHealth > 0.0f && curHealth < s_prevPlayerHealth) {
                  float damage = s_prevPlayerHealth - curHealth;
                  rumble_on_damage(damage, maxHealth);
               }
               s_prevPlayerHealth = curHealth;
            }
         }
      }

      // Always evaluate -- recoil decay is time-based and needs per-frame output
      output_vibration();

   } __except (EXCEPTION_EXECUTE_HANDLER) {}

   return result;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static bool s_rumble_initialized = false;

void rumble_init(uintptr_t exe_base)
{
   if (!g_rumbleEnabled) return;
   if (s_rumble_initialized) return;

   g_log = get_gamelog();

   if (!load_xinput()) {
      g_rumbleEnabled = false;
      return;
   }

   QueryPerformanceFrequency(&s_perfFreq);

   using namespace game_addrs::modtools;

   // Install Detours hooks on the vanilla output stubs
   if (rumble_light_output && rumble_heavy_output) {
      original_light_output = (fn_rumble_output)resolve(exe_base, rumble_light_output);
      original_heavy_output = (fn_rumble_output)resolve(exe_base, rumble_heavy_output);

      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourAttach(&(PVOID&)original_light_output, hooked_light_output);
      DetourAttach(&(PVOID&)original_heavy_output, hooked_heavy_output);
      LONG error = DetourTransactionCommit();

      if (error != NO_ERROR) {
         if (g_log) g_log("[Rumble] Detours hook failed: %ld\n", error);
         original_light_output = nullptr;
         original_heavy_output = nullptr;
      } else {
         if (g_log) g_log("[Rumble] Hooked vanilla output stubs\n");
      }
   } else {
      if (g_log) g_log("[Rumble] No vanilla output stub addresses\n");
   }

   // Hook the state setup stub (dispatch calls this to populate rumble from regions)
   if (rumble_state_setup) {
      original_state_setup = (fn_rumble_state_setup)resolve(exe_base, rumble_state_setup);

      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourAttach(&(PVOID&)original_state_setup, hooked_rumble_state_setup);
      LONG error = DetourTransactionCommit();

      if (error != NO_ERROR) {
         if (g_log) g_log("[Rumble] State setup hook failed: %ld\n", error);
         original_state_setup = nullptr;
      } else {
         if (g_log) g_log("[Rumble] Hooked vanilla state setup\n");
      }
   }

   // Hook Weapon::SignalFire for per-shot recoil rumble
   if (weapon_signal_fire) {
      original_signal_fire = (fn_signal_fire)resolve(exe_base, weapon_signal_fire);

      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourAttach(&(PVOID&)original_signal_fire, hooked_signal_fire);
      LONG error = DetourTransactionCommit();

      if (error != NO_ERROR) {
         if (g_log) g_log("[Rumble] SignalFire hook failed: %ld\n", error);
         original_signal_fire = nullptr;
      } else {
         if (g_log) g_log("[Rumble] Hooked Weapon::SignalFire\n");
      }
   }

   // Hook Weapon::Update for charge rumble (recoil is in SignalFire)
   if (weapon_update) {
      original_weapon_update = (fn_weapon_update)resolve(exe_base, weapon_update);

      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourAttach(&(PVOID&)original_weapon_update, hooked_weapon_update);
      LONG error = DetourTransactionCommit();

      if (error != NO_ERROR) {
         if (g_log) g_log("[Rumble] Weapon::Update hook failed: %ld\n", error);
         original_weapon_update = nullptr;
      } else {
         if (g_log) g_log("[Rumble] Hooked Weapon::Update\n");
      }
   }

   // Resolve sGameOver global for game-over motor decay
   if (s_game_over) {
      s_pGameOver = (volatile BYTE*)resolve(exe_base, s_game_over);
   }

   // Reset all state
   s_gameOverDecaying = false;
   s_recoilLight = s_recoilHeavy = 0.0f;
   s_recoilDecayRateLight = s_recoilDecayRateHeavy = 0.0f;
   s_recoilDurLight = s_recoilDurHeavy = 0.0f;
   s_recoilDelayLight = s_recoilDelayHeavy = 0.0f;
   s_recoilStart = {};
   s_recoilActive = false;

   s_chargingWeapon = nullptr;
   s_chargeLight = s_chargeHeavy = 0.0f;
   s_chargeDelayLight = s_chargeDelayHeavy = 0.0f;
   s_chargeScaleLight = s_chargeScaleHeavy = 1.0f;
   s_timeAtMaxCountdown = -1.0f;
   s_chargeLastSeen = {};

   s_vanillaLight = s_vanillaHeavy = 0.0f;
   s_vanillaLastUpdate = {};

   s_damageLight = s_damageHeavy = 0.0f;
   s_damageDurLight = s_damageDurHeavy = 0.0f;
   s_damageStart = {};
   s_damageActive = false;
   s_prevPlayerHealth = -1.0f;
   s_lastHealthOwner = nullptr;
   s_lastOutputTime = {};

   s_prevLeft = s_prevRight = 0;

   s_rumble_initialized = true;
   if (g_log) g_log("[Rumble] Initialized (scale=%.2f)\n", g_rumbleScale);
}

void rumble_shutdown()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_light_output)
      DetourDetach(&(PVOID&)original_light_output, hooked_light_output);
   if (original_heavy_output)
      DetourDetach(&(PVOID&)original_heavy_output, hooked_heavy_output);
   if (original_state_setup)
      DetourDetach(&(PVOID&)original_state_setup, hooked_rumble_state_setup);
   if (original_signal_fire)
      DetourDetach(&(PVOID&)original_signal_fire, hooked_signal_fire);
   if (original_weapon_update)
      DetourDetach(&(PVOID&)original_weapon_update, hooked_weapon_update);
   DetourTransactionCommit();
   original_light_output = nullptr;
   original_heavy_output = nullptr;
   original_signal_fire = nullptr;
   original_state_setup = nullptr;
   original_dispatch = nullptr;
   original_weapon_update = nullptr;

   if (s_XInputSetState) {
      XINPUT_VIBRATION vib = { 0, 0 };
      s_XInputSetState(0, &vib);
   }
   if (s_xinputDll) {
      FreeLibrary(s_xinputDll);
      s_xinputDll = nullptr;
   }
   s_XInputSetState = nullptr;
   g_rumbleEnabled = false;
}
