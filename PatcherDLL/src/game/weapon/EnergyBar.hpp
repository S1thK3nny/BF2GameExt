#pragma once

#include <cstdint>
#include <cstddef>

namespace game {

// -------------------------------------------------------------------------
// EnergyBarClass
// -------------------------------------------------------------------------

struct EnergyBarClass {
    float m_fEnergyMax;       // +0x00
    float m_fEnergyMin;       // +0x04
    float m_fEnergyOverheat;  // +0x08
};
static_assert(sizeof(EnergyBarClass) == 0x0C);

// -------------------------------------------------------------------------
// EnergyBar
// -------------------------------------------------------------------------

struct EnergyBar {
    EnergyBarClass* m_pClass;       // +0x00
    float           m_fEnergy;      // +0x04
    uint32_t        m_bOverheat : 1;  // +0x08 bit 0
    uint32_t        m_uiRefCount : 31; // +0x08 bits 1-31
};
static_assert(sizeof(EnergyBar) == 0x0C);
static_assert(offsetof(EnergyBar, m_pClass) == 0x00);
static_assert(offsetof(EnergyBar, m_fEnergy) == 0x04);

} // namespace game
