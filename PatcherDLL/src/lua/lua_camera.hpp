#pragma once

#include "lua_hooks.hpp"

// The scripted camera's Lua functions (render/scripted_camera.hpp; the
// reference is docs/user/LUA_API.md#camera):
//
//   CameraShot(where [, opts])          CameraLookAt(target [, opts])
//   CameraFollow(unit [, opts])         CameraFOV([fov [, seconds]])
//   CameraPath(path, seconds [, opts])  CameraShake(amount [, seconds])
//   CameraRelease([seconds])            GetCameraMatrix()
//   GetCameraPosition()                 IsCameraScripted()
int lua_CameraShot(lua_State* L);
int lua_CameraFollow(lua_State* L);
int lua_CameraPath(lua_State* L);
int lua_CameraLookAt(lua_State* L);
int lua_CameraFOV(lua_State* L);
int lua_CameraShake(lua_State* L);
int lua_CameraRelease(lua_State* L);
int lua_GetCameraMatrix(lua_State* L);
int lua_GetCameraPosition(lua_State* L);
int lua_IsCameraScripted(lua_State* L);
