#include "pch.h"

#include "spawn_screen_patch.hpp"
#include "cfile.hpp"
#include <detours.h>
#include <cstring>

// ============================================================================
// Xbox Spawn Screen Port
//
// PC uses a single combined spawn screen (ifs_pc_SpawnSelect) for both
// class selection and spawn point selection (modes 2 and 3 both push it).
//
// Xbox uses two separate screens:
//   Mode 2 → ifs_mapselect  (spawn point selection)
//   Mode 3 → ifs_charselect (character class selection)
//
// Two hooks:
//   1. ChangeMode  — mode 3 pushes charselect instead of PC screen
//   2. pcUpdate    — when on charselect, writes selected class to Xbox panel
// ============================================================================

// ---------------------------------------------------------------------------
// Per-build addresses (offsets from exe_base)
// ---------------------------------------------------------------------------
struct spawn_screen_addrs {
   uintptr_t id_rva;
   uint64_t  id_expected;

   uintptr_t change_mode;           // SpawnDisplay::ChangeMode
   uintptr_t update_object_text;    // SpawnDisplay::UpdateObjectText
   uintptr_t set_slot_info;         // SpawnDisplay::SetSlotInfo (UpdateClassTitle)
   uintptr_t set_entity_element;    // SpawnDisplay::SetEntityElement (3D model)
   uintptr_t find_element;          // RedInterfaceElement::Find

   uintptr_t ifscreenmgr_ptr;       // IFScreenManager* global (ptr-to-ptr)
   uintptr_t ifscreenmgr_push;      // IFScreenManager::Push
   uintptr_t ifscreenmgr_pop;       // IFScreenManager::Pop
   uintptr_t ifscreenmgr_is_active; // IFScreenManager::IsActive

   uintptr_t spawn_mgr_teams;       // SpawnManager team table global (ptr-to-array)
   uintptr_t team_index_off;        // SpawnDisplay offset to team index
};

static const spawn_screen_addrs MODTOOLS = {
   .id_rva              = 0x62b59c,
   .id_expected         = 0x746163696c707041,  // "Applicat"
   .change_mode         = 0x0028b430,          // VA 0x0068b430
   .update_object_text  = 0x00289eb0,          // VA 0x00689eb0
   .set_slot_info       = 0x002897e0,          // VA 0x006897e0
   .set_entity_element  = 0x0000349f,          // VA 0x0040349f
   .find_element        = 0x00429140,          // VA 0x00829140
   .ifscreenmgr_ptr     = 0x007a3888,          // VA 0x00ba3888
   .ifscreenmgr_push    = 0x00010320,          // VA 0x00410320
   .ifscreenmgr_pop     = 0x0000a1fa,          // VA 0x0040a1fa
   .ifscreenmgr_is_active = 0x00009593,        // VA 0x00409593
   .spawn_mgr_teams     = 0x006d5d64,          // VA 0x00ad5d64
   .team_index_off      = 0x2090,
};

static const spawn_screen_addrs STEAM = {
   .id_rva              = 0x39f834,
   .id_expected         = 0x746163696c707041,
   .change_mode         = 0x0002ba40,          // VA 0x0042ba40
   .update_object_text  = 0,                   // TODO
   .set_slot_info       = 0,                   // TODO
   .set_entity_element  = 0,                   // TODO
   .find_element        = 0,                   // TODO
   .ifscreenmgr_ptr     = 0x01a2fd48,          // VA 0x01e2fd48
   .ifscreenmgr_push    = 0x00128660,          // VA 0x00528660
   .ifscreenmgr_pop     = 0x001286e0,          // VA 0x005286e0
   .ifscreenmgr_is_active = 0x001287e0,        // VA 0x005287e0
   .spawn_mgr_teams     = 0,                   // TODO
   .team_index_off      = 0x2054,
};

static const spawn_screen_addrs GOG = {
   .id_rva              = 0x3a0698,
   .id_expected         = 0x746163696c707041,
   .change_mode         = 0x0002ba00,          // VA 0x0042ba00
   .update_object_text  = 0,                   // TODO
   .set_slot_info       = 0,                   // TODO
   .set_entity_element  = 0,                   // TODO
   .find_element        = 0,                   // TODO
   .ifscreenmgr_ptr     = 0x01a311e8,          // VA 0x01e311e8
   .ifscreenmgr_push    = 0x00128660,          // VA 0x00528660
   .ifscreenmgr_pop     = 0x001286e0,          // VA 0x005286e0
   .ifscreenmgr_is_active = 0x001287e0,        // VA 0x005287e0
   .spawn_mgr_teams     = 0,                   // TODO
   .team_index_off      = 0x2054,
};

