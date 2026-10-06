#include "pch.h"
#include "hud_true_widescreen.hpp"
#include "hud_true_widescreen_core.hpp"
#include "hud_number_math_core.hpp"   // hud_number_math::Data, the PblConfig::Data reader
#include "hud_widescreen.hpp"         // ReticleCorrection, taken back off for opted-in reticules
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <detours.h>

#include <stdio.h>
#include <string.h>

// =============================================================================
// What the stock game does to a HUD on a screen wider than 4:3, read on
// modtools and measured in game at 1280 x 720 (docs/RE/HUDSystem.md, "Wide
// screens"):
//   - positions and sizes are fractions of the real screen, so the layout
//     spreads to the edges and everything is a third wider than on 4:3;
//   - RedInterfaceScreen::Render draws the HUD screen groups through a 640 x 480
//     mapping, and the loader letterboxes each group's local matrix (0x006B7300),
//     which nets out as x exact, y = 40 + 8/9 y: a band from y 40 to 680;
//   - the bitmap rect setup scales each bitmap by (1, 0.75 / aspect), 4/3 at
//     16:9; ElementMap scales the minimap by 1 + (that - 1) / 4; text gets none.
// Those apply to whole screen groups, and every .hud file shares the groups.
//
// A file with TrueWidescreen(1) in its FileInfo opts its own pieces out:
//   - FileInfo::ReadData reads the flag. HUD::Manager::Load (one call per
//     file) notes which elements the file made by comparing HUD::Element::sList
//     before and after, and while the file loads its elements are laid out with
//     a 4:3 width (GetContainerViewWidth, the Screen-mode width the
//     relative-to-pixels conversion is handed, and sViewportWidth itself, lent
//     until the file is loaded, which ElementMap's constructor and ElementText's
//     TextBox read directly) and see a 4:3 aspect at the bitmap, minimap and
//     segmented-bar calls, so nothing is stretched;
//   - afterwards each top-level piece is given a slide from where it sits:
//     none, half or all of the width beyond 4:3 (left, middle and right third
//     of the layout). A piece written at the corner with no position is a
//     plain container: its children are slid one by one instead. A Target
//     element is never slid: ElementTarget::Update places its markers from the
//     real viewport width;
//   - the element draw (the one every interface element goes through) draws an
//     opted-in top-level piece under a 1:1 parent of its own, slid, instead of
//     its screen group's squashed one; its children inherit it;
//   - EventPosition turns world-tracking positions (fractions of the real
//     screen: reticule, lock-on, GameExt's target bar and command posts) into
//     the layout fraction that lands on the real point, and takes
//     ReticleCorrection back off the reticule's y;
//   - GetContainerViewWidth keeps the 4:3 width for the file's elements later,
//     when events move them.
// Only with one viewport (split screen keeps the stock HUD) and only on a
// screen the engine treats as wide. Everything is read when a file loads.
//
// The modtools HUD editor writes a .hud back through the same per-element
// width, so it writes an opted-in file's 4:3 numbers. The gaps are closed:
// FileInfo's writer only knows SplitMode, Viewports and Widescreen, so it is
// followed by TrueWidescreen(1) for a FileInfo that read it; a Screen-mode
// position is written against the real width, so the pixels-to-relative
// conversion gets the 4:3 one for an opted-in element, as on the way in; and
// ElementText's SetProperty, GetProperty and WriteData read sViewportWidth for
// TextBox, so it is lent the 4:3 width while they run on an opted-in element.
//
//                                     modtools    Steam       GOG
//   FileInfo::ReadData                0x006B7F50  0x00564DB0  0x00565B30
//   HUD::Manager::Load                0x006B8480  0x00565950  0x005666D0
//   its read of a top-level item      0x006B8730  0x00565B5C  0x005668DC
//   GetContainerViewWidth             0x006926B0  0x005490B0  0x00549E00
//   relative-to-pixels conversion     0x00691120  0x00547010  0x00547D60
//   EventPosition handler             0x0069A350  0x0054F110  0x0054FE60
//   RedInterfaceElement draw          0x00816FA0  0x006C0DE0  0x006C1E70
//   HUD::Element::sList terminator    0x00AD7EE0  0x007EBA18  0x007EC9E8
//   gHudViewPorts cell                0x00BA4478  0x01E573C8  0x01E58878
//   interface frustum width t         0x00E5B504  0x0093E4E0  0x0093F980
//   GetScreenAspectRatio              0x008059A0  0x006B1890  0x006B2910
//   aspect call, bitmap rect setup    0x006988F9  0x0054D738  0x0054E488
//   aspect calls, ElementMap          0x0069BA00  0x005521AC  0x00552F0C
//                                     0x0069BA4E  0x00552208  0x00552F68
//                                     0x0069BA66  0x00552226  0x00552F86
//                                     0x0069BAA3  0x0055227E  0x00552FDE
//   aspect call, segmented bar        0x00696B97  0x0054C24C  0x0054CF9C
//   FileInfo vtable                   0x00A60398  0x007A329C  0x007A4064
//   HUD::Element::sViewportWidth      0x00BA38FC  0x01E56C34  0x01E580E4
//   ElementTarget vtable              0x00A5E000  0x007A17FC  0x007A2658
//   FileInfo WriteData                0x006B8090  (the editor's: modtools only)
//   indent writer / format writer     0x006B5A20 / 0x006B5A50
//   pixels-to-relative conversion     0x00691170
//   gConfigFiles / count              0x00BA4070 / 0x00BA4480
//   ElementText Set/GetProperty       0x006AAE10 / 0x006A95C0
//   ElementText WriteData             0x006A9A90
//
// At the aspect calls the element is in ESI (bitmap rect setup, and
// ElementMap on retail), in EBX (ElementMap on modtools), or 0x200 below the
// ElementBar base in EBX (modtools) or EDI (retail) for the segmented bar.
//
// Retail is LTCG-built, and four of these have their own contracts there:
// Manager::Load takes its PblConfig in ECX; GetContainerViewWidth returns in
// XMM0 and keeps every register but EAX and ECX; the relative-to-pixels
// conversion takes the mode in ECX, value, frame and view in XMM1-3 and the
// screen width on the stack, returns in XMM0 and changes only XMM0 and XMM1;
// and Load reads a top-level item with MOV ECX,ESI / CALL [EAX+8]. LTCG
// callers keep values in registers a callee leaves alone, XMM ones too, so
// the retail stand-ins give back every register but the result. The editor's
// pieces are modtools only: GameExt keeps the editor off on retail.
//
// Element layout, the same on every build: +0xB0 the RedInterfaceElement it
// draws with, +0xB4 its sList node (next at +0), +0x158 its RelativeMode.
// RedInterfaceElement: local matrix at +0x30, the parent's child list at +0x1C
// (null when it has no parent), which RedGroupElement keeps at +0x70; the
// pieces are found in that tree, the one the draw walks, since HUD::Element's
// group (+0xF4) is only set for some. An Event is { EventClass*, payload* };
// an EventClass starts with its name's PblHash and its type (9 = Vector3).
// =============================================================================

