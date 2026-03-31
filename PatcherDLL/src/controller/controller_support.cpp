#include "pch.h"
#include "controller_support.hpp"
#include "util/ini_config.hpp"
#include "core/resolve.hpp"

bool g_controllerEnabled = false;

// ---------------------------------------------------------------------------
// GameLog
// ---------------------------------------------------------------------------

static GameLog_t g_log = nullptr;

// ---------------------------------------------------------------------------
// Game function typedefs
// ---------------------------------------------------------------------------

// SetJoystickEnabled init chain -- __fastcall (ECX = config base)
using fn_joystick_discover = void(__fastcall*)(uintptr_t ecx, void* edx_unused);

using fn_joystick_sync     = void(__fastcall*)(uintptr_t ecx, void* edx_unused);

// ---------------------------------------------------------------------------
// INI path storage (set once from dllmain, reused by Lua API resets)
// ---------------------------------------------------------------------------

static char s_storedIniPath[MAX_PATH] = { 0 };

void controller_set_ini_path(const char* ini_path)
{
   if (ini_path && ini_path[0])
      strncpy_s(s_storedIniPath, sizeof(s_storedIniPath), ini_path, _TRUNCATE);
}

// ---------------------------------------------------------------------------
// Per-mode default binding definitions
// ---------------------------------------------------------------------------

struct ModeBindingDef {
   const char* inputName;
   const char* defaultActions;  // comma-separated for multi-bind
};

// Unit (Infantry) -- standard FPS layout
static const ModeBindingDef s_unitDefaults[] = {
   { "A",         "Jump" },
   { "B",         "Crouch,Roll" },
   { "X",         "Reload" },
   { "Y",         "Use" },
   { "LB",        "SecondaryNext" },
   { "RB",        "PrimaryNext" },
   { "Back",      "PlayerList" },
   { "Start",     "View" },
   { "L3",        "Sprint" },
   { "R3",        "Zoom" },
   { "DPadUp",    "SquadCommand" },
   { "DPadRight", "AcceptHero" },
   { "DPadDown",  "DeclineHero" },
   { "DPadLeft",  "LockTarget" },
   { "RT",        "PrimaryFire" },
   { "LT",        "SecondaryFire" },
   { "LX+",       "StrafeAxis" },
   { "LY-",       "MoveAxis" },
   { "RX+",       "TurnAxis" },
   { "RY-",       "PitchAxis" },
   { nullptr, nullptr },
};

// Vehicle (Hover/Walker/Speeder)
static const ModeBindingDef s_vehicleDefaults[] = {
   { "A",         "Jump" },
   { "B",         "Crouch,Roll" },
   { "X",         "LockTarget" },
   { "Y",         "Use" },
   { "LB",        "SecondaryNext" },
   { "RB",        "PrimaryNext" },
   { "Back",      "Map" },
   { "Start",     "View" },
   { "L3",        "Sprint" },
   { "R3",        "Zoom" },
   { "DPadUp",    "SquadCommand" },
   { "DPadRight", "AcceptHero" },
   { "DPadDown",  "Reload" },
   { "DPadLeft",  "DeclineHero" },
   { "RT",        "PrimaryFire" },
   { "LT",        "SecondaryFire" },
   { "LX+",       "StrafeAxis" },
   { "LY-",       "MoveAxis" },
   { "RX+",       "TurnAxis" },
   { "RY-",       "PitchAxis" },
   { nullptr, nullptr },
};

// Flyer (Starfighter/Gunship) -- LB/RB = barrel roll, weapon cycle on D-pad
static const ModeBindingDef s_flyerDefaults[] = {
   { "A",         "Jump" },
   { "B",         "Crouch" },
   { "X",         "LockTarget" },
   { "Y",         "Use" },
   { "LB",        "StrafeNeg" },
   { "RB",        "StrafePos" },
   { "Back",      "Map" },
   { "Start",     "View" },
   { "L3",        "Sprint" },
   { "R3",        "Zoom" },
   { "DPadUp",    "SquadCommand" },
   { "DPadRight", "PrimaryNext,AcceptHero" },
   { "DPadDown",  "Reload" },
   { "DPadLeft",  "PrimaryPrev,DeclineHero" },
   { "RT",        "PrimaryFire" },
   { "LT",        "SecondaryFire" },
   { "LX+",       "StrafeAxis" },
   { "LY-",       "MoveAxis" },
   { "RX+",       "TurnAxis" },
   { "RY-",       "PitchAxis" },
   { nullptr, nullptr },
};

