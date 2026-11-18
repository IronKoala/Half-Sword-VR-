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

## Weapon in hand on a strength-limited spring (step 1)

The game holds a weapon with a physics joint between the weapon and the hand bone, locked once the weapon is seated (Willie_BP @454086-455803). With the hand posed from your controller a locked joint makes any weapon weightless. Right after his tick the mod turns that seated joint into a spring toward your hand with a force limit, so a weapon heavier than your strength lags and sags. The game's own grip slip and stamina drain read this joint's force, so they keep working. The controller buzzes when the weapon is more than 2 cm from your hand. The log line "Grip:" shows each hand's weapon mass, joint force, estimated gap and the game's slip rate twice a second.

| Setting | Default | What it does |
|---|---|---|
| `pose_grip_spring` | 1 | 1 = spring, 0 = the game's locked joint. |
| `pose_grip_strength_kg` | 20 | The most weight the grip holds up, in kg. Turning strength is this times `pose_grip_radius_cm`. |
| `pose_grip_hz`, `pose_grip_damping` | 8, 1 | How briskly the weapon follows (natural frequency) and how much wobble is damped (1 = none). |
| `pose_grip_radius_cm` | 30 | The lever used for turning stiffness and strength. |
| `pose_grip_buzz` | 0.6 | Buzz strength for the gap (0 = off). |

For unattended tests the dev_script step `hsvr.give_weapon <class path>` equips a weapon through the game's own "Set Up Right Hand Weapon" (the scripted Free Mode start has none).

Next: your strength from the game's stats instead of a fixed number, the hand staying on the weapon with a ghost hand at the controller, two hands, grab button, slide grip.
