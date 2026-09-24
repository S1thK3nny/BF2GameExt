#include "pch.h"
#include "odf_gameext_props.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "core/x86_emit.hpp"
#include "util/install_log.hpp"

#include <cstring>
#include <cstdarg>
#include <cstdio>

// =============================================================================
// GameExt-only ODF property overrides.
//
//     WeaponName         = "vanilla_weapon"
//     WeaponName@GameExt = "gameext_weapon"
//
// -----------------------------------------------------------------------------
// Why the name carries the marker, and why it is a suffix
//
// The platform prefix stock ODFs use (`PC:BuildingCollision`) is resolved by
// OdfMunge, not by the engine: the prefix is compared against `-platform` and a
// non-match is dropped with no warning, and the accepted set is hard-coded
// `xbox|ps2|pc`. `pc:` does not appear as a string in any game exe. So a
// `GameExt:` prefix cannot survive the stock toolchain.
//
// What does survive is an unrecognised property *name*: OdfMunge hashes it and
// emits it like any other PROP, and no SetProperty in any build reports an
// unknown hash, so a vanilla exe ignores it silently.
//
// Measured against ToolsFL/bin/OdfMunge.exe with -text -platform pc:
//
//   WeaponName            = "a"   ->  PROP 0xfbf47dba "a"
//   PC:WeaponName         = "b"   ->  PROP 0xfbf47dba "b"   prefix stripped,
//                                     emitted as a SECOND PROP, last one wins
//   xbox: / ps2: / GameExt: / foo: prefixed  ->  dropped, silently
//   TotallyMadeUpProperty = "c"   ->  PROP 0x79e334f3 "c"   unknown name kept
//   [GameExtProperties] section   ->  dropped, lines and all; a following
//                                     [Properties] resumes normally
//
// A colon is unusable in either position. The property name is everything after
// the LAST colon, and the platform test runs only when the first and last colon
// are the same one, so `WeaponName:GameExt` and `Weapon:Name` are dropped, while
// `GameExt::WeaponName`, `foo::WeaponName`, `::WeaponName` and
// `pc:xbox:WeaponName` all collapse to bare `WeaponName` (0xfbf47dba) - which a
// vanilla exe would then take as well. Only ':' means anything to the munger, so
// '@', '.' and '-' pass into the hashed name untouched.
//
// Two things then decide the exact spelling:
//
//   - OdfMunge's dependency scanner matches property names by SUBSTRING, so
//     `WeaponName@GameExt` is scanned exactly like `WeaponName` and its value
//     lands in the `class` REQN bucket. `GeometryName@GameExt` lands in `model`,
//     `AnimationBank@GameExt` in `anim`. Hiding the target name inside a value
//     instead would make the referenced asset invisible to the scanner and it
//     would never be packed into the .lvl.
//
//   - PblHash is FNV-1a over `(c | 0x20)`:
//
//         h' = (h ^ c) * 16777619
//
//     Every step is a bijection, so appending a fixed suffix is an invertible
//     fold. That makes the real property hash recoverable from the suffixed one
//     with no name table at all, which is why the marker is a SUFFIX. A prefix
//     would change the FNV starting state rather than folding onto a finished
//     hash, and would need the full list of property names shipped with the DLL.
//
// -----------------------------------------------------------------------------
// The guard
//
// Nothing in a hash says whether the name it came from ended in "@GameExt", so
// unfolding blindly would fire a spurious SetProperty for every property in the
// game. The rule is therefore:
//
//     an override applies only if the property it names is one of the last
//     16 properties read
//
// which doubles as the reason the ODF still works in vanilla (the plain line
// has to be there, above it). A fixed-size window needs no per-ODF reset, so
// there is no fifth hook for the four Read entries.
//
// A false positive needs a real property whose name unfolds to another property
// near it. Scanning every ODF under E:\BF2_Modtools (13362 files, 392448
// properties) found zero, both for adjacent pairs and for full per-ODF sets,
// and a 16-entry window is smaller than either.
//
// The reverse cannot be diagnosed: an `@GameExt` line whose partner is not in
// the window is indistinguishable from any other unknown property, so it is
// ignored in silence exactly as vanilla ignores it.
//
// -----------------------------------------------------------------------------
// The hook
//
// Every ODF reaches the engine through one of four readers, and all four have
// the same PROP branch: PblFile::Read32 for the property id, ReadString for the
// value, then a virtual call to SetProperty. The dispatch is nine bytes on every
// build, which is room for `E9 rel32` plus four NOPs, so each site jumps to a
// small naked shim that hands the three values to one shared dispatcher and
// makes the virtual call itself.
//
// Registers, vtable slot and even instruction order differ per reader and per
// build, so the shims are per site. Debug puts the pushes before `MOV ECX`,
// release after; the byte count is the same either way.
//
//   build     reader                site        bytes                        this hash slot
//   --------  --------------------  ----------  ---------------------------  ---- ---- ----
//   modtools  EntityClass::Read     0x004D08AE  8B 13 50 57 8B CB FF 52 18   EBX  EDI  0x18
//             ExplosionClass::Read  0x00601FAB  8B 17 50 56 8B CF FF 52 0C   EDI  ESI  0x0C
//             OrdnanceClass::Read   0x00605A4B  8B 16 50 57 8B CE FF 52 18   ESI  EDI  0x18
//             WeaponClass::Read     0x0061E46C  8B 13 50 57 8B CB FF 52 18   EBX  EDI  0x18
//
//   Steam     EntityClass::Read     0x00491D2D  8B 17 8B CF 50 56 FF 52 18   EDI  ESI  0x18
//             ExplosionClass::Read  0x0051CF2D  8B 17 8B CF 50 56 FF 52 0C   EDI  ESI  0x0C
//             OrdnanceClass::Read   0x005F842D  8B 17 8B CF 50 56 FF 52 18   EDI  ESI  0x18
//             WeaponClass::Read     0x0067A2B9  8B 13 8B CB 50 56 FF 52 18   EBX  ESI  0x18
//
//   GOG       EntityClass::Read     0x00491D2D  same shape as Steam throughout
//             ExplosionClass::Read  0x0051CF2D
//             OrdnanceClass::Read   0x005F94CD
//             WeaponClass::Read     0x0067B359
//
// The value pointer is in EAX on all twelve.
//
// How the four were told apart on retail, which strips the RedWarning strings
// they are identified by in the debug build: there are eight `CMP EAX,'PROP'`
// sites per image, and the readers are the ones whose dispatch matches the
// modtools shape 1:1 - the SetProperty slot (0x0C is Explosion, 0x18 the other
// three), the ReadString buffer size (0x80 on Explosion and Ordnance, 0x100 on
// Entity and Weapon), and the tail after the vcall (bare JMP on Entity, the
// `MOV EAX,[this+0x60] / TEST / JZ / MOV this,EAX` charge-chain walk on Weapon,
// its `CMP this,EAX` variant on Ordnance). Ghidra confirms the two ends
// independently: on Steam the Entity and Weapon sites fall inside functions
// already named `EntityClass::Read` (0x00491CC0) and `WeaponClass::Read`
// (0x0067A240).
//
// The fifth site with a vcall is NOT one of ours and is deliberately skipped:
// slot 0x10, and on Steam it sits in `LoadUtil::ProcessEntity` (0x0057A000),
// which is the world-layer instance property path. That is the lead for
// extending this to `[InstanceProperties]`, and the three remaining sites in
// the same cluster have no vcall at all.
// =============================================================================

