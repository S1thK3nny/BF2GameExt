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
   INI_PATCH("LimitIncreases", "NetworkTimerIncrease","1", "Raise the input/voice-chat update tick from 30 Hz to 120 Hz (the simulation tick is untouched)", "Network Timer Increase"),
   INI_PATCH("LimitIncreases", "MatrixPoolIncrease",  "1", "Extend matrix / item pool size",                      "Matrix/Item Pool Limit Extension"),
   INI_PATCH("LimitIncreases", "StringPoolIncrease", "1", "Increase string pool size",                           "String Pool Increase"),
   INI_PATCH("LimitIncreases", "AudioStreamLimit",   "1", "Raise the concurrent OpenAudioStream limit from 6 to 12. Each stream needs 3.4 MB of contiguous buffers, so this reserves 40 MB of the 32-bit process's 2 GB of virtual address space (RAM use is lower - pages are only committed as streams are actually used)", "Audio Stream Limit Increase"),
   INI_ENTRY("LimitIncreases", "GCVisualLimits",     "1", "Raise Galactic Conquest galaxy-map pathway/particle draw limits (fixes missing pathways and icons with >13 planets)"),

   // [Fixes] — bug-fix patches
   INI_PATCH("Fixes", "ChunkPushFix", "1", "Let explosions push bodies that break into chunks, instead of dropping them where they stood", "Chunk Push Fix"),
   INI_PATCH("Fixes", "PropGeneratorLoopFix", "1", "Fix foliage-update crash at very high FOVs (PrismaticFlower's fix)", "PropGenerator Update Loop Exit Condition"),
   INI_PATCH("Fixes", "SkyObjectLimit", "1", "Raise the SkyObjectClass instance limit (PrismaticFlower's fix)", "SkyObjectClass Limit Extension"),
   INI_PATCH("Fixes", "SaberBlockFix", "1", "Let lightsabers block other lightsabers from any direction. In stock BF2 a saber block only registers while you happen to be aiming at the centre of the map. Set 0 for stock", "Lightsaber Block Direction Fix"),
   INI_ENTRY("Fixes", "TerrainTextureFix", "1", "Re-resolve terrain detail/white textures each map (fixes playlist crash; PrismaticFlower's fix)"),
   INI_ENTRY("Fixes", "BarrelFireOriginFix", "1", "Fire projectiles from barrel hardpoint instead of bone_head. HINT: firing from the barrel adds barrel-to-crosshair parallax, so shots may not land exactly on the reticle once ReticleCorrection re-aligns it to the 3D aim point (worst at close range and with large weapon offsets). Set ReticleCorrection=0 if barrel-origin shots feel off-point"),
   INI_ENTRY("Fixes", "BlurDownsizeClamp", "1", "Clamp blur effect downsize resolution to 512px at high resolutions (PrismaticFlower's fix)"),
   INI_ENTRY("Fixes", "ScreenshotFix", "1", "Replace the broken Print Screen handler on retail builds (PrismaticFlower's fix)"),
   INI_ENTRY("Fixes", "ErrorDialogFix", "1", "Restore fatal-error dialogs on retail builds via a template in BF2GameExt.dll (PrismaticFlower's fix)"),
   INI_ENTRY("Fixes", "DLCMissionInitFix", "0", "EXPERIMENTAL: initialize the DLC mission list when launching a mission from the commandline (PrismaticFlower's fix; not yet working on retail, keep off)"),
   INI_ENTRY("Fixes", "DroidekaDeathAnimation", "1", "Let droidekas play their death animation (death01) instead of exploding instantly; banks without one are unaffected"),
   INI_ENTRY("Fixes", "ReticleCorrection", "-1", "HUD widescreen reticle vertical alignment: -1 auto (scales with aspect ratio), 0 to disable, or a manual strength 0..1 (full letterbox undo at 1)"),

   // [Features] — optional gameplay features (may require additional assets)
   INI_ENTRY("Features", "Prone", "1", "Enable prone stance. Requires data\\_lvl_pc\\prone.lvl (the human_5 animation sub-bank), which is loaded automatically alongside every ingame.lvl read; prone stays off for any mission where that file is missing"),
   INI_ENTRY("Features", "GameLogging", "0", "Enable the engine's BFront2.log file logging on retail builds (modtools always logs)"),
   INI_ENTRY("Features", "EnableSoundWarnings", "0", "Log 'Unable to find sound property' warnings for missing sounds (modtools only; retail stripped the warning code)"),
   INI_ENTRY("Features", "DisableAwardBuffs", "0", "Remove the permanent combat-award buffs. Buffs from officer buff weapons and buff pickups are untouched. Technician's award weapon shares the same unlock bit as its passive, so it stays locked too"),
   INI_ENTRY("Features", "DisableAwardWeapons", "0", "Remove the combat-award weapons. Set alongside DisableAwardBuffs to disable all nine awards"),
   INI_ENTRY("Features", "DisableDeadBodyShooting", "1", "Stop AI from shooting dead bodies entirely (overrides DeadBodyShootingAllFactions)"),
   INI_ENTRY("Features", "DeadBodyShootingAllFactions", "0", "Let all factions shoot dead bodies, not just Alliance (ignored if DisableDeadBodyShooting=1)"),

   // [AI] — AI behaviour tuning
   INI_ENTRY("AI", "PlayerVisionFairness",   "1", "AI spot you at the same range they spot a bot. Stock BF2 doubles its view range for human players. Set 0 for stock"),
   INI_ENTRY("AI", "PlayerPriorityFairness", "1", "AI rank you the same as a bot at equal distance. Stock BF2 ranks you as if you were half as far away. Set 0 for stock"),
   INI_ENTRY("AI", "PlayerThreatFairness",   "0", "AI stop treating you as extra dangerous while you are aiming at them. OFF by default: it is the one fairness option that makes AI feel unresponsive, since they no longer react to being aimed at. Set 1 to enable"),
   INI_ENTRY("AI", "PlayerAwarenessFairness","1", "AI keep looking for other enemies while fighting you. In stock BF2 an AI tracking you cannot notice anyone else at all. Set 0 for stock"),

   // [Controller] — gamepad support
   INI_ENTRY("Controller", "Enabled", "1", "Enable gamepad / controller support"),
   INI_ENTRY("Controller", "Rumble",  "1", "Enable controller rumble / vibration"),

   // [AimAssist] — controller aim assist (Xbox-style, singleplayer only)
   INI_ENTRY("AimAssist", "Enabled",                 "0",   "Enable controller aim assist"),
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