// ---------------------------------------------------------------------------
// Function pointer types
// ---------------------------------------------------------------------------
typedef void  (__thiscall* fn_ChangeMode)(void* self, int mode);
typedef void  (__thiscall* fn_UpdateObjectText)(void* self, void* classPtr, int slotIdx);
typedef void  (__thiscall* fn_SetSlotInfo)(void* self, void* classPtr, int slotIdx);
typedef void  (__thiscall* fn_SetEntityElement)(void* self, void* classPtr);
typedef void* (__cdecl*    fn_FindElement)(const char* name);

typedef void  (__thiscall* fn_IFPush)(void* mgr, const char* name, int flags, int extra);
typedef void  (__thiscall* fn_IFPop)(void* mgr, int flags);
typedef bool  (__thiscall* fn_IFIsActive)(void* mgr, const char* name);

// ---------------------------------------------------------------------------
// Resolved state
// ---------------------------------------------------------------------------
static fn_ChangeMode        original_ChangeMode    = nullptr;
static fn_UpdateObjectText  s_updateObjectText     = nullptr;
static fn_SetSlotInfo       s_setSlotInfo          = nullptr;
static fn_SetEntityElement  s_setEntityElement     = nullptr;
static fn_FindElement       s_findElement          = nullptr;

static fn_IFPush            s_ifPush               = nullptr;
static fn_IFPop             s_ifPop                = nullptr;
static fn_IFIsActive        s_ifIsActive           = nullptr;
static void**               s_ifScreenMgrPtr       = nullptr;

static void**               s_spawnMgrTeams        = nullptr;
static uintptr_t            s_teamIndexOff         = 0;

// Cached charselect element pointers (resolved once on first use)
static void*                s_charBoxText          = nullptr;
static void*                s_charTitleBar         = nullptr;
static bool                 s_charElementsResolved = false;

// ---------------------------------------------------------------------------
// SpawnDisplay layout constants (identical all builds)
// ---------------------------------------------------------------------------
static constexpr uintptr_t OFF_INFO_TEXT  = 0x560;  // void*[10] InfoText elements
static constexpr uintptr_t OFF_TITLE_BAR = 0x588;  // void*[10] titleBar elements
static constexpr uintptr_t OFF_SELECTED  = 0x20A0; // int: selected class index

// Team struct offsets
static constexpr uintptr_t TEAM_CLASS_COUNT = 0x48;
static constexpr uintptr_t TEAM_CLASS_ARRAY = 0x50;

// ---------------------------------------------------------------------------
// Push a named screen through IFScreenManager
// ---------------------------------------------------------------------------
static void push_screen(const char* screenName)
{
   cfile log("BF2GameExt.log", "a");

   void* mgr = *s_ifScreenMgrPtr;
   if (!mgr) {
      log.printf("[SpawnScreen] push_screen(%s): mgr is null\n", screenName);
      return;
   }

   const char* current = (const char*)((uintptr_t)mgr + 0x34);
   log.printf("[SpawnScreen] push_screen(%s): current='%s'\n", screenName,
              (*current != '\0') ? current : "<empty>");

   if (strcmp(screenName, current) == 0) {
      log.printf("[SpawnScreen]   -> already on target, skipping\n");
      return;
   }

   if (strcmp("ifs_pausemenu", current) == 0) {
      log.printf("[SpawnScreen]   -> pause menu active, skipping\n");
      return;
   }

   if (s_ifIsActive(mgr, screenName)) {
      log.printf("[SpawnScreen]   -> IsActive returned true, skipping\n");
      return;
   }

   if (*current != '\0') {
      log.printf("[SpawnScreen]   -> popping current screen\n");
      s_ifPop(mgr, 0);
   }

   log.printf("[SpawnScreen]   -> pushing %s\n", screenName);
   s_ifPush(mgr, screenName, 0, 0);
}

// ---------------------------------------------------------------------------
// Check if the charselect screen is currently active
// ---------------------------------------------------------------------------
static bool is_on_charselect()
{
   void* mgr = *s_ifScreenMgrPtr;
   if (!mgr) return false;
   const char* current = (const char*)((uintptr_t)mgr + 0x34);
   return (*current != '\0') && strcmp(current, "ifs_charselect1") == 0;
}

