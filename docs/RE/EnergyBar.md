# EnergyBar: what EnergyMax / EnergyMin / EnergyOverheat actually do

`EnergyBarClass` is three floats (Phantom, 12 bytes):

    +0x00  float m_fEnergyMax
    +0x04  float m_fEnergyMin
    +0x08  float m_fEnergyOverheat

`EnergyBar` (the instance) is also 12:

    +0x00  EnergyBarClass* m_pClass
    +0x04  float m_fEnergy
    +0x08  uint:1  m_bOverheat   (bit 0)
    +0x08  uint:31 m_uiRefCount  (bits 1..31)

## The two functions that define the semantics

`EnergyBar::SpendEnergy(float cost)` - Phantom `0x00501A70`:

    if (!gUnlimitedEnergyAll && cost > 0.0) {
       if (m_bOverheat) return false;            // refuse only if ALREADY latched
       m_fEnergy -= cost;
       if (m_fEnergy < 0.0) {                    // trips at ZERO, not at EnergyMin
          m_bOverheat = 1;
          if (m_fEnergy < m_pClass->m_fEnergyMin)
             m_fEnergy = m_pClass->m_fEnergyMin;
       }
    }
    return true;

`EnergyBar::AddEnergy(float amount)` - Phantom `0x00501780`:

    m_fEnergy += amount;
    if (m_pClass->m_fEnergyOverheat <= m_fEnergy) {
       m_bOverheat = 0;                          // clear the latch
       if (m_fEnergy > m_pClass->m_fEnergyMax)
          m_fEnergy = m_pClass->m_fEnergyMax;
    }

## What each parameter means

| Param | Role |
|---|---|
| `EnergyMax` | ceiling, applied on regen |
| `EnergyMin` | how far NEGATIVE energy may go - the debt floor, normally a negative number |
| `EnergyOverheat` | the level energy must RECOVER TO before spending is allowed again |

**Overheat always trips at zero.** It is hardcoded in `SpendEnergy` and is not configurable.
`EnergyMin` does not decide when you overheat, only how deep the hole gets.
`EnergyOverheat` is a RECOVERY threshold, not a trip point - it controls lockout duration.

## The behaviour that surprises people

**A spend that overheats you still succeeds.** The latch is tested BEFORE the subtraction, so
with 1 energy left a 50-cost action goes through, energy lands at -49 (clamped to `EnergyMin`),
and the NEXT attempt is refused. There is no minimum-energy check anywhere in the engine - only
the latch. This is the same mechanism behind soldiers being able to roll at almost no energy.

## Configuration traps

**`EnergyOverheat > EnergyMax` locks the bar permanently.** The latch can never clear, and
because the max clamp lives inside the same branch, energy also stops being clamped to max.

**`EnergyOverheat <= 0` effectively disables overheat** - the latch clears on the first regen
tick.

Recovery uses `<=`, so reaching exactly `EnergyOverheat` clears the latch.

`gUnlimitedEnergyAll` bypasses `SpendEnergy` entirely.
