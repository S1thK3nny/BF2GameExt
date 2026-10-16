#pragma once

#include <stdint.h>

#include <vector>

#include "render/scripted_camera_core.hpp"

// =============================================================================
// Scripted camera: a mission script takes the player's view. A shot from a
// matrix, a unit or a point; a chase camera on any unit; a flight along a
// ZeroEditor path; a look toward something; a FOV; a shake; and a blend back to
// the player's own camera. The Lua side is lua/lua_camera.cpp, the engine side
// docs/RE/ScriptedCamera.md.
//
// It draws last, in camera shake's ChaseCamera::SetupCamera detour, by handing
// RedCamera::SetMatrix its own view: the chase camera itself, the aim and the
// first-person arms keep the player's. While a shot, a follow or a path holds
// the view the player is shown in third person (a Trackable::CheckFirstPerson
// detour), and their own choice comes back after. Local: nothing reaches the
// simulation or the network. Modtools, Steam and GOG.
// =============================================================================

namespace scripted_camera {

// A unit as a script names one, found again every frame: by its name (a named
// world object, or a pointer to one), by a handle (an unnamed object's pointer,
// dropped once the object is gone), or by a character (its remote, else its
// vehicle, else its soldier).
struct UnitRef {
   enum class Type : uint8_t { None, Name, Handle, Character };
   Type     type = Type::None;
   uint32_t name = 0;          // EntityEx::mId
   uint8_t* object = nullptr;  // Handle: the GameObject...
   uint32_t handle = 0;        // ...and its mHandleId when it was given
   int      character = -1;
   bool given() const { return type != Type::None; }
};

UnitRef unit_from_object(void* object);
UnitRef unit_from_name(const char* name);
UnitRef unit_from_character(int index);

// A place to put or aim a view: a world point, or a unit and a point in its
// frame ({right, up, forward} from its position).
struct Spot {
   enum class Type : uint8_t { None, Point, Unit };
   Type    type = Type::None;
   V3      point;
   UnitRef unit;
   V3      local;
   bool given() const { return type != Type::None; }
};

// What every view order takes.
struct Common {
   Spot  lookAt;            // keep it aimed here
   float fov = 0.0f;        // the game's measure, degrees; 0 leaves the FOV alone
   float blend = 0.0f;      // seconds blending in; 0 cuts
   float time = -1.0f;      // seconds until it releases itself; negative holds
   float release = 1.0f;    // seconds that release blends back over
};

struct Shot : Common {
   bool  fromMatrix = false;
   float matrix[16] = {};   // an object's matrix (rows right, up, forward, position)
   Spot  from;              // or a unit (looking the way it faces) or a point
   V3    offset;            // {right, up, forward} in the matrix's or unit's frame;
                            // for a point, world x, y, z
};

struct Follow : Common {
   UnitRef unit;
   V3    offset{ 0.0f, 4.0f, -12.0f };   // in the unit's frame
   V3    aim{ 0.0f, 1.0f, 0.0f };        // the point on the unit it looks at
   float lag = 0.3f;                     // seconds it trails its spot by
   bool  world = false;                  // the offset in world axes
};

struct Path : Common {
   std::vector<V3>   nodes;
   std::vector<View> nodeViews;   // aim "nodes": each node's facing, else empty
   float seconds = 5.0f;
   Ease  ease = Ease::InOut;
   bool  loop = false;
   float endRelease = -1.0f;      // at the end, release over this; negative holds
};

// A look over gameplay: the player's own view turned toward `target` for a while.
struct Look {
   Spot  target;
   float time = 2.0f;
   float blend = 0.5f;
   float release = 0.5f;
};

void shot(const Shot& order);
void follow(const Follow& order);
void path(const Path& order);
// With a scripted view, aims it (`target` null: back to its order's own aim);
// without one, a look over gameplay.
void look(const Look& order);
void aim(const Spot* target);
void fov(float degrees, float seconds);
void fov_off();
void shake(float amount, float seconds);
void release(float seconds);

bool holds();                     // a script holds the view, a release included
bool shown(View& out);            // the view drawn last frame, scripted or not
bool installed();

// A ZeroEditor path's node positions; false if the map has no such path.
bool read_path(const char* name, std::vector<V3>& nodes);

// A new mission: everything a script set is dropped.
void reset();

// Camera shake's SetupCamera detour calls this last, every frame the chase
// camera sets the view.
void after_setup(uint8_t* chase, void* camera, float dt);

} // namespace scripted_camera

void scripted_camera_install(uintptr_t exe_base);
void scripted_camera_uninstall();
