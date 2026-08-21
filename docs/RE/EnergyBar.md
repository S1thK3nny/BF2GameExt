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

| Field | Role | Set by |
|---|---|---|
| `m_fEnergyMax` | ceiling, applied on regen | ODF |
| `m_fEnergyMin` | how far NEGATIVE energy may go - the debt floor | **DERIVED, not an ODF param** |
| `m_fEnergyOverheat` | the level energy must RECOVER TO before spending is allowed again | ODF |

## EnergyMin is NOT an ODF parameter - it is derived from the costs

`EnergyBarClass::SetPropertyEnergyCost` (Phantom `0x00501A30`) is its only writer:

    if ( -m_fEnergyMin < cost )        // the XOR with 0x80000000 is a float negate
       m_fEnergyMin = -cost;

So `m_fEnergyMin` tracks **the negative of the largest single energy cost** registered against
the class. Confirmation that nothing else sets it:

  * `EnergyBarClass::SetProperty` (`0x005018E0`) handles three property hashes and writes only
    `m_fEnergyMax` (+0x00) and `m_fEnergyOverheat` (+0x08). It never touches +0x04.
  * `EnergyBarClass::Init` (`0x005017F0`) takes `(max, overheat)` - two floats. No min.

The effect is that the debt floor is **exactly one action deep, automatically**: `SpendEnergy`
allows a single overdraw and clamps to `m_fEnergyMin`, so you can always afford your most
expensive action once and can never be deeper in debt than that one action. Nobody tunes it.

Consequence worth knowing: adding one very expensive ability to a class **deepens the hole for
every other action on that class**, because the floor is shared and set by the maximum.

**Overheat always trips at zero.** It is hardcoded in `SpendEnergy` and is not configurable.
`EnergyMin` does not decide when you overheat, only how deep the hole gets.
`EnergyOverheat` is a RECOVERY threshold, not a trip point - it controls lockout duration.

## The behaviour that surprises people

**A spend that overheats you still succeeds.** The latch is tested BEFORE the subtraction, so
with 1 energy left a 50-cost action goes through, energy lands at -49 (clamped to `EnergyMin`),
and the NEXT attempt is refused. There is no minimum-energy check anywhere in the engine - only
the latch. This is the same mechanism behind soldiers being able to roll at almost no energy.

## The property path self-corrects

`EnergyBarClass::SetProperty` will not let you build the permanently-locked bar described below:

  * setting **max** below the current overheat pulls **overheat down** to match
  * setting **overheat** above max pushes **max up** to match
  * both clamp a negative value to 0

So the trap only exists if something writes the fields directly rather than through the property
path. If that ever happens, `EnergyOverheat > EnergyMax` locks the bar permanently - the latch
can never clear, and because the max clamp lives inside the same branch, energy also stops being
clamped to max.

**`EnergyOverheat <= 0` effectively disables overheat** - the latch clears on the first regen
tick.

One property hash (`0x5621511E`) stores `1 - value` into `m_fEnergyOverheat`, clamped, so it is
a FRACTION-style key rather than an absolute one. Two others (`0xA733FBF5`, `0xF03FFABB`) write
overheat directly and `0x97105D6A` writes max. The ODF key names are hashed and not present as
strings in the image, so the mapping from key name to hash has not been established.

Recovery uses `<=`, so reaching exactly `EnergyOverheat` clears the latch.

`gUnlimitedEnergyAll` bypasses `SpendEnergy` entirely.
