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
   // Positional aliases for the four face buttons. A/B/X/Y are the Xbox
   // labels; DualShock and Nintendo pads print the same four differently, and
   // Nintendo swaps the pairs outright, so "B" is ambiguous documentation for
   // two of the three pad families. These name the position as printed on the
   // pad in the player's hands instead.
   { "FaceDown",  eCONTROLLERINPUT_BUTTON0 },   // A on an Xbox pad
   { "FaceRight", eCONTROLLERINPUT_BUTTON1 },   // B
   { "FaceLeft",  eCONTROLLERINPUT_BUTTON2 },   // X
   { "FaceUp",    eCONTROLLERINPUT_BUTTON3 },   // Y
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
// Binding setup -- the INI pad layout becomes the engine's default bindings,
// and a profile mode with no pad bindings at all is given it
// ---------------------------------------------------------------------------

void controller_setup_bindings(uintptr_t exe_base)
{
   if (!g_controllerEnabled) return;

   g_log = get_gamelog();

   // Build-aware. The tProfile layout is identical across builds
   // (+0x30/+0x5f0/+0x638/+0xf94 and the 0x2c device stride match in
   // joystick_sync), and tGameOpt::ResetBindings reads the default table with
   // the same 0x102 mode stride on all three.

   // Check if a joystick is connected
   int* pNumJoysticks = (int*)resolve(exe_base, g_addr->num_joysticks_global);
   if (!pNumJoysticks) return;
   int numJoysticks = *pNumJoysticks;
   if (numJoysticks <= 0) {
      if (g_log) g_log("[Controller] No joysticks detected (%d)\n", numJoysticks);
      return;
   }

   // The current tProfile.
   uintptr_t joyConfig = (uintptr_t)resolve(exe_base, g_addr->joystick_config_base);
   if (!joyConfig) return;

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
      // Resolve one action string per PHYSICAL input, not per name.
      //
      // Several names address the same input - FaceDown and A, RT and ZNeg -
      // and this loop visits every one of them, so a line written against one
      // spelling has to suppress the other spelling's built-in default.
      // Binding per name would make `FaceDown=Reload` leave A's default Jump in
      // place and drive the button with both.
      //
      // Precedence per input: an explicit INI line wins, whichever spelling it
      // used; otherwise the built-in default. Writing two spellings of one
      // input keeps the first in the table and ignores the second rather than
      // stacking them.
      char resolved[eCONTROLLERINPUT_MAX][256] = {};
      bool haveResolved[eCONTROLLERINPUT_MAX] = {};
      bool wasExplicit[eCONTROLLERINPUT_MAX]  = {};

      // A value no INI line can produce, so "came back unchanged" means the key
      // is absent rather than present and empty - present-and-empty is a
      // deliberate unbind and has to beat the default.
      static const char kAbsent[] = { (char)0x01, (char)0 };

      for (const NamedValue* inp = s_rawInputNames; inp->name; ++inp) {
         if (inp->value < 0 || inp->value >= eCONTROLLERINPUT_MAX) continue;

         char actionBuf[256];
         cfg.get_string(section, inp->name, kAbsent, actionBuf, sizeof(actionBuf));

         if (strcmp(actionBuf, kAbsent) != 0) {
            if (wasExplicit[inp->value]) continue;   // another spelling already won
            strncpy_s(resolved[inp->value], actionBuf, _TRUNCATE);
            wasExplicit[inp->value]  = true;
            haveResolved[inp->value] = true;
         } else if (!wasExplicit[inp->value] && !haveResolved[inp->value]) {
            const char* defaultVal = get_mode_default(mode, inp->name);
            if (defaultVal[0] != 0) {
               strncpy_s(resolved[inp->value], defaultVal, _TRUNCATE);
               haveResolved[inp->value] = true;
            }
         }
      }

      for (int rawInput = 0; rawInput < eCONTROLLERINPUT_MAX; ++rawInput) {
         if (!haveResolved[rawInput] || resolved[rawInput][0] == 0) continue;
         count += parse_action_list(resolved[rawInput], rawInput,
                                    &modeBindings[mode][count],
                                    MAX_BINDINGS - count);
      }
      modeCounts[mode] = count;
   }

   // --- The pad layout as engine data ---
   // ActionKey = ushort mKey[2]; uchar mDevice[2]. A key below 0x100 is a
   // scancode; (raw + 1) << 8 is raw input `raw`, on pad mDevice when raw is
   // below 0x40 and the mouse above it. The controls screen writes keyboard and
   // pad keys into whichever slot is free, so slot 1 is not reserved for the pad.
   constexpr int kActionsPerMode = 0x2B;
   constexpr int kModeStride     = 0x102;
   constexpr int kActionKeySize  = 6;
   constexpr int kPadRawInputs   = 0x40;

   auto actionKey = [](uintptr_t table, int mode, int action) {
      return table + mode * kModeStride + action * kActionKeySize;
   };
   auto isPadKey = [](unsigned short key) {
      return key >= 0x100 && ((key >> 8) - 1) < kPadRawInputs;
   };

   // Engine defaults: new profiles and the controls screen's Restore Defaults
   // copy these. Slot 1 holds the stock secondary key and is only replaced on
   // actions the pad layout uses. Once per run, like the static init it edits.
   static bool s_defaultsPatched = false;
   if (!s_defaultsPatched && g_addr->default_keyboard_bindings) {
      uintptr_t defaults = (uintptr_t)resolve(exe_base, g_addr->default_keyboard_bindings);
      for (int mode = 0; mode < CONTROL_MODE_COUNT; mode++) {
         for (int i = 0; i < modeCounts[mode]; i++) {
            int action = modeBindings[mode][i].processedAction;
            if (action < 0 || action >= kActionsPerMode) continue;
            uintptr_t entry = actionKey(defaults, mode, action);
            *(unsigned short*)(entry + 2) = (unsigned short)((modeBindings[mode][i].rawInput + 1) << 8);
            *(unsigned char*)(entry + 5)  = 0;
         }
      }
      s_defaultsPatched = true;
   }

   // The player's profile belongs to the player. A mode only gets the pad
   // layout when it has no pad binding at all (a profile made before a pad was
   // used, or a mode put back by Restore Defaults, which resets one mode).
   // Any mode with a pad binding is left exactly as the controls screen set it.
   // Runs on every state init; the one before the profile loads is wasted, the
   // one at mission start sees the real profile.
   uintptr_t profileBindings = joyConfig + 0x4A;   // tProfile::mGameOptions.mKeyboardBindings

   bool profileChanged = false;
   if (*(char*)(joyConfig + 0xF94) == 0) {
      *(char*)(joyConfig + 0xF94) = 1;
      profileChanged = true;
   }

   char seededModes[128] = {};
   for (int mode = 0; mode < CONTROL_MODE_COUNT; mode++) {
      bool hasPadBinding = false;
      for (int action = 0; action < kActionsPerMode && !hasPadBinding; action++) {
         const unsigned short* key = (const unsigned short*)actionKey(profileBindings, mode, action);
         hasPadBinding = isPadKey(key[0]) || isPadKey(key[1]);
      }
      if (hasPadBinding || modeCounts[mode] == 0) continue;

      for (int i = 0; i < modeCounts[mode]; i++) {
         int action = modeBindings[mode][i].processedAction;
         if (action < 0 || action >= kActionsPerMode) continue;
         uintptr_t entry = actionKey(profileBindings, mode, action);
         unsigned short* key = (unsigned short*)entry;
         unsigned char* device = (unsigned char*)(entry + 4);
         // Free slot first; with both taken, slot 1 as the defaults do.
         int slot = (key[1] == 0) ? 1 : (key[0] == 0) ? 0 : 1;
         key[slot] = (unsigned short)((modeBindings[mode][i].rawInput + 1) << 8);
         device[slot] = 0;
      }
      profileChanged = true;
      if (seededModes[0]) strcat_s(seededModes, ", ");
      strcat_s(seededModes, s_modeSectionNames[mode] + strlen("Controller."));
   }
   if (seededModes[0] && g_log)
      g_log("[Controller] No pad bindings in: %s. Added the default pad layout there\n", seededModes);

   // Same calls the controls screen makes after a rebind: profile -> live
   // tables, then mark the profile changed so it is saved.
   if (profileChanged && g_addr->joystick_sync && g_addr->joystick_discover) {
      auto applySettings = (fn_joystick_sync)resolve(exe_base, g_addr->joystick_sync);
      auto updateProfCrc = (fn_joystick_discover)resolve(exe_base, g_addr->joystick_discover);
      applySettings(joyConfig, nullptr);
      updateProfCrc(joyConfig, nullptr);
   }
}
