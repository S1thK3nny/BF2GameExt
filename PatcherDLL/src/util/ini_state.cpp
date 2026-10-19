#include "pch.h"
#include <stdlib.h>
#include <string.h>

#include "util/ini_state.hpp"
#include "util/ini_registry.hpp"

namespace {

struct row_state {
   bool        on;        // the INI value, or its default
   bool        applied;   // patch set written (INI_PATCH rows only)
   const bool* live;      // optional runtime flag
};

row_state s_rows[g_ini_registry_count];

// Matches ini_config::get_bool, but numbers go through atof so 0.5 is on.
bool parse_on(const char* value)
{
   if (_stricmp(value, "true") == 0 || _stricmp(value, "yes") == 0) return true;
   if (_stricmp(value, "false") == 0 || _stricmp(value, "no") == 0) return false;
   return atof(value) != 0.0;
}

bool row_matches(const IniEntry& entry, const char* section, const char* key)
{
   return _stricmp(entry.section, section) == 0 && _stricmp(entry.key, key) == 0;
}

}

void ini_state_capture(const ini_config& cfg)
{
   for (int i = 0; i < g_ini_registry_count; ++i) {
      const IniEntry& entry = g_ini_registry[i];
      char value[64];
      cfg.get_string(entry.section, entry.key, entry.default_value, value, sizeof(value));
      // A present but empty key reads as the default, as get_bool does.
      s_rows[i].on = parse_on(value[0] ? value : entry.default_value);
   }
}

void ini_state_mark_patch_set(const char* patch_set_name)
{
   for (int i = 0; i < g_ini_registry_count; ++i) {
      const char* set = g_ini_registry[i].patch_set;
      if (set && strcmp(set, patch_set_name) == 0) s_rows[i].applied = true;
   }
}

void ini_state_bind_live(const char* section, const char* key, const bool* flag)
{
   for (int i = 0; i < g_ini_registry_count; ++i) {
      if (row_matches(g_ini_registry[i], section, key)) s_rows[i].live = flag;
   }
}

bool ini_state_find(const char* section, const char* key, bool* active)
{
   // The proxy only loads us when this is on.
   if (_stricmp(section, "General") == 0 && _stricmp(key, "Enabled") == 0) {
      *active = true;
      return true;
   }

   // A key shared by several patch sets (ParticleFixes) needs all of them.
   bool found = false, on = true;
   for (int i = 0; i < g_ini_registry_count; ++i) {
      const IniEntry& entry = g_ini_registry[i];
      if (!row_matches(entry, section, key)) continue;
      found = true;
      const row_state& row = s_rows[i];
      if (!row.on || (entry.patch_set && !row.applied) || (row.live && !*row.live)) on = false;
   }
   *active = found && on;
   return found;
}
