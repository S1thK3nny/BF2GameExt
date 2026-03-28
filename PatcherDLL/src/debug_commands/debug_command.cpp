#include "pch.h"
#include "debug_command.hpp"

DebugCommand::DrawLine3D_t    DebugCommand::drawLine3D    = nullptr;
DebugCommand::DrawSphere_t    DebugCommand::drawSphere    = nullptr;
DebugCommand::Printf3D_t      DebugCommand::printf3D      = nullptr;
DebugCommand::FindBody_t      DebugCommand::findBody      = nullptr;
DebugCommand::GetWorldXform_t DebugCommand::getWorldXform = nullptr;
DebugCommand::GetRadius_t     DebugCommand::getRadius     = nullptr;

void DebugCommand::initEngine(uintptr_t exe_base)
{
   using namespace game_addrs::modtools;

   drawLine3D    = (DrawLine3D_t)    resolve(exe_base, draw_line_3d);
   drawSphere    = (DrawSphere_t)    resolve(exe_base, draw_sphere);
   printf3D      = (Printf3D_t)     resolve(exe_base, printf_3d);
   findBody      = (FindBody_t)      resolve(exe_base, find_body);
   getWorldXform = (GetWorldXform_t) resolve(exe_base, get_world_xform);
   getRadius     = (GetRadius_t)     resolve(exe_base, get_radius);
}
