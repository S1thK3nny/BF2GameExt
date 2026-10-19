#pragma once

#include "pch.h"

// =============================================================================
// Controller Support -- pad bindings
// =============================================================================
// BF2 (2005) has a complete controller input pipeline from its console port, but
// the PC game gives each action only two keys, shared by keyboard and pad. This
// module adds a third, pad-only slot per action, kept per profile in
// SaveGames\<profile>.padbinds and edited on the game's own controls screen.

// ---------------------------------------------------------------------------
// eRAWINPUTS_CONTROLLER -- raw input IDs (binding table keys)
// From PDB: Battlefront2.pdb (Pandemic Studios)
// ---------------------------------------------------------------------------

enum eRAWINPUTS_CONTROLLER : int {
   eCONTROLLERINPUT_NONE = -1,

   // DirectInput joystick buttons (0-31)
   eCONTROLLERINPUT_BUTTON0  = 0,   // Xbox: A
   eCONTROLLERINPUT_BUTTON1  = 1,   // Xbox: B
   eCONTROLLERINPUT_BUTTON2  = 2,   // Xbox: X
   eCONTROLLERINPUT_BUTTON3  = 3,   // Xbox: Y
   eCONTROLLERINPUT_BUTTON4  = 4,   // Xbox: LB
   eCONTROLLERINPUT_BUTTON5  = 5,   // Xbox: RB
   eCONTROLLERINPUT_BUTTON6  = 6,   // Xbox: Back/View
   eCONTROLLERINPUT_BUTTON7  = 7,   // Xbox: Start/Menu
   eCONTROLLERINPUT_BUTTON8  = 8,   // Xbox: L3 (left stick click)
   eCONTROLLERINPUT_BUTTON9  = 9,   // Xbox: R3 (right stick click)
   eCONTROLLERINPUT_BUTTON10 = 10,
   eCONTROLLERINPUT_BUTTON11 = 11,
   eCONTROLLERINPUT_BUTTON12 = 12,
   eCONTROLLERINPUT_BUTTON13 = 13,
   eCONTROLLERINPUT_BUTTON14 = 14,
   eCONTROLLERINPUT_BUTTON15 = 15,

   // POV hat directions (d-pad via DirectInput)
   eCONTROLLERINPUT_HAT0_UP    = 32,
   eCONTROLLERINPUT_HAT0_RIGHT = 33,
   eCONTROLLERINPUT_HAT0_DOWN  = 34,
   eCONTROLLERINPUT_HAT0_LEFT  = 35,

   // DI axes as positive/negative half-axes
   eCONTROLLERINPUT_X_POS  = 48,  eCONTROLLERINPUT_X_NEG  = 49,  // L stick X
   eCONTROLLERINPUT_Y_POS  = 50,  eCONTROLLERINPUT_Y_NEG  = 51,  // L stick Y
   eCONTROLLERINPUT_Z_POS  = 52,  eCONTROLLERINPUT_Z_NEG  = 53,  // Triggers (combined on DI)
   eCONTROLLERINPUT_RX_POS = 54,  eCONTROLLERINPUT_RX_NEG = 55,  // R stick X
   eCONTROLLERINPUT_RY_POS = 56,  eCONTROLLERINPUT_RY_NEG = 57,  // R stick Y
   eCONTROLLERINPUT_RZ_POS = 58,  eCONTROLLERINPUT_RZ_NEG = 59,  // R trigger (some drivers)

   eCONTROLLERINPUT_MAX = 76,
};

// ---------------------------------------------------------------------------
// ePROCESSEDINPUT_TYPE -- gameplay action IDs (binding table values)
// ---------------------------------------------------------------------------

enum ePROCESSEDINPUT_TYPE : int {
   ePROCESSEDINPUT_NONE = -1,

