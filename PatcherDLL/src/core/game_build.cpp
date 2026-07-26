#include "pch.h"

#include "game_build.hpp"

namespace {
#include "game_addrs_data.inc" // defines: constexpr GameAddrs kAddrsModtools / kAddrsSteam / kAddrsGOG
}

GameBuild g_build = GameBuild::Unknown;

// Defaults to the modtools table so code that runs before detection (or on an
// unidentified build) keeps the historical behaviour.
const GameAddrs*    g_addr    = &kAddrsModtools;
const SoldierLayout* g_soldier = &kSoldierModtools;

void game_build_select(GameBuild build)
{
   g_build = build;
   switch (build) {
   case GameBuild::Steam: g_addr = &kAddrsSteam; g_soldier = &kSoldierRelease; break;
   case GameBuild::GOG:   g_addr = &kAddrsGOG;   g_soldier = &kSoldierRelease; break;
   case GameBuild::Modtools:
   default:               g_addr = &kAddrsModtools; g_soldier = &kSoldierModtools; break;
   }
}