// Hero (Jedi)
static const ModeBindingDef s_heroDefaults[] = {
   { "A",         "Jump" },
   { "B",         "Crouch,Roll" },
   { "X",         "LockTarget" },
   { "Y",         "Use" },
   { "LB",        "SecondaryNext" },
   { "RB",        "PrimaryNext" },
   { "Back",      "Map" },
   { "Start",     "View" },
   { "L3",        "Sprint" },
   { "R3",        "Zoom" },
   { "DPadUp",    "SquadCommand" },
   { "DPadRight", "AcceptHero" },
   { "DPadDown",  "Reload" },
   { "DPadLeft",  "DeclineHero" },
   { "RT",        "PrimaryFire" },
   { "LT",        "SecondaryFire" },
   { "LX+",       "StrafeAxis" },
   { "LY-",       "MoveAxis" },
   { "RX+",       "TurnAxis" },
   { "RY-",       "PitchAxis" },
   { nullptr, nullptr },
};

// Turret -- minimal bindings, empty strings for unused inputs
static const ModeBindingDef s_turretDefaults[] = {
   { "A",         "" },
   { "B",         "" },
   { "X",         "LockTarget" },
   { "Y",         "Use" },
   { "LB",        "" },
   { "RB",        "" },
   { "Back",      "Map" },
   { "Start",     "View" },
   { "L3",        "" },
   { "R3",        "Zoom" },
   { "DPadUp",    "SquadCommand" },
   { "DPadRight", "PrimaryNext,AcceptHero" },
   { "DPadDown",  "Reload" },
   { "DPadLeft",  "PrimaryPrev,DeclineHero" },
   { "RT",        "PrimaryFire" },
   { "LT",        "SecondaryFire" },
   { "LX+",       "StrafeAxis" },
   { "LY-",       "MoveAxis" },
   { "RX+",       "TurnAxis" },
   { "RY-",       "PitchAxis" },
   { nullptr, nullptr },
};

static const ModeBindingDef* s_modeDefaults[CONTROL_MODE_COUNT] = {
   s_unitDefaults,     // 0 = Infantry
   s_vehicleDefaults,  // 1 = Vehicle
   s_flyerDefaults,    // 2 = Flyer
   s_heroDefaults,     // 3 = Jedi
   s_turretDefaults,   // 4 = Turret
};

static const char* s_modeSectionNames[CONTROL_MODE_COUNT] = {
   "Controller.Unit",
   "Controller.Vehicle",
   "Controller.Flyer",
   "Controller.Hero",
   "Controller.Turret",
};

// ---------------------------------------------------------------------------
// String -> enum lookup tables for Lua API
// ---------------------------------------------------------------------------

struct NamedValue {
   const char* name;
   int value;
};

static const NamedValue s_rawInputNames[] = {
   { "A",         eCONTROLLERINPUT_BUTTON0 },
   { "B",         eCONTROLLERINPUT_BUTTON1 },
   { "X",         eCONTROLLERINPUT_BUTTON2 },
   { "Y",         eCONTROLLERINPUT_BUTTON3 },
   { "LB",        eCONTROLLERINPUT_BUTTON4 },
   { "RB",        eCONTROLLERINPUT_BUTTON5 },
   { "Back",      eCONTROLLERINPUT_BUTTON6 },
   { "Start",     eCONTROLLERINPUT_BUTTON7 },
   { "L3",        eCONTROLLERINPUT_BUTTON8 },
   { "R3",        eCONTROLLERINPUT_BUTTON9 },
   { "DPadUp",    eCONTROLLERINPUT_HAT0_UP },
   { "DPadRight", eCONTROLLERINPUT_HAT0_RIGHT },
   { "DPadDown",  eCONTROLLERINPUT_HAT0_DOWN },
   { "DPadLeft",  eCONTROLLERINPUT_HAT0_LEFT },
   { "LX+",       eCONTROLLERINPUT_X_POS },
   { "LX-",       eCONTROLLERINPUT_X_NEG },
   { "LY+",       eCONTROLLERINPUT_Y_POS },
   { "LY-",       eCONTROLLERINPUT_Y_NEG },
   { "ZPos",      eCONTROLLERINPUT_Z_POS },
   { "ZNeg",      eCONTROLLERINPUT_Z_NEG },
   { "RX+",       eCONTROLLERINPUT_RX_POS },
   { "RX-",       eCONTROLLERINPUT_RX_NEG },
   { "RY+",       eCONTROLLERINPUT_RY_POS },
   { "RY-",       eCONTROLLERINPUT_RY_NEG },
   { "RZPos",     eCONTROLLERINPUT_RZ_POS },
   { "RZNeg",     eCONTROLLERINPUT_RZ_NEG },
   // User-friendly trigger aliases (DI Z axis: LT=Z+, RT=Z-)
   { "RT",        eCONTROLLERINPUT_Z_NEG },
   { "LT",        eCONTROLLERINPUT_Z_POS },
   { nullptr, 0 },
};

