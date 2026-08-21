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
   INI_PATCH("LimitIncreases", "SoldierHeightCeiling", "0", "Stop killing soldiers that go above world height 1000. Stock BF2 kills any soldier whose Y passes 1000 - including on spawn - which caps how tall a map can be. Flyers were never affected: the check exists only in the soldier update. The floor at -1000, the fall-speed kill and the +/-2500 horizontal walls are left alone. Cost: a soldier that genuinely escapes upward is never cleaned up and keeps ticking. Set 1 to enable", "Soldier Height Ceiling Removal"),
   INI_ENTRY("LimitIncreases", "ReservationPoolSize", "127", "How many AI reservation claims may be held at once. Squads book vehicle seats, repair points, attack slots and formation positions through a single 60-entry pool, and once it is full every further claim is dropped - the unit keeps re-requesting it and the log fills with \"List pool is full\". 127 is the encoding ceiling, a little over double the room, and costs 1.6 KB. Set 60 for stock. The figure that warning asks for is not real demand: it counts rejected claims since the map loaded"),
   INI_PATCH("LimitIncreases", "HeapExtension",       "1", "Extend RedMemory heap size",                          "RedMemory Heap Extensions"),
   INI_PATCH("LimitIncreases", "SoundLayerLimit",     "1", "Raise SoundParameterized layer limit",                "SoundParameterized Layer Limit Extension"),
   INI_PATCH("LimitIncreases", "DLCMissionLimit",     "1", "Raise DLC / addon mission limit",                     "DLC Mission Limit Extension"),
   INI_PATCH("LimitIncreases", "SoundLimit",          "1", "Raise global sound limit",                            "Sound Limit Extension"),
   INI_PATCH("LimitIncreases", "ObjectLimitIncrease", "1", "Raise entity / object pool limit",                    "Object Limit Increase"),
   INI_PATCH("LimitIncreases", "ComboAnimIncrease",   "1", "Raise combo animation limit",                         "Combo Anims Increase"),
   INI_PATCH("LimitIncreases", "HighResAnimLimit",    "1", "Raise high-resolution animation limit",               "High-Res Animation Limit"),
   INI_PATCH("LimitIncreases", "NetworkTimerIncrease","1", "Raise the input/voice-chat update tick from 30 Hz to 120 Hz (the simulation tick is untouched)", "Network Timer Increase"),
   INI_PATCH("LimitIncreases", "MatrixPoolIncrease",  "1", "Extend matrix / item pool size",                      "Matrix/Item Pool Limit Extension"),
   INI_PATCH("LimitIncreases", "StringPoolIncrease", "1", "Increase string pool size",                           "String Pool Increase"),
   INI_ENTRY("LimitIncreases", "VoiceLimit", "0", "How many sounds may be audible at once. 0 keeps the stock limit of 32. Otherwise a count from 33 to 119; the engine's own probe, voice pool and two ceilings are all raised to match. Works in both mixing paths: under EAX (5.1/7.1 or an audio mode that selects DirectSound hardware) the extra voices are hardware buffers and DirectSound must have some to spare, while software mixing needs nothing external but costs more CPU per voice. Costs 1.4 KB per voice"),
   INI_PATCH("LimitIncreases", "AudioStreamLimit",   "1", "Raise how many sounds can stream at the same time from 6 to 12. Uses more memory",                    "Audio Stream Limit Increase"),
   INI_PATCH("LimitIncreases", "LODLimitExtension",  "1", "Troops and props snap to their blurry low-detail models as soon as a fight gets crowded. Keeps roughly twenty times as many of them at full detail",                                                       "LOD Limit Extension"),
   INI_PATCH("LimitIncreases", "ExplosionVisibleRadius","1","Explosions more than a short way off were not drawn at all, so distant fighting looked empty. Makes them visible across the map",                                                                        "Explosion VisibleRadius Increase"),
   INI_ENTRY("LimitIncreases", "GCVisualLimits",     "1", "Raise Galactic Conquest galaxy-map pathway/particle draw limits (fixes missing pathways and icons with >13 planets)"),

   // [Particles] — one switch for engine correctness, one dial for density.
   // The three fix patch sets deliberately share a single key; ini_lookup_patch_set
   // resolves each set name to the first registry row naming it, so they toggle together.
   INI_PATCH("Particles", "ParticleFixes", "1", "Fix the particle engine: use all the batch caches, stop a full batch from deleting whole effects for a frame, and stop one failed frame from disabling particles for good. Turn off only to compare against stock behaviour", "Particle Cache Increase"),
   INI_PATCH("Particles", "ParticleFixes", "1", nullptr, "Particle Effect Skip Fix"),
   INI_PATCH("Particles", "ParticleFixes", "1", nullptr, "Particle Cache Reset Fix"),
   INI_ENTRY("Particles", "ParticleDensity", "0", "How many particles effects are allowed to show. 0 = stock, 1 = balanced (full density near and mid-range, stock thinning far away, and effects that ask for more than 128 particles get them), 2 = maximum (no thinning with distance at all). Higher costs frame time"),

   // [Fixes] — bug-fix patches
   INI_PATCH("Fixes", "ChunkPushFix", "1", "Let explosions push bodies that break into chunks, instead of dropping them where they stood", "Chunk Push Fix"),
   INI_PATCH("Fixes", "PropGeneratorLoopFix", "1", "Fix foliage-update crash at very high FOVs (PrismaticFlower's fix)", "PropGenerator Update Loop Exit Condition"),
   INI_PATCH("Fixes", "SkyObjectLimit", "1", "Raise the SkyObjectClass instance limit (PrismaticFlower's fix)", "SkyObjectClass Limit Extension"),
   INI_PATCH("Fixes", "SaberBlockFix", "1", "Let lightsabers block other lightsabers from any direction. In stock BF2 a saber block only registers while you happen to be aiming at the centre of the map. Set 0 for stock", "Lightsaber Block Direction Fix"),
   INI_PATCH("Fixes", "BranchRegionFix", "1", "Make EntityPath branch regions work. The engine calls the wrong vtable slot so no branch region is ever created, and derives the region id from one character too early. Name the region \"entitypathbranch <id>\" and write BranchRegion(\"<id>\") in the path node", "EntityPath Branch Region Fix"),
   INI_ENTRY("Fixes", "ImpactSoundWaterFix", "1", "Impact sounds play below world height 0. Stock BF2 compares the impact height against the water surface to choose between the water and generic impact sounds, but on a map with no water it reads an uninitialised value that happens to be just above zero, so everything below Y=0 is silent. Seeds the value properly; maps that do have water still suppress the generic sound underwater. Set 0 for stock"),
   INI_ENTRY("Fixes", "MemoryPoolHeapFix", "1", "A memory pool binds to whichever heap was current when it was constructed, and grows from that heap forever. Pools built during level load bind to the temporary load heap, which is wiped at the end of every load - so the first time such a pool grows it hands out pointers into released memory and the game crashes. This points a pool at the heap that is actually live before it grows. A mission script can avoid the problem with SetMemoryPool, but only if you have the script; this covers maps whose source is lost. Modtools only"),
   INI_ENTRY("Fixes", "CommandPostNullFix", "1", "Survive a mission script pointing command post logic at something that is not a command post. Stock BF2 dereferences the null and crashes; this logs what happened and keeps playing"),
   INI_PATCH("Fixes", "AttachedEffectsOverflowFix", "1", "A model may carry 64 attached effects. Past that the engine prints its own warning and then counts the effect anyway without storing it, so the class is built from 64 real entries plus whatever memory sits after the array, and the level dies on a null read while spawning them. The extra effects are now refused cleanly, which is exactly what the retail builds already do - their compiler dropped the same stray write. Nothing changes for a model within the limit. Modtools only", "Attached Effects Overflow Fix"),
   INI_ENTRY("Fixes", "TerrainTextureFix", "1", "Re-resolve terrain detail/white textures each map (fixes playlist crash; PrismaticFlower's fix)"),
   INI_ENTRY("Fixes", "BarrelFireOriginFix", "1", "Fire projectiles from barrel hardpoint instead of bone_head. HINT: firing from the barrel adds barrel-to-crosshair parallax, so shots may not land exactly on the reticle once ReticleCorrection re-aligns it to the 3D aim point (worst at close range and with large weapon offsets). Set ReticleCorrection=0 if barrel-origin shots feel off-point"),
   INI_ENTRY("Fixes", "BlurDownsizeClamp", "1", "Clamp blur effect downsize resolution to 512px at high resolutions (PrismaticFlower's fix)"),
   INI_ENTRY("Fixes", "ScreenshotFix", "1", "Replace the broken Print Screen handler on retail builds (PrismaticFlower's fix)"),
   INI_ENTRY("Fixes", "ErrorDialogFix", "1", "Restore fatal-error dialogs on retail builds via a template in BF2GameExt.dll (PrismaticFlower's fix)"),
   INI_ENTRY("Fixes", "DLCMissionInitFix", "0", "EXPERIMENTAL: initialize the DLC mission list when launching a mission from the commandline (PrismaticFlower's fix; not yet working on retail, keep off)"),
   INI_ENTRY("Fixes", "DroidekaDeathAnimation", "1", "Let droidekas play their death animation (death01) instead of exploding instantly; banks without one are unaffected"),
   INI_ENTRY("Fixes", "ReticleCorrection", "-1", "HUD widescreen reticle vertical alignment: -1 auto (scales with aspect ratio), 0 to disable, or a manual strength 0..1 (full letterbox undo at 1)"),

   // [Features] — optional gameplay features (may require additional assets)
   INI_ENTRY("Features", "Prone", "1", "Enable prone stance. Requires data\\_lvl_pc\\prone.lvl, which is loaded automatically alongside every ingame.lvl read; prone stays off for any mission where that file is missing"),
   INI_ENTRY("Features", "GameLogging", "0", "Enable the engine's BFront2.log file logging on retail builds"),
   INI_ENTRY("Features", "EnableSoundWarnings", "0", "Log 'Unable to find sound property' warnings for missing sounds (modtools only)"),
   INI_ENTRY("Features", "DisableAwardBuffs", "0", "Remove the permanent combat-award buffs. Buffs from officer buff weapons and buff pickups are untouched. The technician's award weapon goes with its passive"),
   INI_ENTRY("Features", "DisableAwardWeapons", "0", "Remove the combat-award weapons. Set alongside DisableAwardBuffs to disable all nine awards"),
   INI_ENTRY("Features", "DisableDeadBodyShooting", "1", "Stop AI from shooting dead bodies entirely (overrides DeadBodyShootingAllFactions)"),
   INI_ENTRY("Features", "DeadBodyShootingAllFactions", "0", "Let all factions shoot dead bodies, not just Alliance (ignored if DisableDeadBodyShooting=1)"),

   // [Lightsaber] — lightsaber blade lighting
   INI_ENTRY("Lightsaber", "LightsaberIllumination", "1", "Ignited lightsaber blades give off real light in their own blade colour. Objects can only take 4 dynamic lights at once, so a nearby saber can replace one of a room's own lights. Set 0 for stock"),
   INI_ENTRY("Lightsaber", "LightsaberLightRadius", "4.0", "How far the lightsaber light reaches, in metres at full blade extension (it grows as the blade ignites). Brightness is unaffected by this, so it only changes reach - but a larger radius evicts more of the map's own lights"),
   INI_ENTRY("Lightsaber", "LightsaberLightIntensity", "1.0", "Multiplier on the lightsaber light colour. 1.0 uses the blade colour as authored"),

   // [AI] — AI behaviour tuning
   INI_ENTRY("AI", "AIDecisionRate", "1.0", "How often AI away from a player make a NEW decision, as a multiple of stock. The engine grades every unit by distance to the nearest human and puts anything past 100 units on a two to four second decision interval, which is what standing around looks like at range. 2.0 makes every tier think twice as often; below 1.0 slows them to buy frame time back. No tier is taken below the 0.25s the engine gives units standing next to you. Range 0.25 to 4.0"),
   INI_ENTRY("AI", "AIUpdateBudget", "0", "How many AI units may make a NEW decision each simulation turn. 0 keeps the stock 10. The engine services only the most overdue units and re-queues them, while movement and firing run for everyone every turn - so a unit that misses its slot keeps walking and shooting but never re-decides, which is what standing around looks like. Raising this costs frame time and only helps if the budget is actually the constraint, so measure before changing it. Range 10 to 127"),
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

   // ---- Diagnostic ---------------------------------------------------------
   // Read-only instrumentation. Each one only writes to BF2GameExt.log; none of
   // them change behaviour, and all default off.
   INI_ENTRY("Diagnostic", "SoundDiagnostic",   "0", "Report how many sound voices this machine actually gets, how many sounds are being dropped for want of one, and watch the mixed output for bursts pinned at full scale. Modtools only"),
   INI_ENTRY("Diagnostic", "BranchRegionDebug", "0", "Log every step of EntityPath branch-region resolution, so a BranchRegion that will not resolve can be traced. Modtools only"),
   INI_ENTRY("Diagnostic", "AIUpdateDiag",      "0", "Report how many AI units are getting a decision each turn against how many want one, and the spread of units across LOD tiers. This is what says whether AIUpdateBudget is worth raising. Modtools only"),
   INI_ENTRY("Diagnostic", "PoolGrowthDiag",    "0", "Log every memory pool growth with the pool name, the heap it was built on and the heap that is live. A captured heap that differs from the live one is the crash MemoryPoolHeapFix repairs"),
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
