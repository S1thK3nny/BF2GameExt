#pragma once

#include <stdint.h>

// =============================================================================
// CommandWalker::Kill double-call guard.
//
// Observed crash (modtools, a carrier shot down while carrying an AT-AT):
//
//   EXCEPTION C0000005 ACCESS_VIOLATION  EIP=0064BB26   AV: READ addr 00000000
//   ESI=00000000 ECX=00000000
//
//   EntityWalker::Update  0055801B  CALL [EAX+4]          ; Damageable::Kill
//   CommandWalker::Kill   006508B8  MOV ECX,[ESI+0x2000]  ; mobile command post
//                         006508BE  CALL -> 0064BB20
//                         0064BB26  MOV EAX,[ESI]         ; ESI = post = NULL
//
// CommandWalker::Kill kills the walker's mobile command post and then zeroes
// the pointer, so a second Kill on the same walker dereferences NULL.  The
// destructor null-checks the post; Kill does not.
//
// Two Kills happen when anything kills the walker directly.
// EntityWalker::Kill clears the alive bit (Damageable+0xBC bit 3), but only
// EntityWalker::Update's own death branch follows it with Die(), which sets the
// dead flag.  So the next Update sees "not alive, not dead" and runs its death
// branch: Kill() again, then Die().  EntityCarrier::Kill is such a direct
// caller: it kills every attached cargo (Controllable vtable slot 5, a thunk to
// the Damageable Kill) before detaching it.  Stock maps never put a
// CommandWalker on a carrier, so vanilla never reached it.  Lua KillObject is
// another (SetCurHealth(0), then the Damageable Kill directly).
//
// The other classes that own a mobile command post (CommandHover, the command
// buildings) are safe: their Kill passes the post to the same function but
// never zeroes the pointer, and that function returns at once when the post's
// thread is no longer active (Thread::IsActive, vtable slot 4), which the first
// call's DeactivateThread (slot 3) guarantees.  Only CommandWalker zeroes it.
//
// The post is built in the constructor (CommandPost::BuildPost always returns
// a slot), so a NULL post means Kill has already run.  The guard makes the
// second call a no-op; Update's Die() still follows and finishes the death.
//
// All three builds.  The post sits at Damageable+0x2000 on modtools and
// +0x1FC0 on Steam and GOG, which share one address, 0x0047FFE0.
// =============================================================================

void command_walker_kill_fix_install(uintptr_t exe_base);
void command_walker_kill_fix_uninstall();
