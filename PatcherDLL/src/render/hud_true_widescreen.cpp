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
//   FileInfo::ReadData                0x006B7F50
//   HUD::Manager::Load                0x006B8480
//   GetContainerViewWidth             0x006926B0
//   relative-to-pixels conversion     0x00691120
//   EventPosition handler             0x0069A350
//   RedInterfaceElement draw          0x00816FA0
//   HUD::Element::sList terminator    0x00AD7EE0
//   gHudViewPorts cell                0x00BA4478
//   interface frustum width t         0x00E5B504
//   GetScreenAspectRatio              0x008059A0
//   aspect call, bitmap rect setup    0x006988F9 (element in ESI)
//   aspect calls, ElementMap          0x0069BA00, 0x0069BA4E, 0x0069BA66,
//                                     0x0069BAA3 (element in EBX)
//   FileInfo WriteData                0x006B8090
//   FileInfo vtable                   0x00A60398
//   indent writer / format writer     0x006B5A20 / 0x006B5A50
//   pixels-to-relative conversion     0x00691170
//   gConfigFiles / count              0x00BA4070 / 0x00BA4480
//   HUD::Element::sViewportWidth      0x00BA38FC
//   ElementText Set/GetProperty       0x006AAE10 / 0x006A95C0
//   ElementText WriteData             0x006A9A90
//   ElementTarget vtable              0x00A5E000
//   aspect call, segmented bar        0x00696B97 (element + 0x200 in EBX)
//
// Element layout (modtools): +0xB0 the RedInterfaceElement it draws with, +0xB4
// its sList node (next at +0), +0x158 its RelativeMode. RedInterfaceElement:
// local matrix at +0x30, the parent's child list at +0x1C (null when it has no
// parent), which RedGroupElement keeps at +0x70; the pieces are found in that
// tree, the one the draw walks, since HUD::Element's group (+0xF4) is only set
// for some. An Event is { EventClass*, payload* }; an EventClass starts with
// its name's PblHash and its type (9 = Vector3).
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
         if (on) remember_file(self);
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