namespace {

using namespace hud_true_widescreen;
using Data = hud_number_math::Data;

constexpr uint32_t kTrueWidescreen = pbl_hash("TrueWidescreen");

constexpr uint32_t kRed         = 0xB0;
constexpr uint32_t kNode        = 0xB4;
constexpr uint32_t kRedParentList = 0x1C;   // RedInterfaceElement: the parent's child list
constexpr uint32_t kRedChildList  = 0x70;   // RedGroupElement: its child list
constexpr uint32_t kMode        = 0x158;
constexpr uint32_t kRedLocal    = 0x30;
constexpr uint32_t kViewStride  = 0xA0;
constexpr int      kViewGroups  = 5;      // the full screen and four viewports
constexpr uint32_t kNumCameras  = 0x1C;   // CameraManager::m_iNumCam
constexpr uint32_t kTypeVector3 = 9;
constexpr uint8_t  kModeScreen   = 1;
constexpr uint8_t  kModeViewport = 2;
constexpr float    kFourThreeAspect = 0.75f;   // height / width of a 4:3 screen
constexpr int      kMaxDepth    = 64;          // a group chain longer than this is not a HUD
constexpr int      kMaxList     = 100000;      // a longer sList is not one
constexpr int      kMaxPlaced   = 512;
constexpr int      kMaxFiles    = 64;
constexpr uint32_t kBarSegmentedBase = 0x200;  // ElementBarSegmented's ElementBar base

using ReadDataFn  = bool(__fastcall*)(void* self, void* edx, void* config, const Data* data);
using WriteDataFn = void(__fastcall*)(void* self, void* edx, void* file, int indent);
using IndentFn    = void(__fastcall*)(void* file, void* edx, int count);
using FormatFn    = void(__cdecl*)(void* file, const char* format, ...);
using TextCallFn  = uint32_t(__fastcall*)(void* self, void* edx, void* a, void* b);
using LoadFn      = void(__cdecl*)(void* config);
using ViewWidthFn = float(__fastcall*)(const uint8_t* element, void* edx);
using ToPixelsFn  = float(__cdecl*)(int mode, float value, float frame, float view, float screen);
using PositionFn  = void(__cdecl*)(const uint32_t* event, uint8_t* element);
using DrawFn      = void(__fastcall*)(uint8_t* element, void* edx, const float* parent, uint32_t color);
using AspectFn    = float(__cdecl*)();
using LoadRetailFn = void(__fastcall*)(void* config);   // retail: the PblConfig in ECX

ReadDataFn  s_readData  = nullptr;
WriteDataFn s_writeData = nullptr;
IndentFn    s_indent    = nullptr;
FormatFn    s_format    = nullptr;
LoadFn      s_load      = nullptr;
ViewWidthFn s_viewWidth = nullptr;
ToPixelsFn  s_toPixels  = nullptr;
ToPixelsFn  s_toRelative = nullptr;
TextCallFn  s_textSet   = nullptr;
TextCallFn  s_textGet   = nullptr;
TextCallFn  s_textWrite = nullptr;
PositionFn  s_position  = nullptr;
DrawFn      s_draw      = nullptr;
AspectFn    s_aspect    = nullptr;
LoadRetailFn s_loadRetail = nullptr;
void*       s_viewWidthRetail = nullptr;   // reached only by JMP from its stand-in
void*       s_toPixelsRetail  = nullptr;   // reached by JMP or CALL from its stand-in

const int*            s_screen     = nullptr;   // s_screenFull: width, height
const uintptr_t*      s_listEnd    = nullptr;   // HUD::Element::sList terminator
const uintptr_t*      s_viewGroups = nullptr;   // cell holding gHudViewPorts
const float*          s_frustum    = nullptr;   // interface camera frustum width t
const uint8_t* const* s_cameras    = nullptr;   // CameraManager instance cell
const void* const*    s_configFiles = nullptr;  // gConfigFiles: the loaded files' FileInfos
const int*            s_configCount = nullptr;
uint32_t              s_fileInfoVtable = 0;
uint32_t              s_targetVtable   = 0;
float*                s_viewportWidth  = nullptr;   // HUD::Element::sViewportWidth
bool                  s_viewportLent   = false;     // holds the 4:3 width while a file loads
float                 s_viewportOwn    = 0.0f;      // what it held before

// FileInfos that read TrueWidescreen(1), for the editor's writer.
const void* s_optedFiles[kMaxFiles];
int         s_optedFileCount = 0;

// A piece the draw slides: a top-level element of an opted-in file, or a child
// of one that is a plain container. red and vtable, taken when it was placed,
// tell a stale entry (the HUD unloaded and the memory reused) from a live one.
struct Placed {
   const uint8_t* element;
   const uint8_t* red;
   uint32_t       vtable;
   const uint8_t* container;   // the plain container it was placed under, or null at the top
   float          slide;
   bool           isContainer;
};

Placed s_placed[kMaxPlaced];
int    s_placedCount = 0;
bool   s_warnedFull  = false;
bool s_loading      = false;   // inside HUD::Manager::Load
bool s_loadingWide  = false;   // the screen was wide when this file began loading
bool s_fileOptedIn  = false;   // this file's FileInfo has TrueWidescreen(1)
bool s_warnedFlag   = false;
int  s_drawDepth    = 0;       // draw nesting under the screen groups
bool s_inViewGroup  = false;   // drawing under one of the HUD screen groups
const Placed* s_drawContainer = nullptr;   // drawing under an opted-in plain container

uint32_t s_tracked[2 * 3 + 16];            // world-tracking position events, by name hash
uint32_t s_reticule[2];

// ---- screen ----------------------------------------------------------------

bool screen_size(float& width, float& height)
{
   const int w = s_screen[0], h = s_screen[1];
   if (w <= 0 || h <= 0) return false;
   width = (float)w;
   height = (float)h;
   return true;
}

bool one_viewport()
{
   const uint8_t* cameras = *s_cameras;
   return cameras && *reinterpret_cast<const int*>(cameras + kNumCameras) == 1;
}

// Wide and one viewport: the only case opted-in pieces are treated at all.
bool wide(float& width, float& height)
{
   return screen_size(width, height) && is_wide(width, height) && one_viewport();
}

// ---- elements ----------------------------------------------------------------

const uint8_t* red_of(const uint8_t* element)
{
   return *reinterpret_cast<const uint8_t* const*>(element + kRed);
}

void local_position(const uint8_t* red, float& x, float& y)
{
   const float* local = reinterpret_cast<const float*>(red + kRedLocal);
   x = local[12];
   y = local[13];
}

bool current(const Placed& p)
{
   __try {
      return *reinterpret_cast<const uint32_t*>(p.element) == p.vtable && red_of(p.element) == p.red;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

const Placed* placed_element(const uint8_t* element)
{
   for (int i = 0; i < s_placedCount; ++i)
      if (s_placed[i].element == element) return current(s_placed[i]) ? &s_placed[i] : nullptr;
   return nullptr;
}

const Placed* placed_red(const uint8_t* red, const uint8_t* container)
{
   for (int i = 0; i < s_placedCount; ++i)
      if (s_placed[i].red == red && s_placed[i].container == container)
         return current(s_placed[i]) ? &s_placed[i] : nullptr;
   return nullptr;
}

bool is_view_group(const uint8_t* red)
{
   const uintptr_t base = *s_viewGroups;
   if (!base || !red) return false;
   const uintptr_t at = reinterpret_cast<uintptr_t>(red);
   return at >= base && at < base + kViewGroups * kViewStride && (at - base) % kViewStride == 0;
}

// A drawable's parent in the interface tree, the one the draw walks:
// RedGroupElement::AddChild (modtools 0x00838AB0) stores the parent's child
// list, at the parent + 0x70, in the child at + 0x1C. A top-level piece is only
// in it while enabled: HUD::Element::UpdateEnableState (0x00692860) adds its
// drawable to the HUD screen group as it is enabled and removes it once
// disabled. Nested drawables stay with their group throughout.
// The link is a list node, { list, next, prev, item }, and AddChild points its
// item back at the child; a node that does not is no link (the screen groups
// and the editor's own groups hang off other structures).
const uint8_t* red_parent(const uint8_t* red)
{
   const uint8_t* const* node = reinterpret_cast<const uint8_t* const*>(red + kRedParentList);
   const uint8_t* list = node[0];
   if (!list || node[3] != red || reinterpret_cast<uintptr_t>(list) < 0x10000 + kRedChildList) return nullptr;
   return list - kRedChildList;
}

// The placed top-level piece a drawable is part of, found by walking up its
// tree as far as a HUD screen group, and the drawable just under that piece;
// null when none is.
const Placed* placed_top_of(const uint8_t* red, const uint8_t** underTop)
{
   const uint8_t* below = nullptr;
   for (int i = 0; red && i < kMaxDepth; ++i) {
      if (const Placed* p = placed_red(red, nullptr)) {
         if (underTop) *underTop = below;
         return p;
      }
      if (is_view_group(red)) return nullptr;
      below = red;
      red = red_parent(red);
   }
   return nullptr;
}

// Laid out on 4:3: an element of the opted-in file being loaded, or one that
// is part of a placed top-level piece.
bool laid_out_wide(const uint8_t* element)
{
   if (s_loading) return s_fileOptedIn && s_loadingWide;
   if (!s_placedCount || !element) return false;
   __try {
      return placed_top_of(red_of(element), nullptr) != nullptr;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// The slide the draw gives an element's top-level piece, or the child of a
// plain container it is drawn under.
float slide_above(const uint8_t* element)
{
   const uint8_t* underTop = nullptr;
   const Placed* p = placed_top_of(red_of(element), &underTop);
   if (!p) return 0.0f;
   if (!p->isContainer) return p->slide;
   if (!underTop) return 0.0f;   // the container itself
   const Placed* child = placed_red(underTop, p->red);
   return child ? child->slide : 0.0f;
}

// The items HUD::Manager::Load reads at the top of the file being loaded,
// noted at its read call; nested items are read by their groups instead.
constexpr int kMaxTopItems = 1024;
const uint8_t* s_topItems[kMaxTopItems];
int            s_topItemCount = 0;

void __cdecl note_top_item(const uint8_t* item)
{
   if (s_loading && s_topItemCount < kMaxTopItems) s_topItems[s_topItemCount++] = item;
}

bool is_top_item(const uint8_t* element)
{
   for (int i = 0; i < s_topItemCount; ++i)
      if (s_topItems[i] == element) return true;
   return false;
}

// Stands in for Manager::Load's MOV ECX,EDI / CALL [EDX+8], the read of each
// top-level item: notes the item (EDI) and goes on into its Read with the
// three arguments and Load's return address as they were.
__declspec(naked) void read_top_item()
{
   __asm {
      pushad
      push edi
      call note_top_item
      add esp, 4
      popad
      mov ecx, edi
      jmp dword ptr [edx + 8]
   }
}

// Retail: MOV ECX,ESI / CALL [EAX+8], the item in ESI and its vtable in EAX.
__declspec(naked) void read_top_item_retail()
{
   __asm {
      pushad
      push esi
      call note_top_item
      add esp, 4
      popad
      mov ecx, esi
      jmp dword ptr [eax + 8]
   }
}

uintptr_t next_node(uintptr_t node)
{
   return *reinterpret_cast<const uintptr_t*>(node);
}

// Drops entries whose element is gone: the HUD unloads between missions.
void prune()
{
   bool alive[kMaxPlaced] = {};
   const uintptr_t end = reinterpret_cast<uintptr_t>(s_listEnd);
   int n = 0;
   for (uintptr_t node = *s_listEnd; node && node != end && n < kMaxList; node = next_node(node), ++n) {
      const uint8_t* element = reinterpret_cast<const uint8_t*>(node - kNode);
      for (int i = 0; i < s_placedCount; ++i)
         if (s_placed[i].element == element) alive[i] = true;
   }
   int kept = 0;
   for (int i = 0; i < s_placedCount; ++i)
      if (alive[i] && current(s_placed[i])) s_placed[kept++] = s_placed[i];
   s_placedCount = kept;
}

// The elements made since `tail` was the list's last node: the element
// constructor appends. False if `tail` is no longer in the list, when which
// elements are new cannot be told.
template <class Visit>
bool for_each_made(uintptr_t tail, Visit visit)
{
   const uintptr_t end = reinterpret_cast<uintptr_t>(s_listEnd);
   bool after = tail == end;
   int n = 0;
   for (uintptr_t node = *s_listEnd; node && node != end && n < kMaxList; node = next_node(node), ++n) {
      if (after) visit(reinterpret_cast<const uint8_t*>(node - kNode));
      else if (node == tail) after = true;
   }
   return after;
}

bool add(const uint8_t* element, const uint8_t* container, float slide, bool isContainer)
{
   if (s_placedCount == kMaxPlaced) {
      if (!s_warnedFull)
         install_log("[TrueWidescreen] over %d pieces placed; the rest keep the stock layout", kMaxPlaced);
      s_warnedFull = true;
      return false;
   }
   s_placed[s_placedCount++] = { element, red_of(element), *reinterpret_cast<const uint32_t*>(element), container,
                                 slide, isContainer };
   return true;
}

// After an opted-in file loads: a slide for each of its top-level pieces, or
// for each child of a plain container.
void place(uintptr_t tail, float width, float height)
{
   const float layout = layout_width(height);
   const int first = s_placedCount;
   int counts[3] = {}, containers = 0, markers = 0;
   const bool known = for_each_made(tail, [&](const uint8_t* element) {
      const uint8_t* red = red_of(element);
      if (!red || !is_top_item(element)) return;
      if (*reinterpret_cast<const uint32_t*>(element) == s_targetVtable) {
         markers += add(element, nullptr, 0.0f, false);   // its markers are placed on the real screen
         return;
      }
      float x, y;
      local_position(red_of(element), x, y);
      if (is_origin(x, y)) {
         containers += add(element, nullptr, 0.0f, true);
         return;
      }
      const Anchor anchor = anchor_for(x, layout);
      counts[(int)anchor] += add(element, nullptr, slide_for(anchor, width, layout), false);
   });
   const int tops = s_placedCount;
   for_each_made(tail, [&](const uint8_t* element) {
      const uint8_t* red = red_of(element);
      const uint8_t* parent = red ? red_parent(red) : nullptr;
      if (!parent) return;
      bool inContainer = false;
      for (int i = first; i < tops; ++i) inContainer |= s_placed[i].isContainer && s_placed[i].red == parent;
      if (!inContainer) return;
      float x, y;
      local_position(red, x, y);
      const Anchor anchor = anchor_for(x, layout);
      counts[(int)anchor] += add(element, parent, slide_for(anchor, width, layout), false);
   });
   auto log = get_gamelog();
   if (!log) return;
   if (!known)
      log("[TrueWidescreen] a .hud file opted in, but which elements it made could not be told; it keeps the "
          "stock layout\n");
   else
      log("[TrueWidescreen] a .hud file laid out for %.0fx%.0f as on %.0fx%.0f: %d pieces kept to the left, "
          "%d centred, %d to the right (%d plain containers, %d Target elements)\n", width, height, layout,
          height, counts[0], counts[1], counts[2], containers, markers);
}

// ---- FileInfos that opted in, for the editor's writer -------------------------

bool is_loaded_file(const void* fileInfo)
{
   const int count = *s_configCount;
   for (int i = 0; i < count && i < 1024; ++i)
      if (s_configFiles[i] == fileInfo) return true;
   return false;
}

// Keeps the files the HUD still has: a FileInfo is freed with its HUD.
void remember_file(const void* fileInfo)
{
   int kept = 0;
   for (int i = 0; i < s_optedFileCount; ++i)
      if (s_optedFiles[i] != fileInfo && is_loaded_file(s_optedFiles[i])) s_optedFiles[kept++] = s_optedFiles[i];
   s_optedFileCount = kept;
   if (s_optedFileCount < kMaxFiles) s_optedFiles[s_optedFileCount++] = fileInfo;
}

bool opted_file(const void* fileInfo)
{
   if (*static_cast<const uint32_t*>(fileInfo) != s_fileInfoVtable || !is_loaded_file(fileInfo)) return false;
   for (int i = 0; i < s_optedFileCount; ++i)
      if (s_optedFiles[i] == fileInfo) return true;
   return false;
}

// ---- sViewportWidth ------------------------------------------------------------

// While a file with TrueWidescreen(1) loads, sViewportWidth holds the 4:3 width:
// ElementMap's constructor and ElementText's TextBox read it directly.
void lend_viewport_width()
{
   float width, height;
   if (s_viewportLent || !s_loadingWide || !screen_size(width, height)) return;
   s_viewportOwn = *s_viewportWidth;
   *s_viewportWidth = layout_width(height);
   s_viewportLent = true;
}

void return_viewport_width()
{
   if (!s_viewportLent) return;
   *s_viewportWidth = s_viewportOwn;
   s_viewportLent = false;
}

// ElementText's SetProperty, GetProperty and WriteData read sViewportWidth for
// TextBox too: for an opted-in element it holds the 4:3 width while they run.
uint32_t text_call(TextCallFn original, void* self, void* edx, void* a, void* b)
{
   float width, height;
   const bool lend = !s_viewportLent && laid_out_wide(static_cast<const uint8_t*>(self)) && wide(width, height);
   const float own = *s_viewportWidth;
   if (lend) *s_viewportWidth = layout_width(height);
   const uint32_t result = original(self, edx, a, b);
   if (lend) *s_viewportWidth = own;
   return result;
}

uint32_t __fastcall hooked_TextSet(void* self, void* edx, void* a, void* b)
{
   return text_call(s_textSet, self, edx, a, b);
}

uint32_t __fastcall hooked_TextGet(void* self, void* edx, void* a, void* b)
{
   return text_call(s_textGet, self, edx, a, b);
}

uint32_t __fastcall hooked_TextWrite(void* self, void* edx, void* a, void* b)
{
   return text_call(s_textWrite, self, edx, a, b);
}

// ---- hooks ---------------------------------------------------------------------

bool __fastcall hooked_ReadData(void* self, void* edx, void* config, const Data* data)
{
   if (data && data->id == kTrueWidescreen) {
      bool on = false;
      __try {
         if (!data->flag(0, on)) {
            on = false;
            if (!s_warnedFlag) {
               install_log("[TrueWidescreen] TrueWidescreen takes 1 or 0; that file keeps the stock layout");
               s_warnedFlag = true;
            }
         }
         if (on && s_configFiles) remember_file(self);   // for the editor's writer: modtools
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         on = false;
      }
      if (s_loading) {
         s_fileOptedIn = on;
         if (on) lend_viewport_width();
         else return_viewport_width();
      }
      return true;
   }
   return s_readData(self, edx, config, data);
}

// The editor writes a FileInfo with the lines the engine knows; add ours.
void __fastcall hooked_WriteData(void* self, void* edx, void* file, int indent)
{
   s_writeData(self, edx, file, indent);
   bool opted = false;
   __try {
      opted = opted_file(self);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      opted = false;
   }
   if (!opted) return;
   s_indent(file, nullptr, indent);
   s_format(file, "TrueWidescreen(1)\n");
}

// Around each HUD::Manager::Load: which elements the file makes and, if it
// opted in on a wide screen, where its pieces go.
struct Loading {
   float     width;
   float     height;
   bool      isWide;
   uintptr_t tail;
};

Loading begin_load()
{
   Loading l = {};
   l.isWide = wide(l.width, l.height);
   if (s_placedCount) prune();
   l.tail = s_listEnd[1];   // the last node, or the terminator itself when empty
   s_topItemCount = 0;
   s_loading = true;
   s_loadingWide = l.isWide;
   s_fileOptedIn = false;
   return l;
}

void end_load(const Loading& l)
{
   return_viewport_width();
   const bool optedIn = s_fileOptedIn && l.isWide;
   s_loading = false;
   s_fileOptedIn = false;
   if (optedIn) place(l.tail, l.width, l.height);
}

void __cdecl hooked_Load(void* config)
{
   const Loading l = begin_load();
   s_load(config);
   end_load(l);
}

// Retail: the PblConfig in ECX, plain RET. Its one caller reloads every
// register it uses after the call.
void __fastcall hooked_LoadRetail(void* config)
{
   const Loading l = begin_load();
   s_loadRetail(config);
   end_load(l);
}

// GetContainerViewWidth and the two conversions are small helpers that leave
// ECX and EDX alone (GetContainerViewWidth pushes and pops ECX; the conversions
// only touch EAX and ST0), and modtools callers keep values in them across the
// call: HUD::Matrix::WritePosition loads the property name into ECX at
// 0x00691628 and pushes it at 0x00691690, after two conversions;
// ApplyRelativeMode pushes the mode from ECX at 0x00691390 and again, unreloaded,
// at 0x006913B1. So each is replaced by a naked stand-in that keeps ECX and EDX
// and calls the C hook below with the same arguments.

float __cdecl view_width_hook(const uint8_t* element)
{
   const float stock = s_viewWidth(element, nullptr);
   float width, height;
   if (!laid_out_wide(element) || !screen_size(width, height)) return stock;
   return layout_width(height);
}

// The Screen-mode width is handed in as the real one; an element whose view
// width is the 4:3 layout width is opted in, and gets that instead. Both ways:
// reading a .hud and the editor writing one.
float screen_for(int mode, float view, float screen)
{
   if (mode == kModeScreen && view != screen && (s_loading || s_placedCount)) {
      float width, height;
      if (screen_size(width, height) && screen == width && view == layout_width(height)) return view;
   }
   return screen;
}

float __cdecl to_pixels_hook(int mode, float value, float frame, float view, float screen)
{
   return s_toPixels(mode, value, frame, view, screen_for(mode, view, screen));
}

float __cdecl to_relative_hook(int mode, float pixels, float frame, float view, float screen)
{
   return s_toRelative(mode, pixels, frame, view, screen_for(mode, view, screen));
}

// thiscall(element) -> ST0, RET 0.
__declspec(naked) void hooked_ViewWidth()
{
   __asm {
      push ecx
      push edx
      push ecx
      call view_width_hook
      add esp, 4
      pop edx
      pop ecx
      ret
   }
}

// cdecl(mode, value, frame, view, screen) -> ST0. After the two pushes the five
// arguments sit at [esp + 0x0C] .. [esp + 0x1C]; each push of [esp + 0x1C]
// takes the next one down, last to first.
__declspec(naked) void hooked_ToPixels()
{
   __asm {
      push ecx
      push edx
      push dword ptr [esp + 0x1C]
      push dword ptr [esp + 0x1C]
      push dword ptr [esp + 0x1C]
      push dword ptr [esp + 0x1C]
      push dword ptr [esp + 0x1C]
      call to_pixels_hook
      add esp, 0x14
      pop edx
      pop ecx
      ret
   }
}

__declspec(naked) void hooked_ToRelative()
{
   __asm {
      push ecx
      push edx
      push dword ptr [esp + 0x1C]
      push dword ptr [esp + 0x1C]
      push dword ptr [esp + 0x1C]
      push dword ptr [esp + 0x1C]
      push dword ptr [esp + 0x1C]
      call to_relative_hook
      add esp, 0x14
      pop edx
      pop ecx
      ret
   }
}

// Retail GetContainerViewWidth: the element in ECX, the width in XMM0; the
// original changes only EAX, ECX and XMM0, and its LTCG callers may keep
// values in any other register across it (Element::WriteData keeps one in
// EDX, Steam 0x005498BA). An opted-in element gets the 4:3 width; any other
// goes on into the original as called.
bool __cdecl retail_view_width(const uint8_t* element, float* out)
{
   float width, height;
   if (!laid_out_wide(element) || !screen_size(width, height)) return false;
   *out = layout_width(height);
   return true;
}

__declspec(naked) void hooked_ViewWidthRetail()
{
   __asm {
      pushad
      sub    esp, 0x84                  // the width at [esp], then XMM0-7
      movups [esp + 0x04], xmm0
      movups [esp + 0x14], xmm1
      movups [esp + 0x24], xmm2
      movups [esp + 0x34], xmm3
      movups [esp + 0x44], xmm4
      movups [esp + 0x54], xmm5
      movups [esp + 0x64], xmm6
      movups [esp + 0x74], xmm7
      mov    eax, esp
      push   eax
      push   ecx
      call   retail_view_width
      add    esp, 8
      movups xmm1, [esp + 0x14]
      movups xmm2, [esp + 0x24]
      movups xmm3, [esp + 0x34]
      movups xmm4, [esp + 0x44]
      movups xmm5, [esp + 0x54]
      movups xmm6, [esp + 0x64]
      movups xmm7, [esp + 0x74]
      test   al, al
      jz     stock
      movss  xmm0, dword ptr [esp]
      add    esp, 0x84
      popad
      ret
   stock:
      movups xmm0, [esp + 0x04]
      add    esp, 0x84
      popad
      jmp    dword ptr [s_viewWidthRetail]   // ECX as the caller set it
   }
}

// Retail relative-to-pixels conversion: the mode in ECX, value, frame and
// view in XMM1-3, the screen width on the stack (the caller pops), the result
// in XMM0; the original changes only XMM0 and XMM1. A Screen-mode conversion
// is handed the width screen_for picks, through a copy of the argument; the
// rest go straight on.
float __cdecl retail_screen_for(int mode, float view, float screen)
{
   return screen_for(mode, view, screen);
}

static_assert(kModeScreen == 1, "hooked_ToPixelsRetail's compare");
__declspec(naked) void hooked_ToPixelsRetail()
{
   __asm {
      cmp    ecx, 1
      jne    stock
      push   eax                        // becomes the screen width handed on
      pushad
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      // Above the XMM registers: the 0x20 of PUSHAD, the copy at 0xA0, the
      // return address at 0xA4 and the caller's screen width at 0xA8.
      push   dword ptr [esp + 0xA8]
      sub    esp, 4
      movss  dword ptr [esp], xmm3      // view
      push   ecx                        // mode
      call   retail_screen_for
      add    esp, 12
      fstp   dword ptr [esp + 0xA0]
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      add    esp, 0x80
      popad
      call   dword ptr [s_toPixelsRetail]   // ECX and XMM1-3 as the caller set them
      add    esp, 4
      ret
   stock:
      jmp    dword ptr [s_toPixelsRetail]
   }
}

bool tracked(uint32_t hash)
{
   for (uint32_t h : s_tracked)
      if (h == hash) return true;
   return false;
}

bool is_reticule(uint32_t hash)
{
   return hash == s_reticule[0] || hash == s_reticule[1];
}

// A world-tracking position for an opted-in element: the same event with the
// fraction that lands on the real point (and the reticule's y as the engine
// worked it out, before ReticleCorrection).
bool retarget_position(const uint32_t* event, uint8_t* element, float out[3])
{
   __try {
      const uint32_t* cls = reinterpret_cast<const uint32_t*>(event[0]);
      if (!cls || cls[1] != kTypeVector3 || !tracked(cls[0])) return false;
      const uint8_t mode = element[kMode];
      if (mode != kModeScreen && mode != kModeViewport) return false;
      if (!laid_out_wide(element)) return false;
      float width, height;
      if (!wide(width, height)) return false;
      const float* v = reinterpret_cast<const float*>(event[1]);
      out[0] = tracked_fraction(v[0], width, layout_width(height), slide_above(element));
      out[1] = is_reticule(cls[0]) ? hud_widescreen_reticle_uncorrect(v[1]) : v[1];
      out[2] = v[2];
      return true;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

void __cdecl hooked_Position(const uint32_t* event, uint8_t* element)
{
   float v[3];
   if (event && element && s_placedCount && retarget_position(event, element, v)) {
      const uint32_t moved[2] = { event[0], reinterpret_cast<uint32_t>(v) };
      s_position(moved, element);
      return;
   }
   s_position(event, element);
}

void __fastcall hooked_Draw(uint8_t* element, void* edx, const float* parent, uint32_t color)
{
   if (!s_placedCount) {
      s_draw(element, edx, parent, color);
      return;
   }
   const int depth = s_drawDepth;
   const bool wasInView = s_inViewGroup;
   const Placed* wasContainer = s_drawContainer;
   const float* use = parent;
   float matrix[16];
   float width, height;
   if (depth == 0) {
      s_inViewGroup = is_view_group(element);
   } else if (depth == 1) {
      s_drawContainer = nullptr;
      const Placed* p = s_inViewGroup ? placed_red(element, nullptr) : nullptr;
      if (p && wide(width, height)) {
         pixel_parent(matrix, width, height, *s_frustum, p->isContainer ? 0.0f : p->slide);
         use = matrix;
         if (p->isContainer) s_drawContainer = p;
      }
   } else if (depth == 2 && s_drawContainer) {
      const Placed* child = placed_red(element, s_drawContainer->red);
      if (child && child->slide != 0.0f && screen_size(width, height)) {
         memcpy(matrix, parent, sizeof(matrix));
         matrix[12] += child->slide * (640.0f / width);   // pixels to the HUD plane's units
         use = matrix;
      }
   }
   s_drawDepth = depth + 1;
   s_draw(element, edx, use, color);
   s_drawDepth = depth;
   s_inViewGroup = wasInView;
   s_drawContainer = wasContainer;
}

// ---- the aspect -------------------------------------------------------------------

// Stands in for GetScreenAspectRatio where an element's bitmap or minimap is
// set up: 4:3 for an opted-in element, so nothing is stretched, else the real
// ratio.
float __cdecl aspect_for(const uint8_t* element)
{
   return laid_out_wide(element) ? kFourThreeAspect : s_aspect();
}

// cdecl -> ST0 like GetScreenAspectRatio, which touches nothing else: keeps
// every register and the flags.
__declspec(naked) void aspect_esi()
{
   __asm {
      pushfd
      pushad
      push esi
      call aspect_for
      add esp, 4
      popad
      popfd
      ret
   }
}

__declspec(naked) void aspect_ebx()
{
   __asm {
      pushfd
      pushad
      push ebx
      call aspect_for
      add esp, 4
      popad
      popfd
      ret
   }
}

// ElementBarSegmented::SetValue holds its ElementBar base, 0x200 into the
// element, in EBX. The segments sit on an ellipse whose vertical radius it
// scales by 0.75 / aspect to keep a ring round when the width grew with the
// screen; an opted-in bar was laid out at 4:3, so its factor is 1.
static_assert(kBarSegmentedBase == 0x200, "aspect_bar_segmented's offset");
__declspec(naked) void aspect_bar_segmented()
{
   __asm {
      pushfd
      pushad
      lea eax, [ebx - 0x200]
      push eax
      call aspect_for
      add esp, 4
      popad
      popfd
      ret
   }
}

// Retail: the same, keeping the XMM registers too. LTCG knows the original
// changes nothing but ST0, so its callers may keep values in any register
// across it. The element is in ESI at the bitmap rect setup's call and
// ElementMap's; ElementBarSegmented::SetValue holds its ElementBar base in
// EDI.
__declspec(naked) void aspect_esi_retail()
{
   __asm {
      pushfd
      pushad
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      push   esi
      call   aspect_for
      add    esp, 4
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      add    esp, 0x80
      popad
      popfd
      ret
   }
}

__declspec(naked) void aspect_bar_segmented_retail()
{
   __asm {
      pushfd
      pushad
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      lea    eax, [edi - 0x200]
      push   eax
      call   aspect_for
      add    esp, 4
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      add    esp, 0x80
      popad
      popfd
      ret
   }
}

// ---- install ---------------------------------------------------------------------

bool code_is(uintptr_t base, uintptr_t va, const char* what, const char* bytes, size_t length)
{
   __try {
      if (memcmp(resolve(base, va), bytes, length) == 0) return true;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   install_log("[TrueWidescreen] NOT installed: unexpected code at %s 0x%08X", what, (unsigned)va);
   return false;
}

// An absolute address in retail code: the loader has moved it with the exe,
// so it is checked against the build-time address it names, moved the same.
struct Moved {
   size_t    at;   // its offset in the compared bytes
   uintptr_t va;   // the build-time address it names; 0 for none
};

// Retail code as the loader left it: `bytes` wherever `mask` has 'x' ('?'
// marks a CALL's relative target, or an address that is not checked), and
// each moved operand naming its address.
bool retail_code_is(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask,
                    Moved first = {}, Moved second = {})
{
   bool same = true;
   __try {
      const uint8_t* code = static_cast<const uint8_t*>(resolve(base, va));
      for (size_t i = 0; mask[i]; ++i)
         if (mask[i] == 'x' && code[i] != static_cast<uint8_t>(bytes[i])) same = false;
      const Moved moved[] = { first, second };
      for (const Moved& m : moved) {
         if (!m.va) continue;
         const uint32_t want = (uint32_t)(uintptr_t)resolve(base, m.va);
         uint32_t have;
         memcpy(&have, code + m.at, sizeof(have));
         if (have != want) same = false;
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      same = false;
   }
   if (!same) install_log("[TrueWidescreen] NOT installed: unexpected code at %s 0x%08X", what, (unsigned)va);
   return same;
}

// The element draw is shared: another detour on it (the HUD diagnostic) has
// replaced its first three instructions, six bytes on every build, with a
// JMP rel32 and INT3s, and Detours chains onto that. The rest of the prologue
// still has to match. `mask` as for retail_code_is.
constexpr size_t kDrawStolen = 6;

bool draw_is(uintptr_t base, uintptr_t va, const char* bytes, const char* mask)
{
   bool same = false;
   __try {
      const uint8_t* code = static_cast<const uint8_t*>(resolve(base, va));
      for (size_t from = 0; !same && from <= kDrawStolen; from += kDrawStolen) {
         if (from && code[0] != 0xE9) break;
         same = true;
         for (size_t i = from; mask[i]; ++i)
            if (mask[i] == 'x' && code[i] != static_cast<uint8_t>(bytes[i])) same = false;
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      same = false;
   }
   if (!same)
      install_log("[TrueWidescreen] NOT installed: unexpected code at RedInterfaceElement draw 0x%08X", (unsigned)va);
   return same;
}

// A CALL rel32 to GetScreenAspectRatio.
bool aspect_call(uintptr_t base, uintptr_t site)
{
   __try {
      const uint8_t* call = static_cast<const uint8_t*>(resolve(base, site));
      int32_t rel;
      memcpy(&rel, call + 1, sizeof(rel));
      if (call[0] == 0xE8 && (uintptr_t)(call + 5) + rel == (uintptr_t)resolve(base, g_addr->renderer_screen_aspect))
         return true;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   install_log("[TrueWidescreen] NOT installed: no GetScreenAspectRatio call at 0x%08X", (unsigned)site);
   return false;
}

// A vtable slot holding a function, directly (retail) or through an
// incremental-link JMP rel32 (modtools). Both are read as the loader left
// them, so both sides are moved addresses.
bool vtable_slot(uintptr_t base, uintptr_t vtable, uint32_t slot, uintptr_t function, const char* what)
{
   __try {
      const uint8_t* at = *static_cast<const uint8_t* const*>(resolve(base, vtable + slot));
      if (at[0] == 0xE9) {
         int32_t rel;
         memcpy(&rel, at + 1, sizeof(rel));
         at += 5 + rel;
      }
      if (at == static_cast<const uint8_t*>(resolve(base, function))) return true;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   install_log("[TrueWidescreen] NOT installed: %s is not where its vtable says", what);
   return false;
}

void retarget(uintptr_t base, uintptr_t site, void (*to)())
{
   uint8_t* call = static_cast<uint8_t*>(resolve(base, site));
   const int32_t rel = (int32_t)((uintptr_t)to - (uintptr_t)(call + 5));
   memcpy(call + 1, &rel, sizeof(rel));
}

void name_hashes()
{
   char name[64];
   int n = 0;
   for (int w = 1; w <= 2; ++w) {
      _snprintf_s(name, sizeof(name), _TRUNCATE, "player1.weapon%d.reticule.position", w);
      s_tracked[n++] = s_reticule[w - 1] = pbl_hash(name);
      _snprintf_s(name, sizeof(name), _TRUNCATE, "player1.weapon%d.lockOnPosition", w);
      s_tracked[n++] = pbl_hash(name);
      _snprintf_s(name, sizeof(name), _TRUNCATE, "player1.weapon%d.target.position", w);
      s_tracked[n++] = pbl_hash(name);
   }
   for (int post = 1; post <= 16; ++post) {
      _snprintf_s(name, sizeof(name), _TRUNCATE, "player1.commandPost%d.position", post);
      s_tracked[n++] = pbl_hash(name);
   }
}

// Every piece as modtools has it, the editor's included.
bool modtools_code_matches(uintptr_t base)
{
   const auto& a = *g_addr;
   return code_is(base, a.hud_file_info_read_data, "FileInfo::ReadData", "\x53\x55\x8B\x6C\x24\x10\x8B\x45\x00\x3D\xEC\x7E\xE3\x70", 14) &&
          code_is(base, a.hud_manager_load, "HUD::Manager::Load", "\x8B\x44\x24\x04\x81\xEC\x98\x02\x00\x00\x53\x55\x56\x57", 14) &&
          code_is(base, a.hud_container_view_width, "GetContainerViewWidth", "\x51\xE8\x14\xE7\xD7\xFF\x84\xC0\x74\x08\xD9\x05\xFC\x38\xBA\x00", 16) &&
          code_is(base, a.hud_relative_to_pixels, "the relative-to-pixels conversion", "\x8B\x44\x24\x04\x83\xF8\x03\x77\x22\xFF\x24\x85\x50\x11\x69\x00", 16) &&
          code_is(base, a.hud_event_position, "the EventPosition handler", "\x83\xEC\x10\x56\x8B\x74\x24\x18\x8B\xCE", 10) &&
          draw_is(base, a.hud_element_draw, "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\x94\x00\x00\x00\x53\x8B\xD9", "xxxxxxxxxxxxxxx") &&
          code_is(base, a.renderer_screen_aspect, "GetScreenAspectRatio", "\xD9\x05\x18\x2E\xD6\x00\xC3", 7) &&
          code_is(base, a.hud_file_info_write_data, "FileInfo::WriteData", "\x53\x55\x56\x8B\xF1\x8B\x46\x30\x85\xC0\x57\x8B\x7C\x24\x14", 15) &&
          code_is(base, a.hud_pixels_to_relative, "the pixels-to-relative conversion", "\x8B\x44\x24\x04\x83\xF8\x03\x77\x62\xFF\x24\x85\xE0\x11\x69\x00", 16) &&
          code_is(base, a.hud_write_indent, "the .hud indent writer", "\x56\x8B\x74\x24\x08\x85\xF6\x57\x8B\xF9", 10) &&
          code_is(base, a.hud_write_format, "the .hud format writer", "\x8B\x4C\x24\x08\x81\xEC\x00\x04\x00\x00", 10) &&
          code_is(base, a.hud_text_set_property, "ElementText::SetProperty", "\x8B\x44\x24\x04\x83\xEC\x08\x3D\x5A\xFA\xF9\x75\x56\x57\x8B\xF1", 16) &&
          code_is(base, a.hud_text_get_property, "ElementText::GetProperty", "\x51\x8B\x44\x24\x08\x3D\x5A\xFA\xF9\x75", 10) &&
          code_is(base, a.hud_text_write_data, "ElementText::WriteData", "\x83\xEC\x14\x53\x55\x56\x57\x8B\xF1\x8D\x44\x24\x18\x50", 14) &&
          vtable_slot(base, a.hud_file_info_vtable, 0x28, a.hud_file_info_write_data, "FileInfo WriteData") &&
          code_is(base, a.hud_manager_load_read_call, "Manager::Load's read of a top-level item",
                  "\x8B\xCF\xFF\x52\x08\x81\xFD\x97\xC7\xE0\xD4", 11);
}

// The runtime pieces as Steam and GOG have them, the same bytes on both but
// for the addresses the loader moves. Load's guard runs to its first use of
// gHudViewPorts and reads CameraManager's camera count (+0x1C) on the way;
// GetContainerViewWidth's names sViewportWidth and the screen's width; the
// conversion's jump table sits 0x2C into it.
bool retail_code_matches(uintptr_t base)
{
   const auto& a = *g_addr;
   return retail_code_is(base, a.hud_file_info_read_data, "FileInfo::ReadData",
                         "\x55\x8B\xEC\x53\x56\x8B\xF1\x8B\x4D\x0C\x57\x8B\x01\x3D\xEC\x7E\xE3\x70",
                         "xxxxxxxxxxxxxxxxxx") &&
          retail_code_is(base, a.hud_manager_load, "HUD::Manager::Load",
                         "\x55\x8B\xEC\x81\xEC\x9C\x02\x00\x00\x53\x56\x57\x51\x8D\x4D\xD0\xE8\x00\x00\x00\x00\xA1"
                         "\x00\x00\x00\x00\x32\xD2\x8B\x58\x1C\x8B\xCB\xE8\x00\x00\x00\x00\x33\xFF\x85\xDB\x0F\x84"
                         "\x41\x01\x00\x00\xBE\xA0\x00\x00\x00\xEB\x09\x8D\xA4\x24\x00\x00\x00\x00\x8B\xFF\x8B\x0D"
                         "\x00\x00\x00\x00\x8D\x0C",
                         "xxxxxxxxxxxxxxxxx????x????xxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxx????xx",
                         { 0x16, a.camera_manager_instance }, { 0x42, a.hud_view_groups }) &&
          retail_code_is(base, a.hud_manager_load_read_call, "Manager::Load's read of a top-level item",
                         "\x8B\xCE\xFF\x50\x08\x81\xFF\x97\xC7\xE0\xD4", "xxxxxxxxxxx") &&
          retail_code_is(base, a.hud_container_view_width, "GetContainerViewWidth",
                         "\xE8\x00\x00\x00\x00\x84\xC0\x74\x09\xF3\x0F\x10\x05\x00\x00\x00\x00\xC3\xA1\x00\x00\x00\x00",
                         "x????xxxxxxxx????xx????",
                         { 0x0D, a.hud_viewport_width }, { 0x13, a.hud_screen_width }) &&
          retail_code_is(base, a.hud_relative_to_pixels, "the relative-to-pixels conversion",
                         "\x55\x8B\xEC\x83\xF9\x03\x77\x1E\xFF\x24\x8D\x00\x00\x00\x00\xF3\x0F\x59\xCB\x0F\x28\xC1"
                         "\x5D\xC3\xF3\x0F\x59\x4D\x08\x0F\x28\xC1\x5D\xC3\xF3\x0F\x59\xCA\x0F\x28\xC1\x5D\xC3",
                         "xxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxx",
                         { 0x0B, a.hud_relative_to_pixels + 0x2C }) &&
          retail_code_is(base, a.hud_event_position, "the EventPosition handler",
                         "\x55\x8B\xEC\x8B\x55\x08\x83\xEC\x10\x8B\xCA\xE8\x00\x00\x00\x00\x8B\xC8\xE8\x00\x00\x00\x00"
                         "\x83\xF8\x09",
                         "xxxxxxxxxxxx????xxx????xxx") &&
          draw_is(base, a.hud_element_draw,
                  "\x53\x8B\xDC\x83\xEC\x08\x83\xE4\xF0\x83\xC4\x04\x55\x8B\x6B\x04\x89\x6C\x24\x04\x8B\xEC\x83\xEC"
                  "\x58\xA1\x00\x00\x00\x00\x33\xC5\x89\x45\xFC\x8B\x43\x08\x56\x57\x8B\xF9",
                  "xxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxx") &&
          retail_code_is(base, a.renderer_screen_aspect, "GetScreenAspectRatio", "\xD9\x05\x00\x00\x00\x00\xC3",
                         "xx????x");
}

} // namespace

void hud_true_widescreen_install(uintptr_t base)
{
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   const auto& a = *g_addr;
   if (!a.hud_file_info_read_data || !a.hud_manager_load || !a.hud_container_view_width ||
       !a.hud_relative_to_pixels || !a.hud_event_position || !a.hud_element_draw || !a.hud_element_list ||
       !a.hud_view_groups || !a.hud_interface_frustum || !a.renderer_screen_aspect || !a.hud_screen_width ||
       !a.camera_manager_instance || !a.hud_bitmap_rect_aspect_call || !a.hud_map_aspect_call_1 ||
       !a.hud_map_aspect_call_2 || !a.hud_map_aspect_call_3 || !a.hud_map_aspect_call_4 ||
       !a.hud_file_info_vtable || !a.hud_viewport_width || !a.hud_target_vtable || !a.hud_target_update ||
       !a.hud_bar_segmented_aspect_call || !a.hud_manager_load_read_call ||
       (modtools && (!a.hud_file_info_write_data || !a.hud_write_indent || !a.hud_write_format ||
                     !a.hud_pixels_to_relative || !a.hud_config_files || !a.hud_config_file_count ||
                     !a.hud_text_set_property || !a.hud_text_get_property || !a.hud_text_write_data))) {
      install_log("[TrueWidescreen] NOT installed: no address set for this build");
      return;
   }
   if (!(modtools ? modtools_code_matches(base) : retail_code_matches(base)) ||
       !vtable_slot(base, a.hud_file_info_vtable, 0x20, a.hud_file_info_read_data, "FileInfo ReadData") ||
       !vtable_slot(base, a.hud_target_vtable, 0x2C, a.hud_target_update, "ElementTarget::Update"))
      return;
   const uintptr_t aspectSites[] = { a.hud_bitmap_rect_aspect_call, a.hud_map_aspect_call_1, a.hud_map_aspect_call_2,
                                     a.hud_map_aspect_call_3, a.hud_map_aspect_call_4,
                                     a.hud_bar_segmented_aspect_call };
   for (uintptr_t site : aspectSites)
      if (!aspect_call(base, site)) return;

   s_screen     = static_cast<const int*>(resolve(base, a.hud_screen_width));
   s_listEnd    = static_cast<const uintptr_t*>(resolve(base, a.hud_element_list));
   s_viewGroups = static_cast<const uintptr_t*>(resolve(base, a.hud_view_groups));
   s_frustum    = static_cast<const float*>(resolve(base, a.hud_interface_frustum));
   s_cameras    = static_cast<const uint8_t* const*>(resolve(base, a.camera_manager_instance));
   s_aspect     = reinterpret_cast<AspectFn>(resolve(base, a.renderer_screen_aspect));
   s_fileInfoVtable = (uint32_t)(uintptr_t)resolve(base, a.hud_file_info_vtable);
   s_targetVtable = (uint32_t)(uintptr_t)resolve(base, a.hud_target_vtable);
   s_viewportWidth = static_cast<float*>(resolve(base, a.hud_viewport_width));
   name_hashes();

   s_readData  = reinterpret_cast<ReadDataFn>(resolve(base, a.hud_file_info_read_data));
   s_position  = reinterpret_cast<PositionFn>(resolve(base, a.hud_event_position));
   s_draw      = reinterpret_cast<DrawFn>(resolve(base, a.hud_element_draw));
   if (modtools) {
      s_configFiles = static_cast<const void* const*>(resolve(base, a.hud_config_files));
      s_configCount = static_cast<const int*>(resolve(base, a.hud_config_file_count));
      s_indent     = reinterpret_cast<IndentFn>(resolve(base, a.hud_write_indent));
      s_format     = reinterpret_cast<FormatFn>(resolve(base, a.hud_write_format));
      s_load       = reinterpret_cast<LoadFn>(resolve(base, a.hud_manager_load));
      s_viewWidth  = reinterpret_cast<ViewWidthFn>(resolve(base, a.hud_container_view_width));
      s_toPixels   = reinterpret_cast<ToPixelsFn>(resolve(base, a.hud_relative_to_pixels));
      s_writeData  = reinterpret_cast<WriteDataFn>(resolve(base, a.hud_file_info_write_data));
      s_toRelative = reinterpret_cast<ToPixelsFn>(resolve(base, a.hud_pixels_to_relative));
      s_textSet    = reinterpret_cast<TextCallFn>(resolve(base, a.hud_text_set_property));
      s_textGet    = reinterpret_cast<TextCallFn>(resolve(base, a.hud_text_get_property));
      s_textWrite  = reinterpret_cast<TextCallFn>(resolve(base, a.hud_text_write_data));
   } else {
      s_loadRetail      = reinterpret_cast<LoadRetailFn>(resolve(base, a.hud_manager_load));
      s_viewWidthRetail = resolve(base, a.hud_container_view_width);
      s_toPixelsRetail  = resolve(base, a.hud_relative_to_pixels);
   }
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)s_readData, hooked_ReadData);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_position, hooked_Position);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_draw, hooked_Draw);
   if (modtools) {
      if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_load, hooked_Load);
      if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_viewWidth, hooked_ViewWidth);
      if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_toPixels, hooked_ToPixels);
      if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_writeData, hooked_WriteData);
      if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_toRelative, hooked_ToRelative);
      if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_textSet, hooked_TextSet);
      if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_textGet, hooked_TextGet);
      if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_textWrite, hooked_TextWrite);
   } else {
      if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_loadRetail, hooked_LoadRetail);
      if (r == NO_ERROR) r = DetourAttach(&s_viewWidthRetail, hooked_ViewWidthRetail);
      if (r == NO_ERROR) r = DetourAttach(&s_toPixelsRetail, hooked_ToPixelsRetail);
   }
   if (r != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[TrueWidescreen] NOT installed: DetourAttach failed (%ld)", (long)r);
      return;
   }
   if (DetourTransactionCommit() != NO_ERROR) {
      install_log("[TrueWidescreen] NOT installed: the detour transaction failed");
      return;
   }
   if (modtools) {
      retarget(base, a.hud_bitmap_rect_aspect_call, aspect_esi);
      retarget(base, a.hud_map_aspect_call_1, aspect_ebx);
      retarget(base, a.hud_map_aspect_call_2, aspect_ebx);
      retarget(base, a.hud_map_aspect_call_3, aspect_ebx);
      retarget(base, a.hud_map_aspect_call_4, aspect_ebx);
      retarget(base, a.hud_bar_segmented_aspect_call, aspect_bar_segmented);
   } else {
      retarget(base, a.hud_bitmap_rect_aspect_call, aspect_esi_retail);
      retarget(base, a.hud_map_aspect_call_1, aspect_esi_retail);
      retarget(base, a.hud_map_aspect_call_2, aspect_esi_retail);
      retarget(base, a.hud_map_aspect_call_3, aspect_esi_retail);
      retarget(base, a.hud_map_aspect_call_4, aspect_esi_retail);
      retarget(base, a.hud_bar_segmented_aspect_call, aspect_bar_segmented_retail);
   }
   // MOV ECX,EDI / CALL [EDX+8] (modtools) or MOV ECX,ESI / CALL [EAX+8]
   // (retail), 5 bytes, becomes CALL read_top_item(_retail).
   uint8_t* readCall = static_cast<uint8_t*>(resolve(base, a.hud_manager_load_read_call));
   void (*const readTop)() = modtools ? read_top_item : read_top_item_retail;
   const int32_t rel = (int32_t)((uintptr_t)readTop - (uintptr_t)(readCall + 5));
   readCall[0] = 0xE8;
   memcpy(readCall + 1, &rel, sizeof(rel));
   install_log("[TrueWidescreen] installed: .hud files with TrueWidescreen(1) in their FileInfo are laid out "
               "4:3 at the screen's height and kept to their nearest edge on wide screens");
}
