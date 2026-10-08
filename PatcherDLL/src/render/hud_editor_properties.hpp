#pragma once

#include <cstdint>

// GameExt's own .hud properties in the modtools HUD editor's panel, as lists
// the panel steps through: FillFrom on every BarBitmap, and ScreenAnchor on
// the pieces of a file laid out by AuthoredRatio or TrueWidescreen(1). The
// modules that own them answer the panel (hud_bar_fill_from,
// hud_true_widescreen); this one lists them and names their values. The
// editor only lists elements, so FileInfo lines and transforms are not
// offered. Modtools only: GameExt keeps the editor off on Steam and GOG.
void hud_editor_properties_install(uintptr_t exe_base);

// Called when the HUD opens (GameEvents::Open), after HUD::Manager::Open has
// made the item factories: adds the properties to their lists.
void hud_editor_properties_open();