namespace {

constexpr uint32_t kFnvPrime   = 16777619u;
// Modular inverse of kFnvPrime mod 2^32, so one fold step can be undone:
//     h = (h' * kFnvPrimeInv) ^ c
constexpr uint32_t kFnvPrimeInv = 0x359C449Bu;

// The marker, already lowercased the way PblHash sees it: PblHash folds
// (c | 0x20), and '@' (0x40) folds to 0x60, so that is the byte spelled out here
// rather than the literal.
//
// The separator is a free choice except for ':', which the munger owns: there
// `Name:GameExt` is dropped outright and the spellings it does keep collapse to
// the bare property name, which vanilla would then take as well. '@', '.', '-'
// and '_' all pass through into the hashed name and keep their dependency
// scanning. '@' because it appears in no stock ODF key at all, so
// `WeaponName@GameExt` reads as an annotation rather than as a property somebody
// meant to spell differently.
constexpr char kMarker[] = {(char)0x60, 'g', 'a', 'm', 'e', 'e', 'x', 't'};

// Fold the marker onto a finished hash: PblHash(name + "@GameExt") from
// PblHash(name).
inline uint32_t fold_marker(uint32_t h)
{
   for (size_t i = 0; i < sizeof(kMarker); ++i)
      h = (h ^ (uint8_t)kMarker[i]) * kFnvPrime;
   return h;
}

// Sliding window over the properties read so far. Each slot holds a property's
// own hash and the suffixed hash an override of it would have to carry, so the
// fold runs once per property rather than once per comparison.
constexpr int kWindow = 16;

struct Recent {
   uint32_t hash;
   uint32_t folded;
};

Recent s_recent[kWindow] = {};
int    s_recentNext      = 0;   // next slot to overwrite
int    s_recentUsed      = 0;   // slots filled, saturates at kWindow

uint32_t s_applied = 0;

// SetProperty(uint nameHash, const char* value), reached through the class
// vtable. The slot differs between the readers, hence the explicit offset.
typedef void(__thiscall* SetProperty_t)(void* self, uint32_t hash, const char* value);

inline void call_set_property(void* cls, uint32_t vslot, uint32_t hash, const char* value)
{
   void** vtbl = *(void***)cls;
   ((SetProperty_t)vtbl[vslot / sizeof(void*)])(cls, hash, value);
}

// Remember this property as a possible override target.
inline void remember(uint32_t hash)
{
   s_recent[s_recentNext].hash   = hash;
   s_recent[s_recentNext].folded = fold_marker(hash);
   s_recentNext = (s_recentNext + 1) % kWindow;
   if (s_recentUsed < kWindow) ++s_recentUsed;
}

// Newest first, so the nearest plain line wins when a name repeats.
inline bool find_target(uint32_t suffixedHash, uint32_t* out)
{
   for (int i = 1; i <= s_recentUsed; ++i) {
      const Recent& r = s_recent[(s_recentNext - i + kWindow) % kWindow];
      if (r.folded == suffixedHash) {
         *out = r.hash;
         return true;
      }
   }
   return false;
}

} // namespace