static const NamedValue s_actionNames[] = {
   { "PrimaryFire",    ePROCESSEDINPUT_primaryFireButtonDown },
   { "SecondaryFire",  ePROCESSEDINPUT_secondaryFireButtonDown },
   { "Sprint",         ePROCESSEDINPUT_sprintButtonDown },
   { "Jump",           ePROCESSEDINPUT_jumpButtonDown },
   { "Crouch",         ePROCESSEDINPUT_crouchButtonDown },
   { "Zoom",           ePROCESSEDINPUT_zoomButtonDown },
   { "View",           ePROCESSEDINPUT_viewButtonDown },
   { "Reload",         ePROCESSEDINPUT_reloadButtonDown },
   { "Use",            ePROCESSEDINPUT_useButtonPressed },
   { "SquadCommand",   ePROCESSEDINPUT_squadCommandButtonPressed },
   { "AcceptHero",     ePROCESSEDINPUT_acceptHeroPressed },
   { "DeclineHero",    ePROCESSEDINPUT_declineHeroPressed },
   { "LockTarget",     ePROCESSEDINPUT_lockTargetButtonPressed },
   { "PrimaryNext",    ePROCESSEDINPUT_primaryNextButtonPressed },
   { "PrimaryPrev",    ePROCESSEDINPUT_primaryPrevButtonPressed },
   { "SecondaryNext",  ePROCESSEDINPUT_secondaryNextButtonPressed },
   { "SecondaryPrev",  ePROCESSEDINPUT_secondaryPrevButtonPressed },
   { "PlayerList",     ePROCESSEDINPUT_playerList },
   { "Map",            ePROCESSEDINPUT_map },
   { "Roll",           ePROCESSEDINPUT_rollButtonDown },
   { "StrafeAxis",     ePROCESSEDINPUT_STRAFE_AXIS },
   { "MoveAxis",       ePROCESSEDINPUT_MOVE_AXIS },
   { "TurnAxis",       ePROCESSEDINPUT_TURN_AXIS },
   { "PitchAxis",      ePROCESSEDINPUT_PITCH_AXIS },
   // Half-axis actions (for digital buttons -> axis mapping)
   { "StrafePos",      ePROCESSEDINPUT_STRAFE_POS },
   { "StrafeNeg",      ePROCESSEDINPUT_STRAFE_NEG },
   { "MovePos",        ePROCESSEDINPUT_MOVE_POS },
   { "MoveNeg",        ePROCESSEDINPUT_MOVE_NEG },
   { "TurnPos",        ePROCESSEDINPUT_TURN_POS },
   { "TurnNeg",        ePROCESSEDINPUT_TURN_NEG },
   { "PitchPos",       ePROCESSEDINPUT_PITCH_POS },
   { "PitchNeg",       ePROCESSEDINPUT_PITCH_NEG },
   { "None",           ePROCESSEDINPUT_NONE },
   { nullptr, 0 },
};

// ---------------------------------------------------------------------------
// String lookup helpers
// ---------------------------------------------------------------------------

int controller_raw_input_from_name(const char* name)
{
   for (const auto* p = s_rawInputNames; p->name; ++p)
      if (_stricmp(p->name, name) == 0) return p->value;
   return -2; // unknown
}