// ---------------------------------------------------------------------------
// Resolve charselect element pointers (once)
// ---------------------------------------------------------------------------
static void resolve_charselect_elements()
{
   if (s_charElementsResolved) return;
   s_charElementsResolved = true;

   s_charBoxText  = s_findElement("ifs_charselect1.Info.Window.BoxText");
   s_charTitleBar = s_findElement("ifs_charselect1.Info.Window.titleBarElement");

   cfile log("BF2GameExt.log", "a");
   log.printf("[SpawnScreen] Resolved charselect elements: BoxText=%p, titleBar=%p\n",
              s_charBoxText, s_charTitleBar);
}

// ---------------------------------------------------------------------------
// Hide/show SpawnDisplay's embedded FLRenderer elements.
// These are added during Initialize and render regardless of the active
// IFScreen — they cause the PC map/UI overlay on charselect.
// ---------------------------------------------------------------------------
static void set_element_visible(void* elem, int visible)
{
   // vtable+0x4 = SetVisible, vtable+0x8 = SetSelectable
   (*(void (__thiscall**)(void*, int))(*(uintptr_t*)elem + 0x4))(elem, visible);
   (*(void (__thiscall**)(void*, int))(*(uintptr_t*)elem + 0x8))(elem, visible);
}

static bool s_elementsHidden = false;

static void hide_spawn_elements(void* self)
{
   if (s_elementsHidden) return;
   s_elementsHidden = true;

   // Embedded FLRenderer elements added during Initialize
   static const uintptr_t offsets[] = {0x1a0, 0x240, 0x2e0, 0x380, 0x420, 0x4c0};
   for (uintptr_t off : offsets)
      set_element_visible((void*)((uintptr_t)self + off), 0);

   // SoldierElement (ptr at +0x62C)
   void* soldier = *(void**)((uintptr_t)self + 0x62C);
   if (soldier)
      set_element_visible(soldier, 0);
}

static void show_spawn_elements(void* self)
{
   if (!s_elementsHidden) return;
   s_elementsHidden = false;

   static const uintptr_t offsets[] = {0x1a0, 0x240, 0x2e0, 0x380, 0x420, 0x4c0};
   for (uintptr_t off : offsets)
      set_element_visible((void*)((uintptr_t)self + off), 1);

   void* soldier = *(void**)((uintptr_t)self + 0x62C);
   if (soldier)
      set_element_visible(soldier, 1);
}

// ---------------------------------------------------------------------------
// Helper: get the Team pointer from SpawnDisplay
// ---------------------------------------------------------------------------
static void* get_team(void* self)
{
   int teamIdx = *(int*)((uintptr_t)self + s_teamIndexOff);
   return ((void**)(*s_spawnMgrTeams))[teamIdx];
}

// ---------------------------------------------------------------------------
// Update charselect panel with the currently selected class.
// Called every frame from ChangeMode when mode==3 and on charselect screen.
// ---------------------------------------------------------------------------
static void update_charselect(void* self)
{
   resolve_charselect_elements();
   if (!s_charBoxText && !s_charTitleBar) return;

   void* team = get_team(self);
   if (!team) return;

   int classCount = *(int*)((uintptr_t)team + TEAM_CLASS_COUNT);
   if (classCount <= 0) return;

   int selectedIdx = *(int*)((uintptr_t)self + OFF_SELECTED);
   if (selectedIdx < 0 || selectedIdx >= classCount)
      selectedIdx = 0;

   void** classArray = *(void***)((uintptr_t)team + TEAM_CLASS_ARRAY);
   void* classPtr = classArray[selectedIdx];
   if (!classPtr) return;

   // Temporarily swap slot 0 to charselect elements so UpdateObjectText
   // and SetSlotInfo write to them, then restore
   void* savedInfoText = *(void**)((uintptr_t)self + OFF_INFO_TEXT);
   void* savedTitleBar = *(void**)((uintptr_t)self + OFF_TITLE_BAR);

   if (s_charBoxText)
      *(void**)((uintptr_t)self + OFF_INFO_TEXT) = s_charBoxText;
   if (s_charTitleBar)
      *(void**)((uintptr_t)self + OFF_TITLE_BAR) = s_charTitleBar;

   s_updateObjectText(self, classPtr, 0);
   s_setSlotInfo(self, classPtr, 0);

   // Show elements and set full alpha
   if (s_charBoxText) {
      (*(void (__thiscall**)(void*, int))(*(uintptr_t*)s_charBoxText + 0x4))(s_charBoxText, 1);
      (*(void (__thiscall**)(void*, int))(*(uintptr_t*)s_charBoxText + 0x8))(s_charBoxText, 1);
      *(uint8_t*)((uintptr_t)s_charBoxText + 0x2F) = 0xFF;
   }
   if (s_charTitleBar) {
      (*(void (__thiscall**)(void*, int))(*(uintptr_t*)s_charTitleBar + 0x4))(s_charTitleBar, 1);
      (*(void (__thiscall**)(void*, int))(*(uintptr_t*)s_charTitleBar + 0x8))(s_charTitleBar, 1);
      *(uint8_t*)((uintptr_t)s_charTitleBar + 0x2F) = 0xFF;
   }

   // Restore original slot 0 pointers
   *(void**)((uintptr_t)self + OFF_INFO_TEXT) = savedInfoText;
   *(void**)((uintptr_t)self + OFF_TITLE_BAR) = savedTitleBar;

   // Update 3D soldier model for selected class
   s_setEntityElement(self, classPtr);
}

