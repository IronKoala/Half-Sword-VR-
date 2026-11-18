# Phase 4 tests: controls and weapons in your hands

## Stick walking and turning (first part of Phase 4)

- Left stick walks. It goes through the game's own movement (`AddMovementInput` and "Movement Input Vector", the same path as its gamepad and WASD handlers, Willie_BP @768801, @768447, @760861), so his walk speed, stamina, weight and injuries still apply, and walls still stop you. By default it walks toward where you look.
- Right stick turns you (view and body together), smoothly by default. It needs `pose_body_turn=1`.
- The log line "Walk:" shows the stick, the direction and his speed once a second.

| Setting | Default | What it does |
|---|---|---|
| `pose_stick_walk` | 1 | 1 = the left stick walks. |
| `pose_move_relative` | head | `head` = the stick walks toward where you look; `body` = toward where his body faces. |
| `pose_stick_turn` | smooth | `smooth`, `snap` or `off`. |
| `pose_stick_turn_dps`, `pose_snap_turn_deg` | 150, 30 | Smooth turn speed at full stick; snap size. |
| `pose_stick_dead_zone` | 0.15 | How far a stick moves before it counts. |

While you walk with the stick the capsule hold no longer clears his speed (it did, to stop the game's own pull, which also stopped walking); it clears it again half a second after you let go.
