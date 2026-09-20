#pragma once

#include <stdint.h>

// ODF AttachEffect outliving the entity that owned it.
//
// An entity class can pin effects to hardpoints with AttachEffect /
// AttachToHardPoint.  AttachedEffectsClass::BuildEffects creates one
// FLEffectObject per entry and parks it in AttachedEffects::m_aAttachData.  The
// AttachedEffects destructor is only Thread::~Thread plus operator delete - it
// never walks that array - so deleting the entity leaves every one of those
// effects alive.  The orphan keeps reading its owner's world matrix through a
// raw pointer, so the next frame after a DeleteEntity faults on freed memory.
//
// Stock content never trips this: nothing in the stock scripts deletes an entity
// that carries an ODF attach effect, so the path was never exercised.  A mod that
// deletes a prop hits it immediately.
//
// Fix: release the effects the class owns before the destructor runs, which is
// what the destructor was always missing.  Always on - no INI toggle.

void attached_effects_cleanup_install(uintptr_t exe_base);
void attached_effects_cleanup_uninstall();
