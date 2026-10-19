// Standalone tests (not a DLL build) for the setting state GameExt.IsPatchActive
// answers from: INI values, patch sets that did or did not install, and live flags.
#include "../PatcherDLL/src/util/ini_state.cpp"
#include <cassert>
#include <cstdio>

namespace {

const char* patch_set_of(const char* key)
{
   for (int i = 0; i < g_ini_registry_count; ++i)
      if (strcmp(g_ini_registry[i].key, key) == 0) return g_ini_registry[i].patch_set;
   assert(false);
   return nullptr;
}

bool query(const char* section, const char* key)
{
   bool active = true;
   const bool found = ini_state_find(section, key, &active);
   assert(found);
   return active;
}

void test_unknown()
{
   bool active = true;
   assert(!ini_state_find("Features", "NoSuchKey", &active) && !active);
   assert(!ini_state_find("Fixes", "Prone", &active) && !active);   // right key, wrong section
}

void test_values(const char* ini_path)
{
   FILE* f = nullptr;
   fopen_s(&f, ini_path, "w");
   assert(f);
   fputs("[Features]\nProne=0\nGameLogging=yes\nTargetBarLatchSeconds=0.5\n"
         "MPSpawnDelay=\n[Particles]\nParticleFixes=1\n", f);
   fclose(f);
   ini_state_capture(ini_config{ini_path});

   assert(!query("Features", "Prone"));
   assert(query("features", "gamelogging"));             // case ignored, as the INI does
   assert(query("Features", "TargetBarLatchSeconds"));   // 0.5 is not 0
   assert(query("Features", "MPSpawnDelay"));            // empty reads as the default, 15
   assert(!query("Features", "DisableAwardBuffs"));      // absent, default 0
   assert(query("General", "Enabled"));
}

void test_patch_sets()
{
   // On in the INI but not written yet: inactive.
   assert(!query("Fixes", "ChunkPushFix"));
   ini_state_mark_patch_set(patch_set_of("ChunkPushFix"));
   assert(query("Fixes", "ChunkPushFix"));

   // ParticleFixes drives three patch sets and needs all of them.
   ini_state_mark_patch_set("Particle Cache Increase");
   ini_state_mark_patch_set("Particle Effect Skip Fix");
   assert(!query("Particles", "ParticleFixes"));
   ini_state_mark_patch_set("Particle Cache Reset Fix");
   assert(query("Particles", "ParticleFixes"));
}

void test_live()
{
   bool flag = true;
   ini_state_bind_live("Features", "GameLogging", &flag);
   assert(query("Features", "GameLogging"));
   flag = false;
   assert(!query("Features", "GameLogging"));
}

}

int main()
{
   test_unknown();
   // Next to the exe in build/tests. GetPrivateProfileString needs a full path,
   // and the runner starts us from the repo root.
   char ini_path[MAX_PATH];
   GetModuleFileNameA(nullptr, ini_path, MAX_PATH);
   strcpy_s(strrchr(ini_path, '\\') + 1, MAX_PATH - (strrchr(ini_path, '\\') + 1 - ini_path),
            "ini_state_tests.ini");
   test_values(ini_path);
   test_patch_sets();
   test_live();
   std::printf("ini_state: all passed\n");
   return 0;
}
