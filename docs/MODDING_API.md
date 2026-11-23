# SM64CoopDX Modding API

This document describes the proposed shared API namespace for engine-level modding features in this fork. It is an implementation plan, not a claim that the functions below are already available.

## Goals

- Expose engine features through one discoverable Lua namespace.
- Keep feature modules independent: a mod can use one module without enabling unrelated modules.
- Reuse existing SM64CoopDX Lua hooks and engine systems where possible.
- Make multiplayer behavior automatic for features that represent shared world state.
- Preserve compatibility with the existing `network` API. Gameplay APIs must not require HTTPS/WSS permission or an Internet connection.
- Validate Lua arguments and keep state scoped to the mod or world that owns it.

## Public Lua namespace

The proposed namespace is `sm64coopdx`, with one table per feature:

| Module | Responsibility |
| --- | --- |
| `sm64coopdx.gravity` | Gravity direction, strength, modes, and zones |
| `sm64coopdx.liquid` | Water/lava levels, properties, and custom liquid behavior |
| `sm64coopdx.screen_fx` | Screen and post-processing effects |
| `sm64coopdx.video` | In-game video playback |
| `sm64coopdx.camera` | Camera control |
| `sm64coopdx.hud` | Custom HUD elements |
| `sm64coopdx.player` | Player state and player-scoped properties |
| `sm64coopdx.object` | Object helpers and lifecycle |
| `sm64coopdx.audio` | Custom audio playback and control |
| `sm64coopdx.environment` | World ambience and environment settings |

The namespace and function signatures are provisional until the first native module is implemented and tested.

## Module rules

1. **Independent modules:** each module owns its implementation and documentation.
2. **Shared validation:** reject NaN, infinity, out-of-range values, invalid vectors, and unsupported object/player references.
3. **Lifecycle cleanup:** state owned by a mod must be cleared when that mod unloads or reloads.
4. **No implicit Internet access:** these APIs are local engine/gameplay APIs. The existing `network` namespace remains separate and permission-gated.
5. **Capability checks:** modules expose an availability check if a feature is platform- or renderer-dependent.
6. **Versioning:** new functions are additive where possible. Breaking changes require an API version note and migration guidance.

## Multiplayer synchronization policy

Synchronization is automatic when a feature changes shared world state and the engine can replicate it safely.

- **World-scoped state:** the host/authoritative game instance owns changes and replicates the accepted state to other players.
- **Player-scoped state:** the owning player publishes their state; the game validates and distributes it.
- **Local presentation:** effects that only affect one player's display remain local unless a module explicitly offers a synchronized mode.
- **No duplicate networking:** modules should use SM64CoopDX's existing multiplayer synchronization mechanisms, not HTTPS/WSS.
- **Conflict handling:** the implementation must define ownership and reset behavior when multiple mods request the same global property. The initial policy should be deterministic and documented; it must not depend on Lua callback order accidentally.

Automatic synchronization does not mean every API call is broadcast blindly. Values must be validated, bounded, and replicated only when the feature's semantics require it.

## Gravity API — first module

### Scope

The target is a real gravity system, not just a low-gravity example that edits `m.vel.y`.

The long-term feature set is:

- A normalized 3D direction vector.
- A configurable acceleration strength.
- Enable, disable, reset, and query operations.
- World-wide gravity and, later, player- or zone-scoped overrides.
- Automatic multiplayer replication for shared world gravity.
- A safe fallback to normal SM64 gravity when a mod unloads or resets its setting.

### Proposed Lua surface

The following signatures are design examples and are not implemented yet:

```lua
-- Set the world gravity direction.
-- The vector is normalized by the API.
sm64coopdx.gravity.set_direction(0, -1, 0)

-- Set gravity acceleration strength.
sm64coopdx.gravity.set_strength(4.0)

-- Read the current world settings.
local direction = sm64coopdx.gravity.get_direction()
local strength = sm64coopdx.gravity.get_strength()

-- Restore the game's default gravity.
sm64coopdx.gravity.reset()
```

The implementation should return `true` on success or `nil, error_message` on invalid input or an unavailable feature. Exact return conventions will be aligned with the existing Lua API.

### Engine integration and technical constraints

The current engine applies Mario's standard gravity in `src/game/mario_step.c`, in `apply_gravity()`. That function also contains action-specific gravity rules (twirling, cannon launch, long jump, lava boost, metal water, wing cap, and others). The existing `ACTION_HOOK_GRAVITY` is tied to registered Mario actions; it is not, by itself, a world-wide gravity-vector API.

Changing only `m.vel.y` is insufficient for arbitrary 3D gravity. The movement and collision pipeline assumes the normal Y-up world in multiple places. A correct implementation must account for the gravity-relative movement frame, floor/ceiling/wall classification, landing and step logic, camera/input behavior, and multiplayer state. The feature must not be advertised as supporting sideways or inverted gravity until those systems have been addressed and tested.

### Implementation phases

1. **Audit:** map every Y-up assumption in Mario movement, collision, camera, and network synchronization.
2. **Core state:** define validated world-gravity state (direction + strength), default/reset behavior, and ownership.
3. **Physics frame:** introduce gravity-relative acceleration and movement/collision handling without breaking normal Y-up behavior.
4. **Lua binding:** expose the `sm64coopdx.gravity` functions and document argument validation/errors.
5. **Multiplayer:** replicate authoritative world-gravity state automatically and handle late joins, host changes, and mod unloads.
6. **Testing:** verify default gravity parity, all six cardinal directions, diagonal directions, action-specific movement, water/lava interactions, level transitions, late joins, and reset behavior.

A vertical-strength-only implementation may be used as an internal development step, but it must not be presented as full vector gravity.

## Module documentation

Each module will have a dedicated guide and runnable Lua examples under `docs/lua/examples/`.

- [Gravity API](lua/GRAVITY_API.md) — initial design specification; native implementation pending.
- Liquid API — planned.
- Screen FX API — planned.
- Video API — planned.