void __cdecl hooked_Load(void* config)
{
   float width = 0.0f, height = 0.0f;
   const bool isWide = wide(width, height);
   if (s_placedCount) prune();
   const uintptr_t tail = s_listEnd[1];   // the last node, or the terminator itself when empty
   s_topItemCount = 0;
   s_loading = true;
   s_loadingWide = isWide;
   s_fileOptedIn = false;
   s_load(config);
   return_viewport_width();
   const bool optedIn = s_fileOptedIn && isWide;
   s_loading = false;
   s_fileOptedIn = false;
   if (optedIn) place(tail, width, height);
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

// The element draw is shared: another detour on it (the HUD diagnostic) has
// replaced its first three instructions, six bytes, with a JMP rel32 and
// INT3s, and Detours chains onto that. The rest of the prologue still has to
// match.
constexpr size_t kDrawStolen = 6;

bool draw_is(uintptr_t base, uintptr_t va, const char* bytes, size_t length)
{
   __try {
      const uint8_t* code = static_cast<const uint8_t*>(resolve(base, va));
      if (memcmp(code, bytes, length) == 0) return true;
      if (code[0] == 0xE9 && memcmp(code + kDrawStolen, bytes + kDrawStolen, length - kDrawStolen) == 0) return true;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   install_log("[TrueWidescreen] NOT installed: unexpected code at RedInterfaceElement draw 0x%08X", (unsigned)va);
   return false;
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

// A vtable slot holding a function, directly or through an incremental-link
// JMP rel32 (modtools).
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
   install_log("[TrueWidescreen] NOT installed: %s is not where the FileInfo vtable says", what);
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

} // namespace

void hud_true_widescreen_install(uintptr_t base)
{
   if (g_build != GameBuild::Modtools) {
      if (g_build == GameBuild::Steam || g_build == GameBuild::GOG)
         install_log("[TrueWidescreen] not on this build yet: TrueWidescreen files keep the stock layout");
      return;
   }
   const auto& a = *g_addr;
   if (!a.hud_file_info_read_data || !a.hud_manager_load || !a.hud_container_view_width ||
       !a.hud_relative_to_pixels || !a.hud_event_position || !a.hud_element_draw || !a.hud_element_list ||
       !a.hud_view_groups || !a.hud_interface_frustum || !a.renderer_screen_aspect || !a.hud_screen_width ||
       !a.camera_manager_instance || !a.hud_bitmap_rect_aspect_call || !a.hud_map_aspect_call_1 ||
       !a.hud_map_aspect_call_2 || !a.hud_map_aspect_call_3 || !a.hud_map_aspect_call_4 ||
       !a.hud_file_info_write_data || !a.hud_file_info_vtable || !a.hud_write_indent || !a.hud_write_format ||
       !a.hud_pixels_to_relative || !a.hud_config_files || !a.hud_config_file_count || !a.hud_viewport_width ||
       !a.hud_text_set_property || !a.hud_text_get_property || !a.hud_text_write_data || !a.hud_target_vtable ||
       !a.hud_target_update || !a.hud_bar_segmented_aspect_call || !a.hud_manager_load_read_call) {
      install_log("[TrueWidescreen] NOT installed: no address set for this build");
      return;
   }
   if (!code_is(base, a.hud_file_info_read_data, "FileInfo::ReadData", "\x53\x55\x8B\x6C\x24\x10\x8B\x45\x00\x3D\xEC\x7E\xE3\x70", 14) ||
       !code_is(base, a.hud_manager_load, "HUD::Manager::Load", "\x8B\x44\x24\x04\x81\xEC\x98\x02\x00\x00\x53\x55\x56\x57", 14) ||
       !code_is(base, a.hud_container_view_width, "GetContainerViewWidth", "\x51\xE8\x14\xE7\xD7\xFF\x84\xC0\x74\x08\xD9\x05\xFC\x38\xBA\x00", 16) ||
       !code_is(base, a.hud_relative_to_pixels, "the relative-to-pixels conversion", "\x8B\x44\x24\x04\x83\xF8\x03\x77\x22\xFF\x24\x85\x50\x11\x69\x00", 16) ||
       !code_is(base, a.hud_event_position, "the EventPosition handler", "\x83\xEC\x10\x56\x8B\x74\x24\x18\x8B\xCE", 10) ||
       !draw_is(base, a.hud_element_draw, "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\x94\x00\x00\x00\x53\x8B\xD9", 15) ||
       !code_is(base, a.renderer_screen_aspect, "GetScreenAspectRatio", "\xD9\x05\x18\x2E\xD6\x00\xC3", 7) ||
       !code_is(base, a.hud_file_info_write_data, "FileInfo::WriteData", "\x53\x55\x56\x8B\xF1\x8B\x46\x30\x85\xC0\x57\x8B\x7C\x24\x14", 15) ||
       !code_is(base, a.hud_pixels_to_relative, "the pixels-to-relative conversion", "\x8B\x44\x24\x04\x83\xF8\x03\x77\x62\xFF\x24\x85\xE0\x11\x69\x00", 16) ||
       !code_is(base, a.hud_write_indent, "the .hud indent writer", "\x56\x8B\x74\x24\x08\x85\xF6\x57\x8B\xF9", 10) ||
       !code_is(base, a.hud_write_format, "the .hud format writer", "\x8B\x4C\x24\x08\x81\xEC\x00\x04\x00\x00", 10) ||
       !code_is(base, a.hud_text_set_property, "ElementText::SetProperty", "\x8B\x44\x24\x04\x83\xEC\x08\x3D\x5A\xFA\xF9\x75\x56\x57\x8B\xF1", 16) ||
       !code_is(base, a.hud_text_get_property, "ElementText::GetProperty", "\x51\x8B\x44\x24\x08\x3D\x5A\xFA\xF9\x75", 10) ||
       !code_is(base, a.hud_text_write_data, "ElementText::WriteData", "\x83\xEC\x14\x53\x55\x56\x57\x8B\xF1\x8D\x44\x24\x18\x50", 14) ||
       !vtable_slot(base, a.hud_file_info_vtable, 0x20, a.hud_file_info_read_data, "FileInfo ReadData") ||
       !vtable_slot(base, a.hud_file_info_vtable, 0x28, a.hud_file_info_write_data, "FileInfo WriteData") ||
       !vtable_slot(base, a.hud_target_vtable, 0x2C, a.hud_target_update, "ElementTarget::Update") ||
       !code_is(base, a.hud_manager_load_read_call, "Manager::Load's read of a top-level item",
                "\x8B\xCF\xFF\x52\x08\x81\xFD\x97\xC7\xE0\xD4", 11))
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
   s_configFiles = static_cast<const void* const*>(resolve(base, a.hud_config_files));
   s_configCount = static_cast<const int*>(resolve(base, a.hud_config_file_count));
   s_fileInfoVtable = (uint32_t)(uintptr_t)resolve(base, a.hud_file_info_vtable);
   s_targetVtable = (uint32_t)(uintptr_t)resolve(base, a.hud_target_vtable);
   s_viewportWidth = static_cast<float*>(resolve(base, a.hud_viewport_width));
   s_indent     = reinterpret_cast<IndentFn>(resolve(base, a.hud_write_indent));
   s_format     = reinterpret_cast<FormatFn>(resolve(base, a.hud_write_format));
   name_hashes();

   s_readData  = reinterpret_cast<ReadDataFn>(resolve(base, a.hud_file_info_read_data));
   s_writeData = reinterpret_cast<WriteDataFn>(resolve(base, a.hud_file_info_write_data));
   s_toRelative = reinterpret_cast<ToPixelsFn>(resolve(base, a.hud_pixels_to_relative));
   s_load      = reinterpret_cast<LoadFn>(resolve(base, a.hud_manager_load));
   s_viewWidth = reinterpret_cast<ViewWidthFn>(resolve(base, a.hud_container_view_width));
   s_toPixels  = reinterpret_cast<ToPixelsFn>(resolve(base, a.hud_relative_to_pixels));
   s_position  = reinterpret_cast<PositionFn>(resolve(base, a.hud_event_position));
   s_draw      = reinterpret_cast<DrawFn>(resolve(base, a.hud_element_draw));
   s_textSet   = reinterpret_cast<TextCallFn>(resolve(base, a.hud_text_set_property));
   s_textGet   = reinterpret_cast<TextCallFn>(resolve(base, a.hud_text_get_property));
   s_textWrite = reinterpret_cast<TextCallFn>(resolve(base, a.hud_text_write_data));
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)s_readData, hooked_ReadData);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_load, hooked_Load);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_viewWidth, hooked_ViewWidth);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_toPixels, hooked_ToPixels);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_position, hooked_Position);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_draw, hooked_Draw);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_writeData, hooked_WriteData);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_toRelative, hooked_ToRelative);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_textSet, hooked_TextSet);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_textGet, hooked_TextGet);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_textWrite, hooked_TextWrite);
   if (r != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[TrueWidescreen] NOT installed: DetourAttach failed (%ld)", (long)r);
      return;
   }
   if (DetourTransactionCommit() != NO_ERROR) {
      install_log("[TrueWidescreen] NOT installed: the detour transaction failed");
      return;
   }
   retarget(base, a.hud_bitmap_rect_aspect_call, aspect_esi);
   retarget(base, a.hud_map_aspect_call_1, aspect_ebx);
   retarget(base, a.hud_map_aspect_call_2, aspect_ebx);
   retarget(base, a.hud_map_aspect_call_3, aspect_ebx);
   retarget(base, a.hud_map_aspect_call_4, aspect_ebx);
   retarget(base, a.hud_bar_segmented_aspect_call, aspect_bar_segmented);
   // MOV ECX,EDI / CALL [EDX+8] (5 bytes) becomes CALL read_top_item.
   uint8_t* readCall = static_cast<uint8_t*>(resolve(base, a.hud_manager_load_read_call));
   const int32_t rel = (int32_t)((uintptr_t)&read_top_item - (uintptr_t)(readCall + 5));
   readCall[0] = 0xE8;
   memcpy(readCall + 1, &rel, sizeof(rel));
   install_log("[TrueWidescreen] installed: .hud files with TrueWidescreen(1) in their FileInfo are laid out "
               "4:3 at the screen's height and kept to their nearest edge on wide screens");
}