// The shared dispatcher. __cdecl so the naked shims can push arguments and
// clean up without knowing anything about the C side.
extern "C" void __cdecl odf_gameext_prop_dispatch(void* cls, uint32_t hash,
                                                  const char* value, uint32_t vslot)
{
   // A ClassLabel the engine does not know leaves the reader with a null class
   // and it dereferences it here. Nothing we can do about the rest of that ODF,
   // but our own call must not fault.
   if (cls == nullptr) return;

   uint32_t target;
   if (find_target(hash, &target)) {
      // This property's name is an earlier property's name plus "@GameExt".
      // Apply it as the property it names. The suffixed hash itself is not
      // forwarded: no SetProperty anywhere recognises it.
      call_set_property(cls, vslot, target, value);
      ++s_applied;
   } else {
      call_set_property(cls, vslot, hash, value);
   }

   // Remember the name as WRITTEN either way, so an override never becomes the
   // target of a second one.
   remember(hash);
}

namespace {

// -----------------------------------------------------------------------------
// Per-site shims. Each knows only its reader's register allocation and vtable
// slot; everything else is in the dispatcher.
//
// pushad/popad around the call, so every register the reader relies on
// afterwards comes back exactly as SetProperty would have left it - the charge
// chain walk in WeaponClass::Read and the equivalent in OrdnanceClass::Read both
// read `this` straight after the vcall, and both registers are callee-saved by
// __thiscall anyway.
//
// The continuation pointers are shared between Steam and GOG because only one
// build's table is ever installed.
// -----------------------------------------------------------------------------

void* s_contEntity    = nullptr;
void* s_contExplosion = nullptr;
void* s_contOrdnance  = nullptr;
void* s_contWeapon    = nullptr;

#define ODF_PROP_SHIM(fn, thisreg, hashreg, slot, cont) \
   __declspec(naked) void fn()                          \
   {                                                    \
      __asm {                                           \
         __asm pushad                                   \
         __asm push slot                                \
         __asm push eax                                 \
         __asm push hashreg                             \
         __asm push thisreg                             \
         __asm call odf_gameext_prop_dispatch           \
         __asm add  esp, 16                             \
         __asm popad                                    \
         __asm jmp  [cont]                              \
      }                                                 \
   }

// modtools: debug codegen puts the pushes before MOV ECX
ODF_PROP_SHIM(shim_mt_entity,    ebx, edi, 0x18, s_contEntity)
ODF_PROP_SHIM(shim_mt_explosion, edi, esi, 0x0C, s_contExplosion)
ODF_PROP_SHIM(shim_mt_ordnance,  esi, edi, 0x18, s_contOrdnance)
ODF_PROP_SHIM(shim_mt_weapon,    ebx, edi, 0x18, s_contWeapon)

// Steam and GOG: release codegen puts MOV ECX before the pushes
ODF_PROP_SHIM(shim_rt_entity,    edi, esi, 0x18, s_contEntity)
ODF_PROP_SHIM(shim_rt_explosion, edi, esi, 0x0C, s_contExplosion)
ODF_PROP_SHIM(shim_rt_ordnance,  edi, esi, 0x18, s_contOrdnance)
ODF_PROP_SHIM(shim_rt_weapon,    ebx, esi, 0x18, s_contWeapon)

#undef ODF_PROP_SHIM

struct Site {
   const char* name;      // reader, for the install log
   uintptr_t va;          // PROP-branch dispatch site, unrelocated
   uint8_t   expect[9];   // exact bytes the site must carry
   void    (*shim)();
   void**    cont;        // where the shim returns to (site + 9)
};

uint8_t* s_patched[4] = {};
uint8_t  s_orig[4][9] = {};
int      s_patchCount = 0;

// Patch one site: verify the nine bytes, then E9 rel32 + four NOPs.
bool install_site(uintptr_t exe_base, const Site& s)
{
   if (s.va == 0) return false;

   uint8_t* site = (uint8_t*)resolve(exe_base, s.va);
   if (std::memcmp(site, s.expect, sizeof(s.expect)) != 0) return false;

   *s.cont = (void*)(site + sizeof(s.expect));

   std::memcpy(s_orig[s_patchCount], site, sizeof(s.expect));
   s_patched[s_patchCount] = site;
   ++s_patchCount;

   // .text is RW during install; dllmain re-protects afterwards.
   x86::write_branch(site, x86::kJmp, s.shim, 9);
   return true;
}

} // namespace

