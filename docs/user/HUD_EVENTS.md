# Custom HUD events

## Reticule horizon levelling

Bind this on the reticule's rotation pivot:

```text
EventRotation("player1.reticule.horizonRotation")
```

The event carries `(0, 0, angleInDegrees)`, aligning the reticule's up direction
with projected world-up. It reads the active camera, not the unit or vehicle pose,
so it follows the view in first and third person, including 90-degree banks and
inverted flight. It does not change aim, position, size, mesh, visibility or alpha.
There is no INI setting; the binding is the opt-in. Supported on Modtools, Steam
and GOG, for the first local viewport (`player1`).

### Keep rotation and artwork scaling on separate groups

Use an **unscaled pivot** at the reticule position. Keep the existing artwork's
scale and authored rotation inside a child group. Native `EventRotation` rebuilds
the transform from its current basis lengths; repeatedly rotating a group with
unequal X/Y scales can alter its proportions. A nonuniformly scaled ancestor can
also distort the final angle. The event replaces the pivot's rotation rather
than adding to a static `Rotation()` value.

For the existing `player1reticule_group`, retain its position/enable/alpha events
on the outer group, move its `Scale(0.750000,0.800000,0.750000)` into a new child,
and place all its existing reticule artwork inside that child:

```text
Group("player1reticule_group")
{
    EventPosition("player1.weapon1.reticule.position")
    Position(0.500000,0.500000,0,"Viewport")
    Scale(1,1,1)
    EventRotation("player1.reticule.horizonRotation")
    EventEnable("player1.weapon1.reticule.position")
    EventDisable("player1.weapon1.reticule.disable")
    EventAlpha("player1.reticule.alpha")

    Group("player1reticule_artwork")
    {
        Position(0,0,0,"Viewport")
        Scale(0.750000,0.800000,0.750000)
        // Existing reticule Model3D blocks go here, unchanged.
    }
}
```

This is a layout example, not a replacement containing your artwork. If only one
part should level with the horizon, put only that part inside the rotating pivot.
Do not put a screen-centred reticule's position on both parent and child: that
would translate it twice and make it orbit the wrong point.

Straight up/down has no unique horizon. Inside approximately 0.57 degrees of
vertical the event holds its last reliable direction, resuming beyond about
1.15 degrees to avoid jitter. A new camera/mission starts from zero if already
vertical. Crossing the pole can still reverse world-up by 180 degrees; that is
inherent in following world-up rather than tracking a continuous flight roll.

### Verification

The degree payload and native handler were checked against all three supported
executables. Standalone tests cover bank, pitch, yaw, inversion, projection aspect,
zoom, invalid data and vertical-view stability, including 20,000 random projected
world-up comparisons. DLL build and in-game verification are still required.

## Floating target bars

`player1.weapon1.target.position` and `player1.weapon2.target.position` publish
viewport-relative target anchors. Bind with `EventPosition`, not `EventEnable`.
On death or removal, the bar stays at its last screen position for the existing
HUD fade instead of dropping down onto the corpse. Living targets still move
normally during fade-out, and a newly selected target gets its own position.
See [Floating Target Bar](FEATURES.md) for placement and latch behaviour.