// ---------------------------------------------------------------------------
// ChangeMode hook
//
// Mode 2: pass through to original (PC spawn point selection works)
// Mode 3: don't call original (prevents PC element overlay), just push
//          charselect screen directly
// Others: pass through
// ---------------------------------------------------------------------------
static void __fastcall hooked_ChangeMode(void* self, void* /*edx*/, int mode)
{
   if (mode == 3) {
      if (is_on_charselect()) {
         // Already on charselect — ChangeMode fires every frame, so
         // use this as our update tick to populate the Xbox panel.
         hide_spawn_elements(self);
         update_charselect(self);
         return;
      }

      // First entry to mode 3: hide PC elements and push charselect.
      hide_spawn_elements(self);
      push_screen("ifs_charselect1");
      update_charselect(self);
   } else {
      // Restore PC elements when leaving charselect
      show_spawn_elements(self);
      original_ChangeMode(self, mode);
   }
}


// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------
void spawn_screen_install(uintptr_t exe_base)
{
   cfile log("BF2GameExt.log", "a");

   auto check_id = [&](const spawn_screen_addrs& a) -> bool {
      uint64_t val = *(uint64_t*)(exe_base + a.id_rva);
      return val == a.id_expected;
   };

   const spawn_screen_addrs* a = nullptr;
   const char* build = nullptr;

   if (check_id(MODTOOLS))    { a = &MODTOOLS; build = "modtools"; }
   else if (check_id(STEAM))  { a = &STEAM;    build = "Steam"; }
   else if (check_id(GOG))    { a = &GOG;      build = "GOG"; }
   else {
      log.printf("[SpawnScreen] Build not recognized\n");
      return;
   }

   // Charselect update requires additional addresses (modtools only for now)
   bool full_hooks = (a->update_object_text != 0);

   // Resolve function pointers
   original_ChangeMode = (fn_ChangeMode)(exe_base + a->change_mode);
   s_ifScreenMgrPtr    = (void**)(exe_base + a->ifscreenmgr_ptr);
   s_ifPush            = (fn_IFPush)(exe_base + a->ifscreenmgr_push);
   s_ifPop             = (fn_IFPop)(exe_base + a->ifscreenmgr_pop);
   s_ifIsActive        = (fn_IFIsActive)(exe_base + a->ifscreenmgr_is_active);

   if (full_hooks) {
      s_updateObjectText     = (fn_UpdateObjectText)(exe_base + a->update_object_text);
      s_setSlotInfo          = (fn_SetSlotInfo)(exe_base + a->set_slot_info);
      s_setEntityElement     = (fn_SetEntityElement)(exe_base + a->set_entity_element);
      s_findElement          = (fn_FindElement)(exe_base + a->find_element);
      s_spawnMgrTeams        = (void**)(exe_base + a->spawn_mgr_teams);
      s_teamIndexOff         = a->team_index_off;
   }

   // Install Detours hook
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_ChangeMode, hooked_ChangeMode);
   LONG result = DetourTransactionCommit();

   log.printf("[SpawnScreen] Hooks %s on %s (result=%ld, full=%d)\n",
              result == NO_ERROR ? "installed" : "FAILED", build, result, full_hooks);
}

void spawn_screen_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_ChangeMode)
      DetourDetach(&(PVOID&)original_ChangeMode, hooked_ChangeMode);
   DetourTransactionCommit();
}
