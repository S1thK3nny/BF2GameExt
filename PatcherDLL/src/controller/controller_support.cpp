#include "pch.h"
#include "controller_support.hpp"
#include "core/resolve.hpp"
#include "lua/lua_hooks.hpp"
#include "util/install_log.hpp"

#include <detours.h>
#include <stdio.h>
#include <wchar.h>

bool g_controllerEnabled = false;

// ---------------------------------------------------------------------------
// GameLog
// ---------------------------------------------------------------------------

static GameLog_t g_log = nullptr;

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

// Section names in a profile's .padbinds file.
static const char* s_modeSectionNames[CONTROL_MODE_COUNT] = {
   "Unit",
   "Vehicle",
   "Flyer",
   "Hero",
   "Turret",
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
   { "Chat",           ePROCESSEDINPUT_talk },
   { "TeamChat",       ePROCESSEDINPUT_teamTalk },
   { "CommSpotted",    ePROCESSEDINPUT_commSpotted },
   { "CommMedic",      ePROCESSEDINPUT_commMedic },
   { "CommRepair",     ePROCESSEDINPUT_commRepair },
   { "CommAmmo",       ePROCESSEDINPUT_commAmmo },
   { "CommPickup",     ePROCESSEDINPUT_commPickup },
   { "CommBackup",     ePROCESSEDINPUT_commBackup },
   { "CommAttack",     ePROCESSEDINPUT_commAttack },
   { "CommDefend",     ePROCESSEDINPUT_commDefend },
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
// Pad bindings: a third binding slot owned by GameExt
// ---------------------------------------------------------------------------
//
// The engine gives each action two keys (ActionKey: ushort mKey[2];
// uchar mDevice[2]), and keyboard and pad share them. Pad bindings live here
// instead, per profile, in SaveGames\<profile>.padbinds, so they never cost a
// keyboard key and GameExt never writes the profile's own bindings.
//
//  - StandardInputProcess runs a second pass with the pad keys in the live row.
//    ProcessedInputs::Set merges (axes keep the larger value, buttons only set
//    bits), so the passes combine like extra slots.
//  - The controls screen shows them after the keyboard keys, and a pad input
//    pressed there is stored here with the profile row left as it was.
//  - Restore Defaults also restores the mode's pad bindings.

static constexpr int kActionsPerMode = 0x2B;
static constexpr int kModeStride     = 0x102;   // ActionKey[0x2B]
static constexpr int kActionKeySize  = 6;
static constexpr int kPadRawInputs   = 0x40;    // raw ids above are the mouse
static constexpr int kMaxPadBindings = 128;

// RawControllerInputs (controller_base_global + 0x428) and tProfile fields,
// identical on all three builds.
static constexpr int kCtrlMode       = 0x8;
static constexpr int kCtrlLiveRows   = 0x20BC;
static constexpr int kCtrlEdgeFlag   = 0x2600;  // 0xB7 key edge pair
static constexpr int kProfileName    = 0x2;     // ushort mName[16]
static constexpr int kProfileRows    = 0x4A;    // mGameOptions.mKeyboardBindings
static constexpr int kProfilePadOn   = 0xF94;   // joystick input enable

static constexpr int kLuaGlobalsIndex = -10001;

struct PadBinding {
   uint8_t raw;
   uint8_t device;
   uint8_t action;
};

static PadBinding s_pad[CONTROL_MODE_COUNT][kMaxPadBindings];
static int        s_padCount[CONTROL_MODE_COUNT] = {};
static wchar_t    s_padProfile[17] = {};   // profile the store holds
static bool       s_padLoaded = false;

static uintptr_t s_profile    = 0;   // current tProfile
static uintptr_t s_ctrl       = 0;   // RawControllerInputs for player 0
static int*      s_numPads    = nullptr;
static int*      s_rowActions = nullptr;   // [5][0x2B]
static float*    s_holdTimer  = nullptr;

using fn_profile_call_t = void(__fastcall*)(uintptr_t ecx, void* edx);
static fn_profile_call_t s_applySettings = nullptr;   // tProfile::ApplySettings
static fn_profile_call_t s_updateProfCrc = nullptr;   // tProfile::UpdateProfCRC

static unsigned short pad_key(int raw) { return (unsigned short)((raw + 1) << 8); }

static bool is_pad_key(unsigned short key)
{
   return key >= 0x100 && ((key >> 8) - 1) < kPadRawInputs;
}

// Name an input the way the sidecar and the screen write it.
static const char* input_name(int raw, char* buf, size_t size)
{
   if (raw == eCONTROLLERINPUT_Z_POS) return "LT";
   if (raw == eCONTROLLERINPUT_Z_NEG) return "RT";
   for (const NamedValue* p = s_rawInputNames; p->name; ++p)
      if (p->value == raw) return p->name;
   sprintf_s(buf, size, "Raw%d", raw);
   return buf;
}

// "A", "Pad2.LT", "Raw12". Returns false for an unknown name.
static bool parse_input(const char* text, int* raw, int* device)
{
   *device = 0;
   if (_strnicmp(text, "Pad", 3) == 0 && text[3] >= '1' && text[3] <= '9' && text[4] == '.') {
      *device = text[3] - '1';
      text += 5;
   }
   int value = controller_raw_input_from_name(text);
   if (value == -2 && _strnicmp(text, "Raw", 3) == 0) value = atoi(text + 3);
   if (value < 0 || value >= kPadRawInputs) return false;
   *raw = value;
   return true;
}

static void pad_add(int mode, int raw, int device, int action)
{
   if (s_padCount[mode] >= kMaxPadBindings) return;
   for (int i = 0; i < s_padCount[mode]; i++) {
      const PadBinding& b = s_pad[mode][i];
      if (b.raw == raw && b.device == device && b.action == action) return;
   }
   s_pad[mode][s_padCount[mode]++] = { (uint8_t)raw, (uint8_t)device, (uint8_t)action };
}

static void pad_load_defaults(int mode)
{
   s_padCount[mode] = 0;
   for (const ModeBindingDef* d = s_modeDefaults[mode]; d->inputName; ++d) {
      int raw = controller_raw_input_from_name(d->inputName);
      if (raw < 0 || raw >= kPadRawInputs) continue;
      char buf[256];
      strncpy_s(buf, d->defaultActions, _TRUNCATE);
      char* context = nullptr;
      for (char* tok = strtok_s(buf, ",", &context); tok; tok = strtok_s(nullptr, ",", &context)) {
         int action = controller_action_from_name(tok);
         if (action >= 0 && action < kActionsPerMode) pad_add(mode, raw, 0, action);
      }
   }
}

static void sidecar_path(const wchar_t* profile, wchar_t* out, size_t size)
{
   swprintf_s(out, size, L"SaveGames\\%s.padbinds", profile);
}

static void sidecar_save()
{
   if (!s_padProfile[0]) return;
   wchar_t path[MAX_PATH];
   sidecar_path(s_padProfile, path, MAX_PATH);
   FILE* f = nullptr;
   if (_wfopen_s(&f, path, L"wb") != 0 || !f) return;

   char profile[64] = {};
   WideCharToMultiByte(CP_UTF8, 0, s_padProfile, -1, profile, sizeof(profile), nullptr, nullptr);
   fprintf(f, "; BF2GameExt pad bindings for profile \"%s\".\r\n", profile);
   fprintf(f, "; Rebind in Options -> Controls, or edit here while the game is closed.\r\n");
   fprintf(f, "; Input=Action[,Action...]. Names: docs/user/CONTROLLER.md\r\n");

   for (int mode = 0; mode < CONTROL_MODE_COUNT; mode++) {
      fprintf(f, "\r\n[%s]\r\n", s_modeSectionNames[mode]);
      bool written[kMaxPadBindings] = {};
      for (int i = 0; i < s_padCount[mode]; i++) {
         if (written[i]) continue;
         const PadBinding& b = s_pad[mode][i];
         char nameBuf[16];
         if (b.device) fprintf(f, "Pad%d.", b.device + 1);
         fprintf(f, "%s=%s", input_name(b.raw, nameBuf, sizeof(nameBuf)), controller_action_to_name(b.action));
         for (int j = i + 1; j < s_padCount[mode]; j++) {
            const PadBinding& o = s_pad[mode][j];
            if (o.raw != b.raw || o.device != b.device) continue;
            fprintf(f, ",%s", controller_action_to_name(o.action));
            written[j] = true;
         }
         fprintf(f, "\r\n");
      }
   }
   fclose(f);
}

static bool sidecar_load()
{
   wchar_t path[MAX_PATH];
   sidecar_path(s_padProfile, path, MAX_PATH);
   FILE* f = nullptr;
   if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;

   for (int mode = 0; mode < CONTROL_MODE_COUNT; mode++) s_padCount[mode] = 0;

   int mode = -1;
   char line[512];
   while (fgets(line, sizeof(line), f)) {
      char* s = line;
      while (*s == ' ' || *s == '\t') s++;
      char* end = s + strlen(s);
      while (end > s && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
      if (!*s || *s == ';') continue;

      if (*s == '[') {
         char* close = strchr(s, ']');
         if (close) *close = 0;
         mode = -1;
         for (int m = 0; m < CONTROL_MODE_COUNT; m++)
            if (_stricmp(s + 1, s_modeSectionNames[m]) == 0) mode = m;
         continue;
      }

      char* eq = strchr(s, '=');
      if (mode < 0 || !eq) continue;
      *eq = 0;
      char* key = s;
      char* keyEnd = eq;
      while (keyEnd > key && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) *--keyEnd = 0;

      int raw, device;
      if (!parse_input(key, &raw, &device)) {
         if (g_log) g_log("[Controller] .padbinds: unknown input '%s'\n", key);
         continue;
      }
      char* context = nullptr;
      for (char* tok = strtok_s(eq + 1, ",", &context); tok; tok = strtok_s(nullptr, ",", &context)) {
         while (*tok == ' ' || *tok == '\t') tok++;
         char* tend = tok + strlen(tok);
         while (tend > tok && (tend[-1] == ' ' || tend[-1] == '\t')) *--tend = 0;
         if (!*tok) continue;
         int action = controller_action_from_name(tok);
         if (action >= 0 && action < kActionsPerMode) pad_add(mode, raw, device, action);
         else if (action == -2 && g_log) g_log("[Controller] .padbinds: unknown action '%s'\n", tok);
      }
   }
   fclose(f);
   return true;
}

// Follow the current profile. A profile without a sidecar starts from the
// built-in layout, written out so it can be found and edited.
static void pad_sync_profile()
{
   wchar_t current[17] = {};
   wcsncpy_s(current, (const wchar_t*)(s_profile + kProfileName), 16);
   if (s_padLoaded && wcscmp(current, s_padProfile) == 0) return;

   s_padLoaded = true;
   wcscpy_s(s_padProfile, current);
   if (current[0] && sidecar_load()) return;

   for (int mode = 0; mode < CONTROL_MODE_COUNT; mode++) pad_load_defaults(mode);
   if (current[0]) {
      sidecar_save();
      install_log("[Controller] Created the profile's .padbinds from the default pad layout");
   }
}

static bool pad_connected() { return s_numPads && *s_numPads > 0; }

// --- Input: second pass with the pad keys in the live row ---

using fn_standard_input_t = void(__fastcall*)(void* self, void* edx);
static fn_standard_input_t s_origStandardInput = nullptr;

static void __fastcall hooked_StandardInput(void* self, void* edx)
{
   s_origStandardInput(self, edx);
   if (!pad_connected()) return;

   uintptr_t ctrl = (uintptr_t)self;
   int mode = *(int*)(ctrl + kCtrlMode);
   if (mode < 0 || mode >= CONTROL_MODE_COUNT) return;
   pad_sync_profile();
   if (!s_padCount[mode]) return;

   unsigned char* row = (unsigned char*)(ctrl + kCtrlLiveRows + mode * kModeStride);
   unsigned char savedRow[kModeStride];
   memcpy(savedRow, row, kModeStride);
   unsigned char edge0 = *(unsigned char*)(ctrl + kCtrlEdgeFlag);
   unsigned char edge1 = *(unsigned char*)(ctrl + kCtrlEdgeFlag + 1);

   // Two pad keys per action per pass; more need another pass.
   bool placed[kMaxPadBindings] = {};
   int remaining = s_padCount[mode];
   while (remaining > 0) {
      memset(row, 0, kModeStride);
      for (int i = 0; i < s_padCount[mode]; i++) {
         if (placed[i]) continue;
         const PadBinding& b = s_pad[mode][i];
         unsigned char* entry = row + b.action * kActionKeySize;
         unsigned short* key = (unsigned short*)entry;
         int slot = key[0] == 0 ? 0 : key[1] == 0 ? 1 : -1;
         if (slot < 0) continue;
         key[slot] = pad_key(b.raw);
         entry[4 + slot] = b.device;
         placed[i] = true;
         remaining--;
      }
      s_origStandardInput(self, edx);
   }

   memcpy(row, savedRow, kModeStride);
   *(unsigned char*)(ctrl + kCtrlEdgeFlag)     = edge0;
   *(unsigned char*)(ctrl + kCtrlEdgeFlag + 1) = edge1;
}

// --- Controls screen ---

static int screen_mode()
{
   int mode = *(int*)(s_ctrl + kCtrlMode);
   return (mode >= 0 && mode < CONTROL_MODE_COUNT) ? mode : -1;
}

// Same row -> action walk SetBinding and GetKeyBoardCmds do.
static int row_action(int mode, int row)
{
   for (int i = 0; i < kActionsPerMode; i++) {
      int action = s_rowActions[mode * kActionsPerMode + i];
      if (action == -1 || action == 0x22) continue;
      if (--row == 0) return action;
   }
   return -1;
}

static int pad_text(int mode, int action, wchar_t* out, int size)
{
   int len = 0;
   out[0] = 0;
   for (int i = 0; i < s_padCount[mode]; i++) {
      const PadBinding& b = s_pad[mode][i];
      if (b.action != action) continue;
      char nameBuf[16];
      char text[48];
      if (b.device) sprintf_s(text, "PAD%d %s", b.device + 1, input_name(b.raw, nameBuf, sizeof(nameBuf)));
      else          sprintf_s(text, "PAD %s", input_name(b.raw, nameBuf, sizeof(nameBuf)));
      _strupr_s(text);
      len += swprintf_s(out + len, size - len, L"%s%S", len ? L", " : L"", text);
   }
   return len;
}

static lua_CFunction s_origGetKeyBoardCmds = nullptr;

static int __cdecl hooked_GetKeyBoardCmds(lua_State* L)
{
   int r = s_origGetKeyBoardCmds(L);
   int mode = screen_mode();
   if (mode < 0) return r;
   pad_sync_profile();

   int top = g_lua.gettop(L);
   static const char kList[] = "ifs_opt_pckeyboard_listbox_contents";
   g_lua.pushlstring(L, kList, sizeof(kList) - 1);
   g_lua.gettable(L, kLuaGlobalsIndex);
   int list = g_lua.gettop(L);

   const unsigned short* profileRow = (const unsigned short*)(s_profile + kProfileRows + mode * kModeStride);
   int row = 0;
   for (int i = 0; i < kActionsPerMode; i++) {
      int action = s_rowActions[mode * kActionsPerMode + i];
      if (action == -1 || action == 0x22) continue;
      ++row;

      wchar_t pad[128];
      int padLen = pad_text(mode, action, pad, 128);
      if (!padLen) continue;

      g_lua.rawgeti(L, list, row);
      int item = g_lua.gettop(L);
      g_lua.pushlstring(L, "keyStr", 6);
      g_lua.gettable(L, item);
      // The entry is lua_tostring on Steam/GOG (no length out) and
      // luaL_checklstring on modtools, so measure the terminated UTF-16 text.
      const wchar_t* stock = (const wchar_t*)g_lua.tolstring(L, -1, nullptr);

      // A row with no keyboard key shows the "unbound" word; replace it.
      const unsigned short* keys = profileRow + action * (kActionKeySize / 2);
      bool hasStockKey = keys[0] || keys[1];
      wchar_t text[384];
      int len = 0;
      if (hasStockKey && stock) {
         int stockLen = (int)wcsnlen(stock, 300);
         wmemcpy(text, stock, stockLen);
         len = stockLen;
         len += swprintf_s(text + len, 384 - len, L", ");
      }
      len += swprintf_s(text + len, 384 - len, L"%s", pad);
      g_lua.settop(L, item);

      g_lua.pushlstring(L, "keyStr", 6);
      g_lua.pushlstring(L, (const char*)text, (len + 1) * sizeof(wchar_t));
      g_lua.settable(L, item);
      g_lua.settop(L, list);
   }
   g_lua.settop(L, top);
   return r;
}

static lua_CFunction s_origSetBinding = nullptr;

static int __cdecl hooked_SetBinding(lua_State* L)
{
   int mode = screen_mode();
   int action = -1;
   float dt = 0.0f;
   if (mode >= 0) {
      action = row_action(mode, (int)g_lua.tonumber(L, 1));
      if (g_lua.gettop(L) >= 2) dt = g_lua.tonumber(L, 2);
   }
   if (action < 0 || action >= kActionsPerMode) return s_origSetBinding(L);

   unsigned char* profileRow = (unsigned char*)(s_profile + kProfileRows + mode * kModeStride);
   unsigned char* liveRow    = (unsigned char*)(s_ctrl + kCtrlLiveRows + mode * kModeStride);
   unsigned char savedProfile[kModeStride], savedLive[kModeStride];
   memcpy(savedProfile, profileRow, kModeStride);
   memcpy(savedLive, liveRow, kModeStride);
   float timer = *s_holdTimer;

   int r = s_origSetBinding(L);

   pad_sync_profile();
   const unsigned char* before = savedProfile + action * kActionKeySize;
   const unsigned char* after  = profileRow + action * kActionKeySize;

   for (int slot = 0; slot < 2; slot++) {
      unsigned short key = ((const unsigned short*)after)[slot];
      bool changed = key != ((const unsigned short*)before)[slot] || after[4 + slot] != before[4 + slot];
      if (!changed || !is_pad_key(key)) continue;

      // A pad input: it goes to the pad slot, the profile stays as it was.
      int raw = (key >> 8) - 1;
      int device = after[4 + slot];
      memcpy(profileRow, savedProfile, kModeStride);
      memcpy(liveRow, savedLive, kModeStride);
      if (s_applySettings) s_applySettings(s_profile, nullptr);
      if (s_updateProfCrc) s_updateProfCrc(s_profile, nullptr);

      // One action per pad input and one pad input per action, as the
      // screen does for keys.
      char nameBuf[16];
      const char* input = input_name(raw, nameBuf, sizeof(nameBuf));
      int n = 0;
      for (int i = 0; i < s_padCount[mode]; i++) {
         const PadBinding& b = s_pad[mode][i];
         if (b.action == action || (b.raw == raw && b.device == device)) {
            if (b.action != action)
               install_log("[Controller] %s: %s no longer %s", s_modeSectionNames[mode], input,
                           controller_action_to_name(b.action));
            continue;
         }
         s_pad[mode][n++] = b;
      }
      s_padCount[mode] = n;
      pad_add(mode, raw, device, action);
      sidecar_save();
      install_log("[Controller] %s: %s%s bound to %s", s_modeSectionNames[mode],
                  device ? "second pad " : "", input, controller_action_to_name(action));
      return r;
   }

   // Escape held to the end clears the action, the pad slot with it.
   float held = (timer == 0.0f ? 0.0001f : timer) + dt;
   const unsigned short* afterKeys = (const unsigned short*)after;
   if (r == 1 && held >= 1.0f && !afterKeys[0] && !afterKeys[1]) {
      int n = 0;
      for (int i = 0; i < s_padCount[mode]; i++)
         if (s_pad[mode][i].action != action) s_pad[mode][n++] = s_pad[mode][i];
      if (n != s_padCount[mode]) {
         s_padCount[mode] = n;
         sidecar_save();
         install_log("[Controller] %s: pad cleared from %s", s_modeSectionNames[mode],
                     controller_action_to_name(action));
      }
   }
   return r;
}

static lua_CFunction s_origResetControls = nullptr;

static int __cdecl hooked_ResetControls(lua_State* L)
{
   int r = s_origResetControls(L);
   int mode = screen_mode();
   if (mode >= 0) {
      pad_sync_profile();
      pad_load_defaults(mode);
      sidecar_save();
      install_log("[Controller] %s: pad bindings restored to defaults", s_modeSectionNames[mode]);
   }
   return r;
}

// Rows the stock screen hides in a mode (-1) but the built-in pad layout
// binds, so they can be seen and rebound. Labels exist for every action.
static void show_pad_rows(int* table)
{
   for (int mode = 0; mode < CONTROL_MODE_COUNT; mode++) {
      int* rows = table + mode * kActionsPerMode;
      PadBinding defaults[kMaxPadBindings];
      pad_load_defaults(mode);
      int count = s_padCount[mode];
      memcpy(defaults, s_pad[mode], count * sizeof(PadBinding));

      for (int i = 0; i < count; i++) {
         int action = defaults[i].action;
         bool listed = false;
         for (int k = 0; k < kActionsPerMode; k++) listed |= rows[k] == action;
         if (listed) continue;
         for (int k = 0; k < kActionsPerMode; k++) {
            if (rows[k] != -1) continue;
            protected_write(&rows[k], &action, sizeof(action));
            break;
         }
      }
      s_padCount[mode] = 0;
   }
   s_padLoaded = false;
}

// ---------------------------------------------------------------------------
// Install / per-state setup
// ---------------------------------------------------------------------------

template <typename T>
static void attach(T& original, uintptr_t addr, void* hook, uintptr_t exe_base)
{
   original = (T)resolve(exe_base, addr);
   DetourAttach(&(PVOID&)original, hook);
}

void controller_bindings_install(uintptr_t exe_base)
{
   if (!g_controllerEnabled) return;
   if (!g_addr->standard_input_process || !g_addr->script_cb_set_binding ||
       !g_addr->script_cb_get_keyboard_cmds || !g_addr->script_cb_reset_controls ||
       !g_addr->controls_row_actions || !g_addr->set_binding_hold_timer ||
       !g_addr->joystick_config_base || !g_addr->controller_base_global)
      return;

   s_profile    = (uintptr_t)resolve(exe_base, g_addr->joystick_config_base);
   s_ctrl       = (uintptr_t)resolve(exe_base, g_addr->controller_base_global) + 0x428;
   s_numPads    = (int*)resolve(exe_base, g_addr->num_joysticks_global);
   s_rowActions = (int*)resolve(exe_base, g_addr->controls_row_actions);
   s_holdTimer  = (float*)resolve(exe_base, g_addr->set_binding_hold_timer);
   if (g_addr->joystick_sync)     s_applySettings = (fn_profile_call_t)resolve(exe_base, g_addr->joystick_sync);
   if (g_addr->joystick_discover) s_updateProfCrc = (fn_profile_call_t)resolve(exe_base, g_addr->joystick_discover);

   show_pad_rows(s_rowActions);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   attach(s_origStandardInput,   g_addr->standard_input_process,      hooked_StandardInput,   exe_base);
   attach(s_origSetBinding,      g_addr->script_cb_set_binding,       hooked_SetBinding,      exe_base);
   attach(s_origGetKeyBoardCmds, g_addr->script_cb_get_keyboard_cmds, hooked_GetKeyBoardCmds, exe_base);
   attach(s_origResetControls,   g_addr->script_cb_reset_controls,    hooked_ResetControls,   exe_base);
   if (DetourTransactionCommit() != NO_ERROR) {
      s_origStandardInput = nullptr;
      s_origSetBinding = s_origGetKeyBoardCmds = s_origResetControls = nullptr;
      install_log("[Controller] pad binding hooks failed to install");
      return;
   }
   install_log("[Controller] pad bindings installed (SaveGames\\<profile>.padbinds)");
}

void controller_bindings_uninstall()
{
   if (!s_origStandardInput) return;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)s_origStandardInput,   hooked_StandardInput);
   DetourDetach(&(PVOID&)s_origSetBinding,      hooked_SetBinding);
   DetourDetach(&(PVOID&)s_origGetKeyBoardCmds, hooked_GetKeyBoardCmds);
   DetourDetach(&(PVOID&)s_origResetControls,   hooked_ResetControls);
   DetourTransactionCommit();
   s_origStandardInput = nullptr;
}

void controller_setup_bindings(uintptr_t exe_base)
{
   if (!g_controllerEnabled) return;
   g_log = get_gamelog();
   (void)exe_base;
   if (!s_profile || !pad_connected()) return;

   // The engine ignores pads until this is set.
   if (*(char*)(s_profile + kProfilePadOn) == 0) {
      *(char*)(s_profile + kProfilePadOn) = 1;
      if (s_applySettings) s_applySettings(s_profile, nullptr);
      if (s_updateProfCrc) s_updateProfCrc(s_profile, nullptr);
   }
}
