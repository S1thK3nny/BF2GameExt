#include "pch.h"
#include "lua_hooks.hpp"
#include "lua_funcs.hpp"
#include "game_events.hpp"

#include <detours.h>

lua_api g_lua = {};
lua_State* g_L = nullptr;
bool g_useBarrelFireOrigin = false;
char g_loadDisplayPath[260] = "Load\\load";
ExeType    g_exeType = ExeType::UNKNOWN;
game_addrs g_game = {};

// Debug log level: 0=off, 1=normal (hooks/errors/patches), 2=verbose (all diagnostics)
int g_debugLogLevel = 1;

// Absolute log path — computed once from the exe's directory so CWD changes don't break logging
static char g_logPath[MAX_PATH] = {};

static void init_log_path()
{
   char exePath[MAX_PATH];
   GetModuleFileNameA(nullptr, exePath, MAX_PATH);
   // Strip exe filename, keep directory
   char* lastSlash = strrchr(exePath, '\\');
   if (lastSlash) *(lastSlash + 1) = '\0';
   snprintf(g_logPath, MAX_PATH, "%sBF2GameExt.log", exePath);
}

static void dbg_log_impl(const char* fmt, va_list args)
{
   if (!g_logPath[0]) return;
   FILE* f = nullptr;
   fopen_s(&f, g_logPath, "a");
   if (!f) return;
   vfprintf(f, fmt, args);
   fclose(f);
}

// Level 1+ logging — hook install, errors, patches
void dbg_log(const char* fmt, ...)
{
   if (g_debugLogLevel < 1) return;
   va_list args;
   va_start(args, fmt);
   dbg_log_impl(fmt, args);
   va_end(args);
}

// Level 2 logging — verbose diagnostics (dispatch, state changes, per-frame info)
void dbg_log_verbose(const char* fmt, ...)
{
   if (g_debugLogLevel < 2) return;
   va_list args;
   va_start(args, fmt);
   dbg_log_impl(fmt, args);
   va_end(args);
}

// Landing region fire patch — byte pointer into EntityFlyer::Update
unsigned char* g_flyerLandingFirePatch = nullptr;
unsigned char  g_flyerLandingFireOrig  = 0;

// ---------------------------------------------------------------------------
// Barrel fire origin — WeaponCannon OverrideAimer vtable hook
// ---------------------------------------------------------------------------

// Vtable slot + original/hook pointers — accessible from lua_funcs.cpp for live toggling
void** g_cannonOverrideAimerSlot = nullptr;
void*  g_cannonOverrideAimerOrig = nullptr;
void*  g_cannonOverrideAimerHook = nullptr;

// Weapon::ZoomFirstPerson — resolved at install time
typedef bool(__thiscall* ZoomFirstPerson_t)(void* weapon);
static ZoomFirstPerson_t fn_ZoomFirstPerson = nullptr;