   // Boolean inputs
   ePROCESSEDINPUT_primaryFireButtonDown      = 0,
   ePROCESSEDINPUT_secondaryFireButtonDown    = 1,
   ePROCESSEDINPUT_sprintButtonDown           = 2,
   ePROCESSEDINPUT_jumpButtonDown             = 3,
   ePROCESSEDINPUT_crouchButtonDown           = 4,
   ePROCESSEDINPUT_zoomButtonDown             = 5,
   ePROCESSEDINPUT_viewButtonDown             = 6,   // 1st/3rd person toggle
   ePROCESSEDINPUT_reloadButtonDown           = 7,
   ePROCESSEDINPUT_useButtonPressed           = 8,   // enter/exit vehicle
   ePROCESSEDINPUT_squadCommandButtonPressed  = 9,
   ePROCESSEDINPUT_acceptHeroPressed          = 10,
   ePROCESSEDINPUT_declineHeroPressed         = 11,
   ePROCESSEDINPUT_lockTargetButtonPressed    = 12,
   ePROCESSEDINPUT_primaryNextButtonPressed   = 13,
   ePROCESSEDINPUT_primaryPrevButtonPressed   = 14,
   ePROCESSEDINPUT_secondaryNextButtonPressed = 15,
   ePROCESSEDINPUT_secondaryPrevButtonPressed = 16,
   ePROCESSEDINPUT_playerList                 = 17,

   // Extended boolean inputs (18-29)
   ePROCESSEDINPUT_talk                       = 18,
   ePROCESSEDINPUT_teamTalk                   = 19,
   ePROCESSEDINPUT_commSpotted                = 20,   // F5-F12 communication slots
   ePROCESSEDINPUT_commMedic                  = 21,
   ePROCESSEDINPUT_commRepair                 = 22,
   ePROCESSEDINPUT_commAmmo                   = 23,
   ePROCESSEDINPUT_commPickup                 = 24,
   ePROCESSEDINPUT_commBackup                 = 25,
   ePROCESSEDINPUT_commAttack                 = 26,
   ePROCESSEDINPUT_commDefend                 = 27,
   ePROCESSEDINPUT_map                        = 28,
   ePROCESSEDINPUT_rollButtonDown             = 29,  // separate from crouch!

   // Axis inputs
   ePROCESSEDINPUT_STRAFE_AXIS = 30,
   ePROCESSEDINPUT_MOVE_AXIS   = 31,
   ePROCESSEDINPUT_TURN_AXIS   = 32,
   ePROCESSEDINPUT_PITCH_AXIS  = 33,

   // Half-axis inputs (for digital->axis mapping)
   ePROCESSEDINPUT_STRAFE_POS = 35, ePROCESSEDINPUT_STRAFE_NEG = 36,
   ePROCESSEDINPUT_MOVE_POS   = 37, ePROCESSEDINPUT_MOVE_NEG   = 38,
   ePROCESSEDINPUT_TURN_POS   = 39, ePROCESSEDINPUT_TURN_NEG   = 40,
   ePROCESSEDINPUT_PITCH_POS  = 41, ePROCESSEDINPUT_PITCH_NEG  = 42,
};

// ---------------------------------------------------------------------------
// Control modes (matches game's internal mode index)
// ---------------------------------------------------------------------------

enum eControlMode : int {
   CONTROL_MODE_INFANTRY = 0,
   CONTROL_MODE_VEHICLE  = 1,
   CONTROL_MODE_FLYER    = 2,
   CONTROL_MODE_JEDI     = 3,
   CONTROL_MODE_TURRET   = 4,
   CONTROL_MODE_COUNT    = 5,
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Hook the input loop and the controls screen for the pad slot. All builds.
void controller_bindings_install(uintptr_t exe_base);
void controller_bindings_uninstall();

// Switch the engine's pad input on for the current profile. Call on every state
// init, after the input system is up.
void controller_setup_bindings(uintptr_t exe_base);

// String-to-enum lookups for .padbinds names.
// Returns -2 on unknown string (distinct from NONE=-1).
int controller_raw_input_from_name(const char* name);
int controller_action_from_name(const char* name);
const char* controller_action_to_name(int action);

// Global enable flag (set from INI)
extern bool g_controllerEnabled;
