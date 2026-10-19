#pragma once

#include "util/ini_config.hpp"

// What each BF2GameExt.ini setting is doing this session, for GameExt.IsPatchActive.
// Values are read once at startup, as the patches are, so an edit mid-session
// changes nothing here either.

// Read every g_ini_registry row. Call once, before any patch set is applied.
void ini_state_capture(const ini_config& cfg);

// A patch set was written. Rows naming it count as active only after this.
void ini_state_mark_patch_set(const char* patch_set_name);

// The setting is also off while *flag is false (features that can switch off
// after startup, such as prone without prone.lvl).
void ini_state_bind_live(const char* section, const char* key, const bool* flag);

// False if no row has that section and key. Otherwise *active is whether the
// setting is on and running. Both names ignore case, as the INI does.
bool ini_state_find(const char* section, const char* key, bool* active);