void odf_gameext_props_install(uintptr_t exe_base)
{
   const bool modtools = (g_build == GameBuild::Modtools);
   const bool retail   = (g_build == GameBuild::Steam || g_build == GameBuild::GOG);
   if (!modtools && !retail) return;

   // Addresses come from the per-build table; only the shape is chosen here.
   const Site modtoolsSites[] = {
      { "EntityClass::Read",    g_addr->entity_class_read_prop_site,
        {0x8B, 0x13, 0x50, 0x57, 0x8B, 0xCB, 0xFF, 0x52, 0x18},
        &shim_mt_entity,    &s_contEntity },
      { "ExplosionClass::Read", g_addr->explosion_class_read_prop_site,
        {0x8B, 0x17, 0x50, 0x56, 0x8B, 0xCF, 0xFF, 0x52, 0x0C},
        &shim_mt_explosion, &s_contExplosion },
      { "OrdnanceClass::Read",  g_addr->ordnance_class_read_prop_site,
        {0x8B, 0x16, 0x50, 0x57, 0x8B, 0xCE, 0xFF, 0x52, 0x18},
        &shim_mt_ordnance,  &s_contOrdnance },
      { "WeaponClass::Read",    g_addr->weapon_class_read_prop_site,
        {0x8B, 0x13, 0x50, 0x57, 0x8B, 0xCB, 0xFF, 0x52, 0x18},
        &shim_mt_weapon,    &s_contWeapon },
   };

   const Site retailSites[] = {
      { "EntityClass::Read",    g_addr->entity_class_read_prop_site,
        {0x8B, 0x17, 0x8B, 0xCF, 0x50, 0x56, 0xFF, 0x52, 0x18},
        &shim_rt_entity,    &s_contEntity },
      { "ExplosionClass::Read", g_addr->explosion_class_read_prop_site,
        {0x8B, 0x17, 0x8B, 0xCF, 0x50, 0x56, 0xFF, 0x52, 0x0C},
        &shim_rt_explosion, &s_contExplosion },
      { "OrdnanceClass::Read",  g_addr->ordnance_class_read_prop_site,
        {0x8B, 0x17, 0x8B, 0xCF, 0x50, 0x56, 0xFF, 0x52, 0x18},
        &shim_rt_ordnance,  &s_contOrdnance },
      { "WeaponClass::Read",    g_addr->weapon_class_read_prop_site,
        {0x8B, 0x13, 0x8B, 0xCB, 0x50, 0x56, 0xFF, 0x52, 0x18},
        &shim_rt_weapon,    &s_contWeapon },
   };

   const Site* sites = modtools ? modtoolsSites : retailSites;

   int ok = 0;
   for (int i = 0; i < 4; ++i) {
      if (install_site(exe_base, sites[i])) {
         ++ok;
      } else {
         // A silent decline turns a wrong address into a wasted play test, so
         // name the reader that did not take.
         install_log("[ODF] %s PROP site 0x%08x did not match - overrides on "
                     "that ODF type are off", sites[i].name, sites[i].va);
      }
   }

   install_log("[ODF] GameExt property overrides: %d/4 readers hooked", ok);
}

void odf_gameext_props_uninstall()
{
   for (int i = 0; i < s_patchCount; ++i)
      protected_write(s_patched[i], s_orig[i], sizeof(s_orig[i]));
   s_patchCount = 0;
}

uint32_t odf_gameext_props_applied()
{
   return s_applied;
}
