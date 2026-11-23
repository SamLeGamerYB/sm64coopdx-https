# Gravity API

<badge>Design specification — not implemented yet</badge>

The Gravity API is the first planned module in the shared `sm64coopdx` Lua namespace. Its purpose is to provide real gravity control without requiring mods to rewrite Mario's physics in Lua.

## Planned namespace

```lua
sm64coopdx.gravity
```

## Planned functions

| Function | Purpose |
| --- | --- |
| `set_direction(x, y, z)` | Set the world gravity direction. The API normalizes the vector. |
| `set_strength(value)` | Set gravity acceleration strength. |
| `get_direction()` | Return the current direction as a vector. |
| `get_strength()` | Return the current strength. |
| `is_enabled()` | Report whether a custom gravity override is active. |
| `reset()` | Restore the game's default gravity. |

These names and signatures are provisional. The final API must use the project's established Lua conventions and validated vector conversion helpers.

## Example — intended usage

This example describes the target interface; it will not work until the native API is implemented.

```lua
-- Normal downward gravity
sm64coopdx.gravity.set_direction(0, -1, 0)
sm64coopdx.gravity.set_strength(4.0)

-- Restore normal game behavior when this mod's effect ends
sm64coopdx.gravity.reset()
```

## Semantics

- Direction is a finite, non-zero 3D vector and is normalized by the API.
- Strength is finite, non-negative, and bounded by a documented engine-safe maximum.
- The initial implementation targets world-scoped gravity. Player- and zone-scoped overrides are later extensions.
- World-scoped gravity is authoritative and synchronized automatically in multiplayer.
- Local-only visual effects must not silently change gameplay gravity.
- Resetting or unloading the owning mod restores the previous/default state safely.
- Invalid arguments return `nil, error_message` (or the final convention selected for the shared API) and do not partially modify state.

## Important engine requirement

A vector acceleration alone does not implement arbitrary gravity. Current Mario physics applies gravity through `apply_gravity()` in `src/game/mario_step.c`, while much of the movement and collision pipeline assumes Y-up coordinates. The complete feature must make movement, collision, floor/ceiling handling, camera-relative controls, and multiplayer replication agree on the same gravity frame.

Until that work is complete, this API must not claim that sideways, inverted, or diagonal gravity is fully supported.
