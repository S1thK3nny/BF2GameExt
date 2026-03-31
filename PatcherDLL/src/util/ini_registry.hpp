#pragma once

// Central registry of every INI section/key that the runtime reads.
// This is the SINGLE SOURCE OF TRUTH — generate_ini.py parses this file to
// produce dist/BF2GameExt.ini, and the runtime code uses these constants for
// lookups.  When you add a new feature with an INI toggle, add it here and
// re-run: python generate_ini.py
//
// Controller per-button bindings are NOT listed here because they are
// data-driven from the ModeBindingDef tables in controller_support.cpp.
// generate_ini.py parses those tables separately.

// --- Entry definition ---------------------------------------------------

struct IniEntry {
   const char* section;       // INI [Section]
   const char* key;           // Key name
   const char* default_value; // Default when key/file is absent
   const char* comment;       // Human-readable description (nullptr = none)
   const char* patch_set;     // If non-null, the patch_set::name this maps to
};

// INI_ENTRY / INI_PATCH — helpers to keep the table compact.
#define INI_ENTRY(sec, key, def, comment) \
   { sec, key, def, comment, nullptr }
#define INI_PATCH(sec, key, def, comment, patch_name) \
   { sec, key, def, comment, patch_name }

// --- The registry -------------------------------------------------------
// generate_ini.py looks for the block between BEGIN_REGISTRY / END_REGISTRY
// markers and parses each INI_ENTRY / INI_PATCH line with a simple regex.

// BEGIN_REGISTRY
inline constexpr IniEntry g_ini_registry[] = {

   // [General] — read by DInput8Proxy (dinput8.dll), not PatcherDLL
   INI_ENTRY("General", "Enabled",  "1",              "Master enable/disable switch for BF2GameExt"),
   INI_ENTRY("General", "DLLPath",  "BF2GameExt.dll", "Path to the main extension DLL (relative to proxy)"),

   // [LimitIncreases] — engine limit patches (all default to enabled)
   INI_PATCH("LimitIncreases", "HeapExtension",       "1", "Extend RedMemory heap size",                          "RedMemory Heap Extensions"),
   INI_PATCH("LimitIncreases", "SoundLayerLimit",     "1", "Raise SoundParameterized layer limit",                "SoundParameterized Layer Limit Extension"),
   INI_PATCH("LimitIncreases", "DLCMissionLimit",     "1", "Raise DLC / addon mission limit",                     "DLC Mission Limit Extension"),
   INI_PATCH("LimitIncreases", "SoundLimit",          "1", "Raise global sound limit",                            "Sound Limit Extension"),
   INI_PATCH("LimitIncreases", "ParticleCacheIncrease","1","Increase particle effect cache",                      "Particle Cache Increase"),
   INI_PATCH("LimitIncreases", "ObjectLimitIncrease", "1", "Raise entity / object pool limit",                    "Object Limit Increase"),
   INI_PATCH("LimitIncreases", "ComboAnimIncrease",   "1", "Raise combo animation limit",                         "Combo Anims Increase"),
   INI_PATCH("LimitIncreases", "HighResAnimLimit",    "1", "Raise high-resolution animation limit",               "High-Res Animation Limit"),
   INI_PATCH("LimitIncreases", "NetworkTimerIncrease","1", "Increase network timer count",                        "Network Timer Increase"),
   INI_PATCH("LimitIncreases", "MatrixPoolIncrease",  "1", "Extend matrix / item pool size",                      "Matrix/Item Pool Limit Extension"),

   // [Fixes] — bug-fix patches
   INI_PATCH("Fixes", "ChunkPushFix", "1", "Fix chunk push crash", "Chunk Push Fix"),

   // [Features] — optional gameplay features (may require additional assets)
   INI_ENTRY("Features", "Prone", "1", "Enable prone stance (requires prone animations in soldier banks)"),

   // [Controller] — gamepad support
   INI_ENTRY("Controller", "Enabled", "1", "Enable gamepad / controller support"),
   INI_ENTRY("Controller", "Rumble",  "1", "Enable controller rumble / vibration"),

   // [AimAssist] — controller aim assist (Xbox-style, singleplayer only)
   INI_ENTRY("AimAssist", "Enabled",                 "1",   "Enable controller aim assist"),
   INI_ENTRY("AimAssist", "ConeAngle",               "30",  "Fallback cone angle in degrees when weapon has no AutoAimSize"),
   INI_ENTRY("AimAssist", "TrackingDeadZone",         "0.5", "Dead zone multiplier for weapon AutoAimSize"),
   INI_ENTRY("AimAssist", "FrictionStrength",          "3.0", "Directional friction scale when aiming away from lock"),
   INI_ENTRY("AimAssist", "PullStrength",              "5.0", "Auto-tracking ramp rate per second toward locked target"),
   INI_ENTRY("AimAssist", "LockBreakTime",             "0.1", "Seconds of pushing away to break target lock"),
   INI_ENTRY("AimAssist", "AutoLockOnHit",             "1",   "Automatically lock onto first enemy you damage"),
   INI_ENTRY("AimAssist", "SnapStrength",              "1.0", "Instant correction on first lock frame (0 = ramp only)"),
   INI_ENTRY("AimAssist", "ProximityFriction",         "1",   "Slow stick when crosshair is near any enemy"),
   INI_ENTRY("AimAssist", "ProximityFrictionRadius",   "0.5", "Screen-space radius for proximity slowdown"),
   INI_ENTRY("AimAssist", "ProximityFrictionScale",    "0.4", "Min friction at dead center (0 = full stop, 1 = none)"),
};
// END_REGISTRY

inline constexpr int g_ini_registry_count =
   sizeof(g_ini_registry) / sizeof(g_ini_registry[0]);

// --- Lookup helper used by apply_patches --------------------------------

struct IniLookup {
   const char* section;
   const char* key;
};

// Find the INI section+key for a given patch_set name.  Returns {nullptr,nullptr} if
// the patch set has no INI toggle (should never happen for shipped patch sets).
inline IniLookup ini_lookup_patch_set(const char* patch_set_name)
{
   for (int i = 0; i < g_ini_registry_count; ++i) {
      if (g_ini_registry[i].patch_set &&
          strcmp(g_ini_registry[i].patch_set, patch_set_name) == 0) {
         return { g_ini_registry[i].section, g_ini_registry[i].key };
      }
   }
   return { nullptr, nullptr };
}