int controller_action_from_name(const char* name)
{
   for (const auto* p = s_actionNames; p->name; ++p)
      if (_stricmp(p->name, name) == 0) return p->value;
   return -2; // unknown
}

const char* controller_action_to_name(int action)
{
   for (const auto* p = s_actionNames; p->name; ++p)
      if (p->value == action) return p->name;
   return "None";
}

// ---------------------------------------------------------------------------
// INI parsing helpers
// ---------------------------------------------------------------------------

// Look up the hardcoded default action string for an input name in a mode.
static const char* get_mode_default(int mode, const char* inputName)
{
   const ModeBindingDef* defs = s_modeDefaults[mode];
   for (const ModeBindingDef* d = defs; d->inputName; ++d)
      if (_stricmp(d->inputName, inputName) == 0)
         return d->defaultActions;
   return "";  // not bound by default
}

// Parse a comma-separated action string (e.g. "Crouch,Roll") into ButtonBinding
// entries. Returns the number of bindings written.
static int parse_action_list(const char* actionStr, int rawInput,
                             ButtonBinding* outBindings, int outMax)
{
   int count = 0;
   char buf[256];
   strncpy_s(buf, sizeof(buf), actionStr, _TRUNCATE);

   char* context = nullptr;
   char* token = strtok_s(buf, ",", &context);
   while (token && count < outMax) {
      // Trim leading/trailing spaces
      while (*token == ' ') token++;
      char* end = token + strlen(token) - 1;
      while (end > token && *end == ' ') { *end = '\0'; end--; }

      if (*token == '\0') { token = strtok_s(nullptr, ",", &context); continue; }

      int action = controller_action_from_name(token);
      if (action == -2) {
         if (g_log) g_log("[Controller] WARNING: unknown action '%s'\n", token);
      } else if (action == ePROCESSEDINPUT_NONE) {
         // "None" = explicitly unbind -- don't add any binding
      } else {
         outBindings[count].rawInput = rawInput;
         outBindings[count].processedAction = action;
         count++;
      }
      token = strtok_s(nullptr, ",", &context);
   }
   return count;
}

// ---------------------------------------------------------------------------
// Binding setup -- reads per-mode INI config and writes to game binding tables
// ---------------------------------------------------------------------------

