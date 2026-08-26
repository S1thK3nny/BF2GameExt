#pragma once

#include <stdint.h>

// =============================================================================
// Custom weapon HUD icons: stop two mods from cancelling each other out.
//
// THE SYMPTOM
//   Mods that add weapons ship an "extra weapons" .hud file so their weapons get
//   a HUD icon.  Each one works on its own.  Load two in the same session - a map
//   mod plus a side mod that replaced rep.lvl, say - and you get two icons per
//   weapon: the mod's correctly placed one, plus a stray one showing the raw
//   world model at the stock icon's position.  Which weapons break depends on
//   which .lvl happened to load last, so it looks random.
//
// WHAT AN "EXTRA WEAPONS" FIX ACTUALLY IS
//   Two items per weapon channel:
//     1. a TransformNameMesh mapping each custom weapon's mesh name to
//        com_inv_mesh, wired EventInput("player1.weaponN.change") ->
//        EventOutput("player1.weaponN.mesh").  That blanks the STOCK icon element,
//        which is what listens to player1.weaponN.mesh.
//     2. the mod's own Model3D, bound to the raw player1.weaponN.change, with
//        Scale(0,0,0) as its default and a hand-placed MeshInfo per weapon it
//        supports.  ElementModel3D::FindMeshInfoByHashID returns &mDefault on a
//        miss, so a weapon the mod does not list collapses that element to zero -
//        which is why two mods' Model3D elements do not fight each other.
//
// THE DEFECT
//   HUD::TransformNameMesh::EventInput does not stay quiet when it has no mapping
//   for the incoming weapon.  It resolves the mesh-name hash straight out of
//   RedModel::_HashTable and sends that model to its output event:
//
//       if (mNumMappings && (nm = FindNameMesh(this, hash)) &&
//           (m = nm->GetMesh(...)))
//           goto send;
//       m = RedModel::_HashTable.Find(hash);    // <- the miss path
//       if (!m) return;
//       send: Event(mEventClassOutput, m).Send();
//
//   EventClass::RegisterEventHandler appends at the tail and EventClass::Send
//   walks head to tail, so handlers fire in registration order and the last .hud
//   file parsed decides.  With two fixes loaded, the transform that has never
//   heard of the weapon overwrites the one that has, the stock icon element gets
//   the real mesh instead of com_inv_mesh, and it draws alongside the mod's own
//   icon.  That is the second, wrong-looking icon.
//
//   It breaks stock remaps too: hudtransforms.hud maps cis_weap_inf_wrist_trishot
//   to hud_cis_trishot, and any extraweapons fix loaded after it falls through and
//   re-sends the world mesh.
//
// THE FIX
//   Arbitration, not merging: mapped beats unmapped.  Before running the original,
//   if THIS transform has no mapping for the hash but another transform sharing
//   the same output event does, return without sending.
//
//   Order independent, needs no changes to anyone's .hud file, and identical to
//   stock when only one transform is present.  If no transform has the mapping
//   they all still fall through to the passthrough, which is the correct stock
//   behaviour for a vanilla weapon.
//
//   Known limit: a transform whose mapping names a mesh that was never loaded
//   still falls through, because we arbitrate on "has a mapping" rather than
//   calling NameMesh::GetMesh on another item mid-dispatch.  That case is already
//   broken today and is a bug in the mod's .req, not a regression here.
//
//   All three builds (modtools, Steam, GOG).  The install byte-guards both
//   functions and declines with a line in BF2GameExt.log if either prologue does
//   not match, rather than patching something it does not recognise.
//
//   INI: [Fixes] WeaponIconFix=1
// =============================================================================

extern bool g_hudWeaponIconFixEnabled;

void hud_weapon_icon_fix_install(uintptr_t exe_base);
void hud_weapon_icon_fix_uninstall();