// Replacement for WeaponCannon::OverrideAimer (vtable slot 0x70).
// When enabled, reads the barrel fire point matrix translation from the Weapon
// and writes it to the Aimer's mFirePos. Falls back to vanilla aimer position
// when the matrix is stale (first-person zoom) or reflected (water).
static bool __fastcall hooked_cannon_OverrideAimer(void* weapon, void* /*edx*/)
{
   if (!g_useBarrelFireOrigin) return false;

   // Zoom detection: revert to vanilla aimer when in first-person zoom.
   // Two cases to handle:
   //   1. Weapon has ZoomFirstPerson — zooming transitions into first-person
   //   2. Player is already in first-person and zooms any weapon
   void* owner = *(void**)((char*)weapon + 0x6C);
   if (owner) {
      bool isZoomed = *(bool*)((char*)owner + g_game.controllable_mIsAiming_offset);
      if (isZoomed) {
         // Case 1: weapon class forces first-person on zoom
         if (fn_ZoomFirstPerson && fn_ZoomFirstPerson(weapon))
            return false;
         // Case 2: already in first-person view
         void* tracker = *(void**)((char*)owner + 0x34);
         if (tracker && *(bool*)((char*)tracker + 0x14))
            return false;
      }
   }

   __try {
      void* aimer = *(void**)((char*)weapon + 0x70);   // Weapon::mAimer
      if (!aimer) return false;

      // Weapon::mFirePointMatrix at weapon+0x20 (PblMatrix, 0x40 bytes).
      // PblMatrix::trans row is at offset 0x30 — the world-space fire position.
      float* trans = (float*)((char*)weapon + 0x20 + 0x30);

      // Validate: check for uninitialized (0xCDCDCDCD) or zero
      const uint32_t raw = *(uint32_t*)&trans[0];
      if (raw == 0xCDCDCDCD ||
          (trans[0] == 0.0f && trans[1] == 0.0f && trans[2] == 0.0f))
         return false;

      float* aimerFirePos = (float*)((char*)aimer + 0x88);  // Aimer::mFirePos
      float* rootPos      = (float*)((char*)aimer + 0x70);  // Aimer::mRootPos

      // Water reflection check: the rendering reflection pass can mirror
      // mFirePointMatrix Y across the water plane. Normal barrel-to-root
      // Y delta is ~3 units; reflected can be 18+. If reflected, clamp Y
      // to a reasonable offset from rootPos rather than skipping entirely.
      float fireY = trans[1];
      float barrelRootDy = fireY - rootPos[1];
      if (barrelRootDy < -5.0f || barrelRootDy > 5.0f) {
         fireY = rootPos[1] - 2.7f;
      }

      // Write to both mFirePos and mFirePointMatrix trans.
      // Muzzle flash reads mFirePos; projectile system may read the matrix.
      aimerFirePos[0] = trans[0];
      aimerFirePos[1] = fireY;
      aimerFirePos[2] = trans[2];

      trans[0] = aimerFirePos[0];
      trans[1] = fireY;
      trans[2] = aimerFirePos[2];
      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// ---------------------------------------------------------------------------
// OnCharacterFireWeapon — Weapon::SignalFire Detours hook
// ---------------------------------------------------------------------------

// Weapon::OverrideSoldierVelocity — __thiscall, 4 float* stack params, RET 0x10
using fn_override_soldier_velocity = bool(__thiscall*)(void*, float*, float*, float*, float*);
static fn_override_soldier_velocity original_override_soldier_velocity = nullptr;

// Player tracking (only the human player aims/zooms)
static void* s_playerOwner = nullptr;
static int   s_localPlayerCharIdx = -1;


// ---------------------------------------------------------------------------
// Resolve character index from a Controllable pointer using the same
// pointer-arithmetic method the vanilla Lua callbacks use:
//   charIndex = (Controllable.mCharacter - Character::sCharacters) / sizeof(Character)
// Returns -1 if not found.
// ---------------------------------------------------------------------------
static int resolve_char_index_from_controllable(void* controllable)
{
   if (!controllable) return -1;
   if (!g_game.char_array_base_ptr || !g_game.char_array_max_count) return -1;
   if (!g_game.controllable_mCharacter_offset) return -1;

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - 0x400000u + base; };

   const uintptr_t arrayBase = *(uintptr_t*)res(g_game.char_array_base_ptr);
   if (!arrayBase) return -1;
   const int maxChars = *(int*)res(g_game.char_array_max_count);

   // Read Controllable.mCharacter — back-pointer to the Character array slot
   uintptr_t chrPtr = *(uintptr_t*)((char*)controllable + g_game.controllable_mCharacter_offset);
   if (!chrPtr || chrPtr < arrayBase) return -1;

   int idx = (int)((chrPtr - arrayBase) / 0x1B0);
   if (idx < 0 || idx >= maxChars) return -1;
   return idx;
}

// ---------------------------------------------------------------------------
// Per-character speed factor system
//
// Two override types per character:
//   aimOnly=false  "general" — always applies (e.g. AI fire slow, debuffs)
//   aimOnly=true   "aim"     — only applies while the character is aiming
// Both can coexist; effective factor = min(aim, general).
// ---------------------------------------------------------------------------

struct CharacterSpeedOverride {
   int    charIndex;       // -1 = empty slot
   float  targetFactor;    // desired factor (0–1, from Lua)
   float  currentFactor;   // lerped value for smooth transitions
   DWORD  expireTick;      // GetTickCount() expiry; 0 = permanent
   float  lerpSpeed;       // per-frame lerp rate
   bool   aimOnly;         // true = only apply while aiming
};

static constexpr float DEFAULT_LERP_SPEED = 0.02f;
static constexpr int MAX_CHAR_OVERRIDES = 64;
static CharacterSpeedOverride s_charOverrides[MAX_CHAR_OVERRIDES];

// ---------------------------------------------------------------------------
// Fire-speed overrides — slow characters while firing (driven by Lua per-shot callbacks)
// ---------------------------------------------------------------------------

struct FireSpeedOverride {
   int    charIndex;       // -1 = empty slot
   float  targetFactor;    // desired factor (0–1)
   float  currentFactor;   // lerped value for smooth transitions
   float  lerpSpeed;       // per-frame lerp rate
   float  cooldownSec;     // seconds to stay slowed after fire release
   float  chance;          // probability (0–1) that a given burst applies
   DWORD  lastFireTick;    // GetTickCount() of last frame where firing was true
   bool   wasFiring;       // previous frame fire state (for rising-edge detection)
   bool   chanceActive;    // true = this burst passed the chance roll
};

static constexpr int MAX_FIRE_OVERRIDES = 64;
static FireSpeedOverride s_fireOverrides[MAX_FIRE_OVERRIDES];

void clear_all_fire_speed_factors()
{
   for (int i = 0; i < MAX_FIRE_OVERRIDES; ++i)
      s_fireOverrides[i].charIndex = -1;
}

void clear_fire_speed_factor(int charIndex)
{
   for (int i = 0; i < MAX_FIRE_OVERRIDES; ++i) {
      if (s_fireOverrides[i].charIndex == charIndex)
         s_fireOverrides[i].charIndex = -1;
   }
}

float get_entity_movement_speed(void* entity)
{
   if (!entity) return 0.0f;

   __try {
      // EntityControllable: Controllable subobject at +0x240, velocity offset is build-specific
      float* vel = (float*)((char*)entity + 0x240 + g_game.controllable_to_velocity_offset);
      return sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return 0.0f;
   }
}

void clear_all_character_speed_factors()
{
   for (int i = 0; i < MAX_CHAR_OVERRIDES; ++i)
      s_charOverrides[i].charIndex = -1;
}

// Find an existing entry matching charIndex + aimOnly, or allocate a new slot.
static CharacterSpeedOverride* find_or_alloc(int charIndex, bool aimOnly)
{
   // Exact match first
   for (int i = 0; i < MAX_CHAR_OVERRIDES; ++i) {
      if (s_charOverrides[i].charIndex == charIndex && s_charOverrides[i].aimOnly == aimOnly)
         return &s_charOverrides[i];
   }

   // Find empty or oldest expired slot
   int best = -1;
   DWORD oldest = MAXDWORD;
   DWORD now = GetTickCount();
   for (int i = 0; i < MAX_CHAR_OVERRIDES; ++i) {
      if (s_charOverrides[i].charIndex < 0) { best = i; break; }
      if (s_charOverrides[i].expireTick &&
          (int)(now - s_charOverrides[i].expireTick) >= 0) { best = i; break; }
      if (s_charOverrides[i].expireTick && s_charOverrides[i].expireTick < oldest) {
         oldest = s_charOverrides[i].expireTick; best = i;
      }
   }
   if (best < 0) best = 0;

   s_charOverrides[best].charIndex = -1;  // mark as fresh
   return &s_charOverrides[best];
}

void set_character_speed_factor(int charIndex, float factor, float durationSec, float lerpSpeed)
{
   if (factor < 0.0f) factor = 0.0f;
   if (factor > 1.0f) factor = 1.0f;
   if (lerpSpeed <= 0.0f) lerpSpeed = DEFAULT_LERP_SPEED;

   DWORD expire = 0;
   if (durationSec > 0.0f)
      expire = GetTickCount() + (DWORD)(durationSec * 1000.0f);

   CharacterSpeedOverride* ovr = find_or_alloc(charIndex, false);
   bool isNew = (ovr->charIndex != charIndex);
   ovr->charIndex     = charIndex;
   ovr->targetFactor  = factor;
   ovr->expireTick    = expire;
   ovr->lerpSpeed     = lerpSpeed;
   ovr->aimOnly       = false;
   if (isNew) ovr->currentFactor = 1.0f;
}

void set_character_aim_speed_factor(int charIndex, float factor, float lerpSpeed)
{
   if (factor < 0.0f) factor = 0.0f;
   if (factor > 1.0f) factor = 1.0f;
   if (lerpSpeed <= 0.0f) lerpSpeed = DEFAULT_LERP_SPEED;

   CharacterSpeedOverride* ovr = find_or_alloc(charIndex, true);
   bool isNew = (ovr->charIndex != charIndex);
   ovr->charIndex     = charIndex;
   ovr->targetFactor  = factor;
   ovr->expireTick    = 0;  // aim overrides are always permanent
   ovr->lerpSpeed     = lerpSpeed;
   ovr->aimOnly       = true;
   if (isNew) ovr->currentFactor = 1.0f;
}

void set_character_fire_speed_factor(int charIndex, float factor, float cooldownSec, float chance, float lerpSpeed)
{
   if (factor < 0.0f) factor = 0.0f;
   if (factor > 1.0f) factor = 1.0f;
   if (chance < 0.0f) chance = 0.0f;
   if (chance > 1.0f) chance = 1.0f;
   if (lerpSpeed <= 0.0f) lerpSpeed = DEFAULT_LERP_SPEED;
   if (cooldownSec < 0.0f) cooldownSec = 0.0f;

   // Find existing entry for this charIndex, or allocate a free slot
   FireSpeedOverride* slot = nullptr;
   for (int i = 0; i < MAX_FIRE_OVERRIDES; ++i) {
      if (s_fireOverrides[i].charIndex == charIndex) { slot = &s_fireOverrides[i]; break; }
   }
   if (!slot) {
      for (int i = 0; i < MAX_FIRE_OVERRIDES; ++i) {
         if (s_fireOverrides[i].charIndex < 0) { slot = &s_fireOverrides[i]; break; }
      }
   }
   if (!slot) slot = &s_fireOverrides[0];  // fallback: overwrite first slot

   bool isNew = (slot->charIndex != charIndex);
   slot->charIndex     = charIndex;
   slot->targetFactor  = factor;
   slot->lerpSpeed     = lerpSpeed;
   slot->cooldownSec   = cooldownSec;
   slot->chance        = chance;
   slot->lastFireTick  = GetTickCount();  // refresh on every bullet (called from Lua per-shot)
   if (isNew) {
      slot->currentFactor = 1.0f;
      slot->wasFiring     = false;
      slot->chanceActive  = false;
   }
}

void clear_character_speed_factor(int charIndex)
{
   for (int i = 0; i < MAX_CHAR_OVERRIDES; ++i) {
      if (s_charOverrides[i].charIndex == charIndex)
         s_charOverrides[i].charIndex = -1;
   }
   clear_fire_speed_factor(charIndex);
}

// Apply per-character velocity cap. Called from the velocity hook for each owner.
static void apply_character_speed(void* owner, int charIdx, bool isPlayer, bool isAiming)
{
   int soldierState = *(int*)((char*)owner + g_game.controllable_to_soldierState_offset);
   bool stateAllowed = (soldierState == 0 || soldierState == 1);

   float effectiveFactor = 1.0f;

   for (int i = 0; i < MAX_CHAR_OVERRIDES; ++i) {
      CharacterSpeedOverride* ovr = &s_charOverrides[i];
      if (ovr->charIndex != charIdx) continue;

      // Handle expiry for timed entries
      bool expired = false;
      if (ovr->expireTick) {
         DWORD now = GetTickCount();
         if ((int)(now - ovr->expireTick) >= 0)
            expired = true;
      }

      if (expired) {
         // Fade out toward 1.0, then clear
         ovr->currentFactor += (1.0f - ovr->currentFactor) * 0.05f;
         if (ovr->currentFactor > 0.995f) {
            ovr->charIndex = -1;
            continue;
         }
      } else {
         // Determine if this entry is active right now
         bool active = true;
         if (ovr->aimOnly)
            active = isAiming;

         // Snap to 1.0 for non-allowed states (roll, jump, sprint, etc.)
         if (!stateAllowed && ovr->currentFactor < 1.0f) {
            ovr->currentFactor = 1.0f;
            continue;
         }

         float target = (active && stateAllowed && ovr->targetFactor < 1.0f)
                        ? ovr->targetFactor : 1.0f;

         // Linear lerp
         float diff = target - ovr->currentFactor;
         if (fabsf(diff) <= ovr->lerpSpeed)
            ovr->currentFactor = target;
         else
            ovr->currentFactor += (diff > 0.0f) ? ovr->lerpSpeed : -ovr->lerpSpeed;
      }

      if (ovr->currentFactor < effectiveFactor)
         effectiveFactor = ovr->currentFactor;
   }

   // --- Fire-speed overrides ---
   for (int i = 0; i < MAX_FIRE_OVERRIDES; ++i) {
      FireSpeedOverride* fovr = &s_fireOverrides[i];
      if (fovr->charIndex != charIdx) continue;

      // Detect firing via lastFireTick (updated per-bullet by SetCharacterFireSpeedFactor
      // called from Lua OnCharacterFireWeapon). Active window = 500ms between shots.
      DWORD now = GetTickCount();
      bool isFiring = false;
      if (fovr->lastFireTick != 0) {
         float elapsed = (float)(now - fovr->lastFireTick) / 1000.0f;
         isFiring = (elapsed < 0.5f);
      }

      // Rising edge: new burst started — roll the chance.
      // Skip re-roll if still within cooldown from a previous burst (prevents
      // rapid walk/run oscillation on fast burst-fire weapons).
      bool prevCooldownActive = false;
      if (!fovr->wasFiring && fovr->lastFireTick != 0) {
         float elapsed = (float)(now - fovr->lastFireTick) / 1000.0f;
         prevCooldownActive = (elapsed < fovr->cooldownSec);
      }

      if (isFiring && !fovr->wasFiring && !prevCooldownActive) {
         if (fovr->chance >= 1.0f) {
            fovr->chanceActive = true;
         } else {
            // Mix charIndex into randomness so all chars don't roll identically
            unsigned int seed = now ^ (unsigned int)(charIdx * 2654435761u);
            float roll = (float)(seed % 10000) / 10000.0f;
            fovr->chanceActive = (roll < fovr->chance);
         }
      }
      fovr->wasFiring = isFiring;

      // Determine if fire slow should be active
      bool inCooldown = false;
      if (!isFiring && fovr->lastFireTick != 0) {
         float elapsed = (float)(now - fovr->lastFireTick) / 1000.0f;
         inCooldown = (elapsed < fovr->cooldownSec);
      }

      bool fireActive = (isFiring || inCooldown) && fovr->chanceActive;

      // Reset chanceActive once cooldown expires (so next burst gets a fresh roll)
      if (!isFiring && !inCooldown)
         fovr->chanceActive = false;

      // Snap to 1.0 for non-allowed states (roll, jump, sprint, etc.)
      if (!stateAllowed && fovr->currentFactor < 1.0f) {
         fovr->currentFactor = 1.0f;
         continue;
      }

      float target = (fireActive && stateAllowed) ? fovr->targetFactor : 1.0f;

      // Linear lerp
      float diff = target - fovr->currentFactor;
      if (fabsf(diff) <= fovr->lerpSpeed)
         fovr->currentFactor = target;
      else
         fovr->currentFactor += (diff > 0.0f) ? fovr->lerpSpeed : -fovr->lerpSpeed;

      if (fovr->currentFactor < effectiveFactor)
         effectiveFactor = fovr->currentFactor;
   }

   // Cap velocity to maxSpeed * effectiveFactor (only clamps — no jitter)
   if (effectiveFactor < 1.0f) {
      void* soldierClass = *(void**)((char*)owner + g_game.controllable_to_soldierClass_offset);
      if (soldierClass) {
         float maxSpeed  = *(float*)((char*)soldierClass + g_game.soldierClass_maxSpeed_offset);
         float maxStrafe = *(float*)((char*)soldierClass + g_game.soldierClass_maxStrafe_offset);
         float capSpeed  = maxSpeed  * effectiveFactor;
         float capStrafe = maxStrafe * effectiveFactor;
         float cap = (capSpeed > capStrafe) ? capSpeed : capStrafe;

         float* vel = (float*)((char*)owner + g_game.controllable_to_velocity_offset);
         float hSpeedSq = vel[0] * vel[0] + vel[2] * vel[2];
         if (hSpeedSq > cap * cap) {
            float scale = cap / sqrtf(hSpeedSq);
            vel[0] *= scale;
            vel[2] *= scale;
         }
      }
   }

}

static bool __fastcall hooked_override_soldier_velocity(void* thisPtr, void* /*edx*/,
                                                         float* p1, float* p2, float* p3, float* p4)
{
   bool result = original_override_soldier_velocity(thisPtr, p1, p2, p3, p4);

   void* owner = *(void**)((char*)thisPtr + 0x6C);
   if (!owner) return result;

   int charIdx = resolve_char_index_from_controllable(owner);
   if (charIdx < 0) return result;

   bool isAiming = *(bool*)((char*)owner + g_game.controllable_mIsAiming_offset);
   bool isPlayer = false;

   // Track the player (only humans aim/zoom)
   if (isAiming) {
      s_playerOwner = owner;
      s_localPlayerCharIdx = charIdx;
      isPlayer = true;
   } else if (owner == s_playerOwner || charIdx == s_localPlayerCharIdx) {
      isPlayer = true;
   }

   apply_character_speed(owner, charIdx, isPlayer, isAiming);
   return result;
}

using fn_signal_fire = void(__thiscall*)(void*);
static fn_signal_fire original_signal_fire = nullptr;

static void __fastcall hooked_signal_fire(void* weapon, void* /*edx*/)
{
   static bool s_loggedOnce = false;
   if (!s_loggedOnce) { dbg_log("[signal_fire_hook] FIRED weapon=%p\n", weapon); s_loggedOnce = true; }

   // Debounce: SignalFire can be called multiple times per trigger pull (salvo).
   // Read mLastFireTime before the original call. If unchanged after, skip.
   float prevFireTime = 0.0f;
   __try { prevFireTime = *(float*)((char*)weapon + g_game.weapon_mLastFireTime_offset); }
   __except (EXCEPTION_EXECUTE_HANDLER) {}

   original_signal_fire(weapon);

   if (!g_L) return;

   __try {
      float newFireTime = *(float*)((char*)weapon + g_game.weapon_mLastFireTime_offset);
      if (prevFireTime == newFireTime) return;
   } __except (EXCEPTION_EXECUTE_HANDLER) {}

   __try {
      // Weapon ODF name: weapon+mClass -> WeaponClass*, +0x30 -> char[]
      void* weaponClass = *(void**)((char*)weapon + g_game.weapon_mClass_offset);
      if (!weaponClass) return;
      const char* odfName = (const char*)((char*)weaponClass + 0x30);
      if (!odfName || !odfName[0]) return;

      // Owner Controllable
      void* owner = *(void**)((char*)weapon + 0x6C);
      if (!owner) return;

      // Vehicle hop: follow mPilot if owner is a vehicle
      void* shooter = owner;
      int pilotType = *(int*)((char*)owner + g_game.controllable_mPilotType_offset);
      void* pilot   = *(void**)((char*)owner + g_game.controllable_mPilot_offset);
      if (pilotType != 1 && !(pilotType == 4 && pilot == owner)) {
         if (pilot) shooter = pilot;
      }

      // Resolve charIndex — silently skip if no character (e.g. autonomous turrets)
      int charIndex = resolve_char_index_from_controllable(shooter);
      if (charIndex < 0) {
         dbg_log_verbose("[signal_fire] no charIndex for shooter=%p odf=%s\n", shooter, odfName);
         return;
      }

      dbg_log_verbose("[signal_fire] charIndex=%d odf=%s\n", charIndex, odfName);

      // Push event args and dispatch through the generic event system
      g_lua.pushnumber(g_L, (float)charIndex);
      g_lua.pushstring(g_L, odfName);
      g_evtCharacterFireWeapon.dispatch(charIndex, 2);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// OnCharacterExitVehicle — Detours hook on the exit-vehicle function
// ---------------------------------------------------------------------------

// EntitySoldier::ExitVehicle — __thiscall, this=EntitySoldier*, 2 bool stack params, RET 0x8
using fn_char_exit_vehicle = bool(__thiscall*)(void*, bool, bool);
static fn_char_exit_vehicle original_char_exit_vehicle = nullptr;

static bool __fastcall hooked_char_exit_vehicle(void* thisPtr, void* /*edx*/, bool param_1, bool param_2)
{
   // thisPtr = EntitySoldier* (NOT Controllable*).
   // Disasm shows both modtools and steam read this+0xCC to get the Character
   // array slot pointer. This is a field in the GameObject region — the same
   // offset in both builds (unlike Controllable.mCharacter which shifts).
   // Cannot use resolve_char_index_from_controllable here.
   int charIndex = -1;
   void* vehicleEntity = nullptr;

   __try {
      if (g_game.char_array_base_ptr && g_game.char_array_max_count) {
         const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
         auto res = [=](uintptr_t a) -> uintptr_t { return a - 0x400000u + base; };
         const uintptr_t arrayBase = *(uintptr_t*)res(g_game.char_array_base_ptr);
         const int maxChars = *(int*)res(g_game.char_array_max_count);

         if (arrayBase) {
            // Read Character slot pointer from EntitySoldier+0xCC (same offset both builds)
            uintptr_t chrPtr = *(uintptr_t*)((char*)thisPtr + 0xCC);
            if (chrPtr >= arrayBase) {
               int idx = (int)((chrPtr - arrayBase) / 0x1B0);
               if (idx >= 0 && idx < maxChars) {
                  charIndex = idx;
                  // Character.mVehicle (+0x14C) is a Controllable*, but Lua
                  // expects an EntityEx*/GameObject* (what GetEntityClass uses).
                  // Use the virtual Trackable::GetGameObject() call — the same
                  // method vanilla GetCharacterVehicle uses. This works for all
                  // Controllable types (vehicles, turrets, etc.) because each
                  // class implements GetGameObject() in its own vtable.
                  // Trackable subobject is at Controllable+0x18, and
                  // GetGameObject is vtable slot 8 (offset 0x20).
                  void* vehCtrl = *(void**)((char*)chrPtr + 0x14C);
                  if (vehCtrl) {
                     uintptr_t trackable = (uintptr_t)vehCtrl + 0x18;
                     uintptr_t vtable = *(uintptr_t*)trackable;
                     typedef void* (__thiscall* fn_get_game_object)(void*);
                     auto getGameObj = (fn_get_game_object)(*(uintptr_t*)(vtable + 0x20));
                     vehicleEntity = getGameObj((void*)trackable);
                  }
               }
            }
         }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {}

   // Dispatch BEFORE the exit call. The vehicle entity is still alive here,
   // so Lua scripts can safely pass the lightuserdata to game functions.
   // After ExitVehicle returns, the vehicle entity may be destroyed.
   if (g_L && charIndex >= 0) {
      dbg_log_verbose("[exit_vehicle] charIndex=%d vehicle=%p\n", charIndex, vehicleEntity);

      __try {
         g_lua.pushnumber(g_L, (float)charIndex);
         g_lua.pushlightuserdata(g_L, vehicleEntity);
         g_evtCharacterExitVehicle.dispatch(charIndex, 2);
      } __except (EXCEPTION_EXECUTE_HANDLER) {}
   }

   return original_char_exit_vehicle(thisPtr, param_1, param_2);
}

static const uintptr_t unrelocated_base = 0x400000;

static void* resolve(uintptr_t exe_base, uintptr_t unrelocated_addr)
{
   if (!unrelocated_addr) return nullptr;
   return (void*)((unrelocated_addr - unrelocated_base) + exe_base);
}

// ---------------------------------------------------------------------------
// LoadDisplay path + DLC flag patch state
// ---------------------------------------------------------------------------
static uint32_t* g_loadDisplay_push_op_ptr  = nullptr;
static uint32_t  g_loadDisplay_push_op_orig = 0;
uint8_t*  g_loadDisplay_dlc_flag_ptr = nullptr;
uint8_t*  g_loadRandom_dlc_flag_ptr = nullptr;

static void write_code_byte(uint8_t* ptr, uint8_t val)
{
   DWORD oldProt;
   if (VirtualProtect(ptr, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
      *ptr = val;
      VirtualProtect(ptr, 1, oldProt, &oldProt);
   }
}

// ---------------------------------------------------------------------------
// ingame.lvl stricmp shim — ends-with match + one-shot flag
// ---------------------------------------------------------------------------
// Replaces the CALL to __stricmp in Lua_Callbacks::ReadDataFile.
// Returns 0 (match) on the FIRST call where arg1 ends with "ingame.lvl",
// then returns non-zero for all subsequent matches until reset.
// Same calling convention as __stricmp: __cdecl, 2 args on stack, result in EAX.

bool g_ingameInitArmed = false;
static uint8_t g_ingameStricmpOrigBytes[6] = {};
static uint8_t* g_ingameStricmpCallSite = nullptr;

static int __cdecl ingame_stricmp_shim(const char* str1, const char* str2)
{
   // str2 is always "ingame.lvl" (pushed from .rdata by the game)
   // str1 is the path from Lua arg 1 (e.g. "..\..\addon\BF3\data\_LVL_PC\ingame.lvl")
   if (!str1 || !str2) return 1;

   size_t len1 = strlen(str1);
   size_t len2 = strlen(str2);
   if (len1 < len2) return 1;

   // Check if str1 ends with str2 (case-insensitive)
   if (_stricmp(str1 + len1 - len2, str2) != 0) return 1;

   // Only fire when armed by Lua via ArmIngameInit()
   if (!g_ingameInitArmed) return 1;
   g_ingameInitArmed = false;
   dbg_log("[ingame_stricmp] armed match \"%s\" — firing init\n", str1);
   return 0;
}

using fn_init_state = void(__cdecl*)();
static fn_init_state original_init_state = nullptr;

static void __cdecl hooked_init_state()
{
   original_init_state();

   // Reset load display overrides to vanilla every map change
   strncpy_s(g_loadDisplayPath, sizeof(g_loadDisplayPath), "Load\\load", _TRUNCATE);
   if (g_loadDisplay_dlc_flag_ptr) write_code_byte(g_loadDisplay_dlc_flag_ptr, 0x00);
   if (g_loadRandom_dlc_flag_ptr)  write_code_byte(g_loadRandom_dlc_flag_ptr, 0x00);

   // Reset ingame.lvl init one-shot so it fires again on next map
   g_ingameInitArmed = false;

   // Reset per-character speed overrides on map change
   clear_all_character_speed_factors();
   clear_all_fire_speed_factors();
   s_playerOwner = nullptr;
   s_localPlayerCharIdx = -1;

   // Read gLuaState_Pointer now that InitState has returned
   lua_State** p_lua_state = (lua_State**)((g_game.g_lua_state_ptr - 0x400000) +
                                           (uintptr_t)GetModuleHandleW(nullptr));
   g_L = *p_lua_state;

   dbg_log("[lua_hooks] hooked_init_state fired. g_L=%p\n", (void*)g_L);
   dbg_log_verbose("[lua_hooks] init_state: reset load display, speed factors, ingame arm\n");

   if (g_L)
      register_lua_functions(g_L);
}

// ---------------------------------------------------------------------------
// CloseState hook — null out g_L when the Lua state is destroyed
// ---------------------------------------------------------------------------

using fn_close_state = void(__cdecl*)();
static fn_close_state original_close_state = nullptr;

static void __cdecl hooked_close_state()
{
   dbg_log("[lua_hooks] hooked_close_state fired. g_L was %p\n", (void*)g_L);
   g_L = nullptr;
   original_close_state();
}


void lua_register_func(lua_State* L, const char* name, lua_CFunction fn)
{
   if (not g_lua.pushlstring or not g_lua.pushcclosure or not g_lua.settable) return;

   // Lua 5.0: push name + closure, then settable into LUA_GLOBALSINDEX
   g_lua.pushlstring(L, name, strlen(name));
   g_lua.pushcclosure(L, fn, 0);
   g_lua.settable(L, LUA_GLOBALSINDEX);
}

// ---------------------------------------------------------------------------
// Exe detection — identify modtools / steam / GOG from PE section layout
// ---------------------------------------------------------------------------

static ExeType detect_exe(uintptr_t exe_base)
{
   struct ExeId { ExeType type; uintptr_t file_offset; };
   static const ExeId ids[] = {
      { ExeType::MODTOOLS, 0x62b59c },
      { ExeType::GOG,      0x39f298 },
      { ExeType::STEAM,    0x39e234 },
   };
   constexpr uint64_t expected_magic = 0x746163696c707041;

   auto* dos = (IMAGE_DOS_HEADER*)exe_base;
   auto* nt  = (IMAGE_NT_HEADERS32*)(exe_base + dos->e_lfanew);
   auto* sec = IMAGE_FIRST_SECTION(nt);
   int nsec  = nt->FileHeader.NumberOfSections;

   for (const auto& id : ids) {
      for (int i = 0; i < nsec; ++i) {
         if (id.file_offset >= sec[i].PointerToRawData &&
             id.file_offset < sec[i].PointerToRawData + sec[i].SizeOfRawData) {
            uintptr_t mem = exe_base + sec[i].VirtualAddress +
                            (id.file_offset - sec[i].PointerToRawData);
            if (*(uint64_t*)mem == expected_magic)
               return id.type;
            break;
         }
      }
   }
   return ExeType::UNKNOWN;
}

// ---------------------------------------------------------------------------
// Fill game-specific addresses from the detected namespace
// ---------------------------------------------------------------------------

#define FILL_GAME_ADDRS(NS) do {                                              \
   g_game.init_state                         = NS::init_state;                \
   g_game.close_state                        = NS::close_state;               \
   g_game.g_lua_state_ptr                    = NS::g_lua_state_ptr;           \
   g_game.weapon_signal_fire                 = NS::weapon_signal_fire;        \
   g_game.weapon_cannon_vftable_override_aimer = NS::weapon_cannon_vftable_override_aimer; \
   g_game.weapon_override_aimer_impl         = NS::weapon_override_aimer_impl;\
   g_game.weapon_override_aimer_thunk        = NS::weapon_override_aimer_thunk;\
   g_game.weapon_zoom_first_person           = NS::weapon_zoom_first_person;  \
   g_game.flyer_landing_fire_jnp             = NS::flyer_landing_fire_jnp;   \
   g_game.char_array_base_ptr                = NS::char_array_base_ptr;       \
   g_game.char_array_max_count               = NS::char_array_max_count;      \
   g_game.pbl_hash_ctor                      = NS::pbl_hash_ctor;            \
   g_game.hash_to_name                       = NS::hash_to_name;             \
   g_game.entity_class_registry              = NS::entity_class_registry;    \
   g_game.team_array_ptr                     = NS::team_array_ptr;           \
   g_game.game_log                           = NS::game_log;                 \
   g_game.weapon_mLastFireTime_offset        = NS::weapon_mLastFireTime_offset; \
   g_game.weapon_mClass_offset               = NS::weapon_mClass_offset;        \
   g_game.controllable_mCharacter_offset     = NS::controllable_mCharacter_offset;  \
   g_game.controllable_mPilot_offset         = NS::controllable_mPilot_offset;  \
   g_game.controllable_mPilotType_offset     = NS::controllable_mPilotType_offset; \
   g_game.controllable_mPlayerId_offset      = NS::controllable_mPlayerId_offset;  \
   g_game.controllable_mIsAiming_offset      = NS::controllable_mIsAiming_offset;  \
   g_game.controllable_to_soldierState_offset = NS::controllable_to_soldierState_offset; \
   g_game.controllable_to_soldierClass_offset = NS::controllable_to_soldierClass_offset; \
   g_game.controllable_to_velocity_offset    = NS::controllable_to_velocity_offset;  \
   g_game.soldierClass_maxSpeed_offset       = NS::soldierClass_maxSpeed_offset;     \
   g_game.soldierClass_maxStrafe_offset      = NS::soldierClass_maxStrafe_offset;    \
   g_game.weapon_override_soldier_velocity   = NS::weapon_override_soldier_velocity; \
   g_game.char_exit_vehicle                  = NS::char_exit_vehicle;               \
   g_game.load_display_path_push_op          = NS::load_display_path_push_op;       \
   g_game.load_display_dlc_flag_byte         = NS::load_display_dlc_flag_byte;      \
   g_game.load_random_dlc_flag_byte          = NS::load_random_dlc_flag_byte;       \
   g_game.ingame_stricmp_call                = NS::ingame_stricmp_call;             \
   g_game.ingame_stricmp_call_size           = NS::ingame_stricmp_call_size;        \
   g_game.net_in_shell                       = NS::net_in_shell;                    \
   g_game.net_enabled                        = NS::net_enabled;                     \
   g_game.net_enabled_next                   = NS::net_enabled_next;                \
   g_game.net_on_client                      = NS::net_on_client;                   \
} while(0)

// ---------------------------------------------------------------------------
// Resolve all Lua API function pointers from the active namespace
// ---------------------------------------------------------------------------

#define RESOLVE_LUA_API(NS) do {                                              \
   g_lua.open              = (fn_lua_open)             resolve(exe_base, NS::lua_open);         \
   g_lua.newthread         = (fn_lua_newthread)        resolve(exe_base, NS::lua_newthread);    \
   g_lua.gettop            = (fn_lua_gettop)           resolve(exe_base, NS::lua_gettop);       \
   g_lua.settop            = (fn_lua_settop)           resolve(exe_base, NS::lua_settop);       \
   g_lua.pushvalue         = (fn_lua_pushvalue)        resolve(exe_base, NS::lua_pushvalue);    \
   g_lua.remove            = (fn_lua_remove)           resolve(exe_base, NS::lua_remove);       \
   g_lua.insert            = (fn_lua_insert)           resolve(exe_base, NS::lua_insert);       \
   g_lua.replace           = (fn_lua_replace)          resolve(exe_base, NS::lua_replace);      \
   g_lua.checkstack        = (fn_lua_checkstack)       resolve(exe_base, NS::lua_checkstack);   \
   g_lua.xmove             = (fn_lua_xmove)            resolve(exe_base, NS::lua_xmove);       \
   g_lua.type              = (fn_lua_type)             resolve(exe_base, NS::lua_type);         \
   g_lua.type_name         = (fn_lua_typename)         resolve(exe_base, NS::lua_typename);     \
   g_lua.isnumber          = (fn_lua_isnumber)         resolve(exe_base, NS::lua_isnumber);     \
   g_lua.isstring          = (fn_lua_isstring)         resolve(exe_base, NS::lua_isstring);     \
   g_lua.iscfunction       = (fn_lua_iscfunction)      resolve(exe_base, NS::lua_iscfunction);  \
   g_lua.isuserdata        = (fn_lua_isuserdata)       resolve(exe_base, NS::lua_isuserdata);   \
   g_lua.equal             = (fn_lua_equal)            resolve(exe_base, NS::lua_equal);        \
   g_lua.rawequal          = (fn_lua_rawequal)         resolve(exe_base, NS::lua_rawequal);     \
   g_lua.lessthan          = (fn_lua_lessthan)         resolve(exe_base, NS::lua_lessthan);     \
   g_lua.tonumber          = (fn_lua_tonumber)         resolve(exe_base, NS::lua_tonumber);     \
   g_lua.toboolean         = (fn_lua_toboolean)        resolve(exe_base, NS::lua_toboolean);    \
   g_lua.tostring          = (fn_lua_tostring)         resolve(exe_base, NS::lua_tostring);     \
   g_lua.tolstring         = (fn_luaL_checklstring)    resolve(exe_base, NS::luaL_checklstring);\
   g_lua.str_len           = (fn_lua_strlen)           resolve(exe_base, NS::lua_strlen);       \
   g_lua.touserdata        = (fn_lua_touserdata)       resolve(exe_base, NS::lua_touserdata);   \
   g_lua.tothread          = (fn_lua_tothread)         resolve(exe_base, NS::lua_tothread);     \
   g_lua.topointer         = (fn_lua_topointer)        resolve(exe_base, NS::lua_topointer);    \
   g_lua.pushnil           = (fn_lua_pushnil)          resolve(exe_base, NS::lua_pushnil);      \
   g_lua.pushnumber        = (fn_lua_pushnumber)       resolve(exe_base, NS::lua_pushnumber);   \
   g_lua.pushlstring       = (fn_lua_pushlstring)      resolve(exe_base, NS::lua_pushlstring);  \
   g_lua.pushstring        = (fn_lua_pushstring)       resolve(exe_base, NS::lua_pushstring);   \
   g_lua.pushvfstring      = (fn_lua_pushvfstring)     resolve(exe_base, NS::lua_pushvfstring); \
   g_lua.pushfstring       = (fn_lua_pushfstring)      resolve(exe_base, NS::lua_pushfstring);  \
   g_lua.pushcclosure      = (fn_lua_pushcclosure)     resolve(exe_base, NS::lua_pushcclosure); \
   g_lua.pushboolean       = (fn_lua_pushboolean)      resolve(exe_base, NS::lua_pushboolean);  \
   g_lua.pushlightuserdata = (fn_lua_pushlightuserdata)resolve(exe_base, NS::lua_pushlightuserdata); \
   g_lua.gettable          = (fn_lua_gettable)         resolve(exe_base, NS::lua_gettable);     \
   g_lua.rawget            = (fn_lua_rawget)           resolve(exe_base, NS::lua_rawget);       \
   g_lua.rawgeti           = (fn_lua_rawgeti)          resolve(exe_base, NS::lua_rawgeti);      \
   g_lua.newtable          = (fn_lua_newtable)         resolve(exe_base, NS::lua_newtable);     \
   g_lua.getmetatable      = (fn_lua_getmetatable)     resolve(exe_base, NS::lua_getmetatable); \
   g_lua.getfenv           = (fn_lua_getfenv)          resolve(exe_base, NS::lua_getfenv);      \
   g_lua.settable          = (fn_lua_settable)         resolve(exe_base, NS::lua_settable);     \
   g_lua.rawset            = (fn_lua_rawset)           resolve(exe_base, NS::lua_rawset);       \
   g_lua.rawseti           = (fn_lua_rawseti)          resolve(exe_base, NS::lua_rawseti);      \
   g_lua.setmetatable      = (fn_lua_setmetatable)     resolve(exe_base, NS::lua_setmetatable); \
   g_lua.setfenv           = (fn_lua_setfenv)          resolve(exe_base, NS::lua_setfenv);      \
   g_lua.call              = (fn_lua_call)             resolve(exe_base, NS::lua_call);         \
   g_lua.pcall             = (fn_lua_pcall)            resolve(exe_base, NS::lua_pcall);        \
   g_lua.load              = (fn_lua_load)             resolve(exe_base, NS::lua_load);         \
   g_lua.dump              = (fn_lua_dump)             resolve(exe_base, NS::lua_dump);         \
   g_lua.error             = (fn_lua_error)            resolve(exe_base, NS::lua_error);        \
   g_lua.next              = (fn_lua_next)             resolve(exe_base, NS::lua_next);         \
   g_lua.concat            = (fn_lua_concat)           resolve(exe_base, NS::lua_concat);       \
   g_lua.newuserdata       = (fn_lua_newuserdata)      resolve(exe_base, NS::lua_newuserdata);  \
   g_lua.getgcthreshold    = (fn_lua_getgcthreshold)   resolve(exe_base, NS::lua_getgcthreshold); \
   g_lua.getgccount        = (fn_lua_getgccount)       resolve(exe_base, NS::lua_getgccount);  \
   g_lua.setgcthreshold    = (fn_lua_setgcthreshold)   resolve(exe_base, NS::lua_setgcthreshold); \
   g_lua.getupvalue        = (fn_lua_getupvalue)       resolve(exe_base, NS::lua_getupvalue);   \
   g_lua.setupvalue        = (fn_lua_setupvalue)       resolve(exe_base, NS::lua_setupvalue);   \
   g_lua.getinfo           = (fn_lua_getinfo)          resolve(exe_base, NS::lua_getinfo);      \
   g_lua.sethook           = (fn_lua_sethook)          resolve(exe_base, NS::lua_sethook);      \
   g_lua.gethook           = (fn_lua_gethook)          resolve(exe_base, NS::lua_gethook);      \
   g_lua.gethookmask       = (fn_lua_gethookmask)      resolve(exe_base, NS::lua_gethookmask);  \
   g_lua.gethookcount      = (fn_lua_gethookcount)     resolve(exe_base, NS::lua_gethookcount); \
   g_lua.getstack          = (fn_lua_getstack)         resolve(exe_base, NS::lua_getstack);     \
   g_lua.getlocal          = (fn_lua_getlocal)         resolve(exe_base, NS::lua_getlocal);     \
   g_lua.setlocal          = (fn_lua_setlocal)         resolve(exe_base, NS::lua_setlocal);     \
   g_lua.dobuffer          = (fn_lua_dobuffer)         resolve(exe_base, NS::lua_dobuffer);     \
   g_lua.dostring          = (fn_lua_dostring)         resolve(exe_base, NS::lua_dostring);     \
   g_lua.L_openlib         = (fn_luaL_openlib)         resolve(exe_base, NS::luaL_openlib);     \
   g_lua.L_callmeta        = (fn_luaL_callmeta)        resolve(exe_base, NS::luaL_callmeta);    \
   g_lua.L_typerror        = (fn_luaL_typerror)        resolve(exe_base, NS::luaL_typerror);    \
   g_lua.L_argerror        = (fn_luaL_argerror)        resolve(exe_base, NS::luaL_argerror);    \
   g_lua.L_checklstring    = (fn_luaL_checklstring)    resolve(exe_base, NS::luaL_checklstring);\
   g_lua.L_optlstring      = (fn_luaL_optlstring)      resolve(exe_base, NS::luaL_optlstring);  \
   g_lua.L_checknumber     = (fn_luaL_checknumber)     resolve(exe_base, NS::luaL_checknumber); \
   g_lua.L_optnumber       = (fn_luaL_optnumber)       resolve(exe_base, NS::luaL_optnumber);   \
   g_lua.L_checktype       = (fn_luaL_checktype)       resolve(exe_base, NS::luaL_checktype);   \
   g_lua.L_checkany        = (fn_luaL_checkany)        resolve(exe_base, NS::luaL_checkany);    \
   g_lua.L_newmetatable    = (fn_luaL_newmetatable)    resolve(exe_base, NS::luaL_newmetatable); \
   g_lua.L_getmetatable    = (fn_luaL_getmetatable)    resolve(exe_base, NS::luaL_getmetatable); \
   g_lua.L_where           = (fn_luaL_where)           resolve(exe_base, NS::luaL_where);       \
   g_lua.L_error           = (fn_luaL_error)           resolve(exe_base, NS::luaL_error);       \
   g_lua.L_ref             = (fn_luaL_ref)             resolve(exe_base, NS::luaL_ref);         \
   g_lua.L_unref           = (fn_luaL_unref)           resolve(exe_base, NS::luaL_unref);       \
   g_lua.L_getn            = (fn_luaL_getn)            resolve(exe_base, NS::luaL_getn);        \
   g_lua.L_setn            = (fn_luaL_setn)            resolve(exe_base, NS::luaL_setn);        \
   g_lua.L_loadfile        = (fn_luaL_loadfile)        resolve(exe_base, NS::luaL_loadfile);    \
   g_lua.L_loadbuffer      = (fn_luaL_loadbuffer)      resolve(exe_base, NS::luaL_loadbuffer);  \
   g_lua.L_checkstack      = (fn_luaL_checkstack)      resolve(exe_base, NS::luaL_checkstack);  \
   g_lua.L_getmetafield    = (fn_luaL_getmetafield)    resolve(exe_base, NS::luaL_getmetafield);\
   g_lua.L_buffinit        = (fn_luaL_buffinit)        resolve(exe_base, NS::luaL_buffinit);    \
   g_lua.L_prepbuffer      = (fn_luaL_prepbuffer)      resolve(exe_base, NS::luaL_prepbuffer);  \
   g_lua.L_addlstring      = (fn_luaL_addlstring)      resolve(exe_base, NS::luaL_addlstring);  \
   g_lua.L_addvalue        = (fn_luaL_addvalue)        resolve(exe_base, NS::luaL_addvalue);    \
   g_lua.L_pushresult      = (fn_luaL_pushresult)      resolve(exe_base, NS::luaL_pushresult);  \
   g_lua.open_base         = (fn_luaopen)              resolve(exe_base, NS::luaopen_base);     \
   g_lua.open_table        = (fn_luaopen)              resolve(exe_base, NS::luaopen_table);    \
   g_lua.open_io           = (fn_luaopen)              resolve(exe_base, NS::luaopen_io);       \
   g_lua.open_string       = (fn_luaopen)              resolve(exe_base, NS::luaopen_string);   \
   g_lua.open_math         = (fn_luaopen)              resolve(exe_base, NS::luaopen_math);     \
   g_lua.open_debug        = (fn_luaopen)              resolve(exe_base, NS::luaopen_debug);    \
} while(0)

void lua_hooks_install(uintptr_t exe_base)
{
   init_log_path();
   clear_all_character_speed_factors();
   clear_all_fire_speed_factors();

   // --- Detect which exe we're running in ---
   g_exeType = detect_exe(exe_base);

   {
      const char* exeNames[] = { "UNKNOWN", "MODTOOLS", "STEAM", "GOG" };
      dbg_log("[lua_hooks] detect_exe: exe_base=0x%08X type=%s\n",
              (unsigned)exe_base, exeNames[(int)g_exeType]);
   }

   if (g_exeType == ExeType::UNKNOWN) {
      FatalAppExitA(0, "BF2GameExt: could not identify executable!");
      return;
   }

   // --- Fill game-specific addresses from the correct namespace ---
   switch (g_exeType) {
      case ExeType::MODTOOLS: FILL_GAME_ADDRS(lua_addrs::modtools); break;
      case ExeType::STEAM:    FILL_GAME_ADDRS(lua_addrs::steam);    break;
      case ExeType::GOG:      FILL_GAME_ADDRS(lua_addrs::gog);      break;
      default: break;
   }

   // --- Resolve Lua API function pointers ---
   switch (g_exeType) {
      case ExeType::MODTOOLS: RESOLVE_LUA_API(lua_addrs::modtools); break;
      case ExeType::STEAM:    RESOLVE_LUA_API(lua_addrs::steam);    break;
      case ExeType::GOG:      RESOLVE_LUA_API(lua_addrs::gog);      break;
      default: break;
   }

   dbg_log_verbose("[lua_hooks] g_game: init_state=0x%08X g_lua_state_ptr=0x%08X\n",
                   (unsigned)g_game.init_state, (unsigned)g_game.g_lua_state_ptr);
   dbg_log_verbose("[lua_hooks] g_lua.pushlstring=%p pushcclosure=%p settable=%p\n",
                   (void*)g_lua.pushlstring, (void*)g_lua.pushcclosure, (void*)g_lua.settable);

   // --- Hook init_state and SignalFire via Detours ---
   // On modtools: DLL loads as PE import, sections are writable here.
   // On steam/GOG: DLL loads via dinput8.dll proxy after DRM unpacking,
   //   sections are writable here (install_patches makes them writable).
   original_init_state  = (fn_init_state) resolve(exe_base, g_game.init_state);
   original_close_state = (fn_close_state)resolve(exe_base, g_game.close_state);
   original_signal_fire = (fn_signal_fire)resolve(exe_base, g_game.weapon_signal_fire);
   original_override_soldier_velocity = (fn_override_soldier_velocity)resolve(exe_base, g_game.weapon_override_soldier_velocity);
   original_char_exit_vehicle = (fn_char_exit_vehicle)resolve(exe_base, g_game.char_exit_vehicle);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_init_state, hooked_init_state);
   DetourAttach(&(PVOID&)original_close_state, hooked_close_state);
   DetourAttach(&(PVOID&)original_signal_fire, hooked_signal_fire);
   if (original_override_soldier_velocity)
      DetourAttach(&(PVOID&)original_override_soldier_velocity, hooked_override_soldier_velocity);
   if (original_char_exit_vehicle)
      DetourAttach(&(PVOID&)original_char_exit_vehicle, hooked_char_exit_vehicle);
   LONG detourResult = DetourTransactionCommit();
   dbg_log_verbose("[lua_hooks] Detours commit result=%ld\n", detourResult);

   // Patch LoadDisplay path operand — redirect to our mutable buffer.
   // Sections are still writable at this point (dllmain made them PAGE_READWRITE).
   g_loadDisplay_push_op_ptr = (uint32_t*)resolve(exe_base, g_game.load_display_path_push_op);
   if (g_loadDisplay_push_op_ptr) {
      g_loadDisplay_push_op_orig = *g_loadDisplay_push_op_ptr;
      *g_loadDisplay_push_op_ptr = (uint32_t)(uintptr_t)g_loadDisplayPath;
      dbg_log_verbose("[LoadDisplay] patched path operand 0x%08x -> 0x%08x (\"%s\")\n",
                      g_loadDisplay_push_op_orig, *g_loadDisplay_push_op_ptr, g_loadDisplayPath);
   }

   // Resolve DLC flag bytes for runtime toggling via SetLoadDisplayFromDLC
   g_loadDisplay_dlc_flag_ptr = (uint8_t*)resolve(exe_base, g_game.load_display_dlc_flag_byte);
   g_loadRandom_dlc_flag_ptr  = (uint8_t*)resolve(exe_base, g_game.load_random_dlc_flag_byte);

   // ingame.lvl stricmp shim — DISABLED until remaster script dependency is resolved.
   // The patch works, but community mods rely on remaster scripts that expect UI
   // elements not present in the vanilla ingame.lvl (e.g. SideIcon*.skin).
#if 0
   // Patch ReadDataFile's CALL __stricmp to our ingame.lvl shim.
   // Modtools: 5-byte direct CALL (E8 rel32) — overwrite with CALL to shim.
   // Steam/GOG: 6-byte indirect CALL (FF15 addr32) — overwrite with CALL + NOP.
   // Sections are still writable at this point.
   if (g_game.ingame_stricmp_call) {
      g_ingameStricmpCallSite = (uint8_t*)resolve(exe_base, g_game.ingame_stricmp_call);
      unsigned callSize = g_game.ingame_stricmp_call_size;

      // Save original bytes for uninstall
      memcpy(g_ingameStricmpOrigBytes, g_ingameStricmpCallSite, callSize);

      // Write CALL rel32 to our shim
      uintptr_t callAddr = (uintptr_t)g_ingameStricmpCallSite;
      int32_t rel = (int32_t)((uintptr_t)&ingame_stricmp_shim - (callAddr + 5));
      g_ingameStricmpCallSite[0] = 0xE8;
      memcpy(g_ingameStricmpCallSite + 1, &rel, 4);
      // NOP any remaining bytes (6-byte indirect calls need 1 NOP)
      for (unsigned i = 5; i < callSize; ++i)
         g_ingameStricmpCallSite[i] = 0x90;

      dbg_log("[ingame_stricmp] patched %u bytes at 0x%08x -> shim @ 0x%08x\n",
              callSize, (unsigned)g_game.ingame_stricmp_call, (unsigned)(uintptr_t)&ingame_stricmp_shim);
   }
#endif

   // Resolve Weapon::ZoomFirstPerson for the barrel fire origin hook
   fn_ZoomFirstPerson = (ZoomFirstPerson_t)resolve(exe_base, g_game.weapon_zoom_first_person);

   // Resolve EntityFlyer::Update landing region fire suppression patch site.
   // At this address there's a JNP (0x7B) that skips fire suppression when
   // mInLandingRegionFactor == 0. Changing to JMP (0xEB) allows firing while flying
   // in landing regions.  Sections are still writable at this point.
   if (g_game.flyer_landing_fire_jnp) {
      g_flyerLandingFirePatch = (unsigned char*)resolve(exe_base, g_game.flyer_landing_fire_jnp);
      if (*g_flyerLandingFirePatch == 0x7B)
         g_flyerLandingFireOrig = 0x7B;
   }

   // Patch WeaponCannon vtable: replace OverrideAimer with our hook.
   // Validate that the slot currently points to the vanilla implementation.
   g_cannonOverrideAimerSlot = (void**)resolve(exe_base, g_game.weapon_cannon_vftable_override_aimer);
   void* expected_impl  = resolve(exe_base, g_game.weapon_override_aimer_impl);
   void* expected_thunk = resolve(exe_base, g_game.weapon_override_aimer_thunk);

   g_cannonOverrideAimerHook = (void*)&hooked_cannon_OverrideAimer;

   if (*g_cannonOverrideAimerSlot == expected_impl ||
       (expected_thunk && *g_cannonOverrideAimerSlot == expected_thunk)) {
      g_cannonOverrideAimerOrig = *g_cannonOverrideAimerSlot;
      *g_cannonOverrideAimerSlot = g_cannonOverrideAimerHook;
   }

   // If Lua is already initialized, register immediately.
   if (!g_L && g_game.g_lua_state_ptr) {
      lua_State** p_lua_state = (lua_State**)resolve(exe_base, g_game.g_lua_state_ptr);
      if (p_lua_state && *p_lua_state) {
         g_L = *p_lua_state;
         register_lua_functions(g_L);
      }
   }

   dbg_log("[lua_hooks] install complete\n");
}

void lua_hooks_post_install()
{
}

void lua_hooks_uninstall()
{
   if (original_init_state || original_close_state || original_signal_fire) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      if (original_init_state)
         DetourDetach(&(PVOID&)original_init_state, hooked_init_state);
      if (original_close_state)
         DetourDetach(&(PVOID&)original_close_state, hooked_close_state);
      if (original_signal_fire)
         DetourDetach(&(PVOID&)original_signal_fire, hooked_signal_fire);
      if (original_override_soldier_velocity)
         DetourDetach(&(PVOID&)original_override_soldier_velocity, hooked_override_soldier_velocity);
      if (original_char_exit_vehicle)
         DetourDetach(&(PVOID&)original_char_exit_vehicle, hooked_char_exit_vehicle);
      DetourTransactionCommit();
   }

   // Restore LoadDisplay path operand
   if (g_loadDisplay_push_op_ptr && g_loadDisplay_push_op_orig) {
      DWORD oldProt;
      if (VirtualProtect(g_loadDisplay_push_op_ptr, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
         *g_loadDisplay_push_op_ptr = g_loadDisplay_push_op_orig;
         VirtualProtect(g_loadDisplay_push_op_ptr, sizeof(uint32_t), oldProt, &oldProt);
      }
   }

   // Restore ingame.lvl stricmp call
   if (g_ingameStricmpCallSite && g_game.ingame_stricmp_call_size) {
      DWORD oldProt;
      unsigned sz = g_game.ingame_stricmp_call_size;
      if (VirtualProtect(g_ingameStricmpCallSite, sz, PAGE_EXECUTE_READWRITE, &oldProt)) {
         memcpy(g_ingameStricmpCallSite, g_ingameStricmpOrigBytes, sz);
         VirtualProtect(g_ingameStricmpCallSite, sz, oldProt, &oldProt);
      }
   }

   // Restore WeaponCannon vtable entry
   if (g_cannonOverrideAimerSlot && g_cannonOverrideAimerOrig) {
      DWORD oldProt;
      if (VirtualProtect(g_cannonOverrideAimerSlot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
         *g_cannonOverrideAimerSlot = g_cannonOverrideAimerOrig;
         VirtualProtect(g_cannonOverrideAimerSlot, sizeof(void*), oldProt, &oldProt);
      }
   }
}