void controller_setup_bindings(uintptr_t exe_base)
{
   if (!g_controllerEnabled) return;

   g_log = get_gamelog();

   using namespace game_addrs::modtools;

   // Check if a joystick is connected
   int* pNumJoysticks = (int*)resolve(exe_base, num_joysticks_global);
   if (!pNumJoysticks) return;
   int numJoysticks = *pNumJoysticks;
   if (numJoysticks <= 0) {
      if (g_log) g_log("[Controller] No joysticks detected (%d)\n", numJoysticks);
      return;
   }

   if (g_log) g_log("[Controller] %d joystick(s) detected, setting up bindings...\n", numJoysticks);

   // Enable joystick input processing.
   uintptr_t joyConfig = (uintptr_t)resolve(exe_base, joystick_config_base);
   if (!joyConfig) return;
   *(char*)(joyConfig + 0xF94) = 1;

   uintptr_t ctrlBase = (uintptr_t)resolve(exe_base, controller_base_global) + 0x428;

   // Table pointers and constants
   constexpr int RT_ENTRIES_PER_MODE = 0x2B; // 43
   constexpr int RT_ENTRY_SIZE = 6;
   uintptr_t luaTable = ctrlBase + 0x1ACC;
   uintptr_t rtTable  = ctrlBase + 0x20BC;

   // INI config (uses stored path from controller_set_ini_path, or null = defaults only)
   ini_config cfg{ s_storedIniPath[0] ? s_storedIniPath : nullptr };

   // Build per-mode bindings from INI overrides on top of hardcoded defaults.
   // Max bindings per mode: 28 inputs * 4 actions each = 112 (generous upper bound)
   constexpr int MAX_BINDINGS = 128;
   ButtonBinding modeBindings[CONTROL_MODE_COUNT][MAX_BINDINGS];
   int modeCounts[CONTROL_MODE_COUNT] = { 0 };

   for (int mode = 0; mode < CONTROL_MODE_COUNT; mode++) {
      const char* section = s_modeSectionNames[mode];
      int count = 0;

      // For each known input name, check INI override or use default.
      // The default tables use "RT"/"LT" for triggers, so those names will
      // match and provide defaults. "ZNeg"/"ZPos" have empty defaults and
      // won't generate bindings unless the user explicitly sets them in INI.
      for (const NamedValue* inp = s_rawInputNames; inp->name; ++inp) {
         const char* defaultVal = get_mode_default(mode, inp->name);

         char actionBuf[256];
         cfg.get_string(section, inp->name, defaultVal, actionBuf, sizeof(actionBuf));

         if (actionBuf[0] == '\0') continue;  // no binding for this input

         count += parse_action_list(actionBuf, inp->value,
                                    &modeBindings[mode][count],
                                    MAX_BINDINGS - count);
      }
      modeCounts[mode] = count;
   }

   // --- Write to 0x1ACC table (raw input -> action, for Lua API) ---
   // This table is input-indexed, so for multi-bind (B=Crouch,Roll),
   // only the last action is stored. This is fine -- 0x1ACC is Lua API only.
   for (int mode = 0; mode < CONTROL_MODE_COUNT; mode++) {
      for (int i = 0; i < modeCounts[mode]; i++) {
         int rawInput = modeBindings[mode][i].rawInput;
         int action   = modeBindings[mode][i].processedAction;
         if (rawInput >= 0 && rawInput < eCONTROLLERINPUT_MAX) {
            uintptr_t entry = luaTable + (mode * 0x4C + rawInput) * 4;
            *(int*)entry = action;
         }
      }
   }

   // --- Helper: write one mode's bindings to a table with 0x20BC format ---
   auto writeModeBindings = [&](uintptr_t tableBase, int stride, int mode) {
      // Clear slot 1 for all actions in this mode first (removes stale bindings)
      for (int action = 0; action < RT_ENTRIES_PER_MODE; action++) {
         uintptr_t entry = tableBase + mode * stride + action * RT_ENTRY_SIZE;
         *(unsigned short*)(entry + 2) = 0;  // slot 1 scancode
         *(unsigned char*)(entry + 5) = 0;   // slot 1 device index
      }
      // Write new bindings to slot 1
      for (int i = 0; i < modeCounts[mode]; i++) {
         int rawInput = modeBindings[mode][i].rawInput;
         int action   = modeBindings[mode][i].processedAction;
         if (action >= 0 && action < RT_ENTRIES_PER_MODE) {
            unsigned short encoded = (unsigned short)((rawInput + 1) << 8);
            uintptr_t entry = tableBase + mode * stride + action * RT_ENTRY_SIZE;
            *(unsigned short*)(entry + 2) = encoded;  // slot 1 scancode
            *(unsigned char*)(entry + 5) = 0;          // slot 1 device index
         }
      }
   };

   // Step 1: Write bindings to UI config FIRST (so sync preserves them)
   {
      uintptr_t uiBase = joyConfig + 0x4A;
      for (int mode = 0; mode < CONTROL_MODE_COUNT; mode++)
         writeModeBindings(uiBase, 0x102, mode);
      if (g_log) g_log("[Controller] Wrote bindings to UI config\n");
   }

   // Step 2: Call the joystick init chain (discover device + sync bindings)
   if (joystick_discover && joystick_sync) {
      uintptr_t configBase = joyConfig;
      auto discover = (fn_joystick_discover)resolve(exe_base, joystick_discover);
      auto sync     = (fn_joystick_sync)resolve(exe_base, joystick_sync);
      discover(configBase, nullptr);
      sync(configBase, nullptr);
      if (g_log) g_log("[Controller] Called joystick discover + sync\n");
   }

   // Step 3: Write bindings to 0x20BC runtime table (in case sync reset them)
   for (int mode = 0; mode < CONTROL_MODE_COUNT; mode++)
      writeModeBindings(rtTable, RT_ENTRIES_PER_MODE * RT_ENTRY_SIZE, mode);

   if (g_log) g_log("[Controller] Bindings applied for %d modes\n", CONTROL_MODE_COUNT);
}
