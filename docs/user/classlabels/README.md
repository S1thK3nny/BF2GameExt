# Class Labels

A `ClassLabel` is the first line of an ODF and decides what kind of object it is: `cannon`, `launcher`, `melee` and so on. BF2GameExt adds new ones. Each has its own page below with its properties, an example and its limits.

For which builds each one works on, see the [compatibility table](../../../README.md#compatibility).

## Weapon Classes

| ClassLabel | Description | Since |
|------------|-------------|-------|
| [`dualcannon`](dualcannon.md) | A `cannon` with a second gun in the other hand. Shots alternate between the two guns, per trigger pull or within a salvo. | 1.1.0 |

## What every GameExt ClassLabel has in common

- **It needs its own ODF.** `ClassLabel@GameExt` does nothing, because the class is decided before any property is read (see [GameExt-Only Values](../ODF_PROPERTIES.md#gameext-only-values)).
- **A game without BF2GameExt crashes** loading any level that contains one, so a mod that uses a GameExt ClassLabel requires the extension. `@GameExt` does not get around this: a `WeaponName@GameExt` pointing at one still packs that weapon into the level.
- **Multiplayer needs BF2GameExt on every machine** on a map that uses one. Maps that do not use a GameExt ClassLabel are unaffected, whoever has the extension installed.
