#pragma once
//
// SetInstanceProperty - reach a world object's [InstanceProperties] at runtime.
//
// Stock Lua has no route to these objects at all.  Lua_Callbacks::SetProperty
// resolves its first argument through EntityEx::mIdMap_ and then RTTI-checks the
// result against EntityEx, so anything deriving from Entity but not EntityEx is
// invisible to it.  VehicleSpawn is one such family; EntitySound is another.
//
// This is deliberately written as a registry of families rather than a vehicle
// spawn function, because the wall is structural and shared.  Adding a family
// means adding one finder that maps a name to an object plus the engine's own
// SetProperty for it.
//
#include <cstdint>

// Applies `prop` = `value` to every supported world object named `name`.
// Returns the number of objects written, 0 if the name matched nothing.
int instance_props_set(const char* name, const char* prop, const char* value);
