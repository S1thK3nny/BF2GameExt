#pragma once

#include <stdint.h>

// AI player-focus fairness patches.  Read from the INI in dllmain before
// ai_fairness_install().
//
//   g_aiPlayerVisionFairness    — [AI] PlayerVisionFairness     (default ON)
//   g_aiPlayerPriorityFairness  — [AI] PlayerPriorityFairness   (default ON)
//   g_aiPlayerThreatFairness    — [AI] PlayerThreatFairness     (default OFF)
//   g_aiPlayerAwarenessFairness — [AI] PlayerAwarenessFairness  (default ON)
//
// Three of the four remove a bias and default on.  #3 removes a real behaviour
// (AI escalating whoever aims at them) and defaults off; see the .cpp.
extern bool g_aiPlayerVisionFairness;
extern bool g_aiPlayerPriorityFairness;
extern bool g_aiPlayerThreatFairness;
extern bool g_aiPlayerAwarenessFairness;

void ai_fairness_install(uintptr_t exe_base);
void ai_fairness_uninstall();
