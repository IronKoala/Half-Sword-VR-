# Phase 4: the character moves with the player ("surgery")

Status 2026-11-16: written and compiled, **not yet run in the game**. Branch `claude/body-follow`, on top of
`claude/controller-input`. Background: `docs/phase3-motion-controls.md`, `docs/phase2-arm-research.md`.

## What was wrong, and why

Matthew's three complaints after phase 3: the character moves without him and not with him, the hand
(and so the sword) is not where his hand is, and the camera floats at the character's eyeballs so he
sees the inside of the head. All three come from one cause: the view rode on the simulated body, and
the body only followed poses.

What the arm probe's dump (2026-11-15) shows about the game's "muscles":

- They are the engine's physical animation component (`PhysicalAnimation` on the pawn, strength
  multiplier 2). The pawn keeps one strength per body part (`PA Strength Hand R` = 1000, `UA R` = 926,
  spine about 970, ...), an `Orientation Strength` of 1000 and an `Angular Strength` of 100.
- Those are the *turning* part of the spring between each simulated body and its place in the pose.
  Nothing in the pawn's variables sets the *positional* part. So no body part is ever pulled to a
  place: each bone is turned towards its pose angle and the hand ends up wherever the chain of bones
  leaves it. That is why the hand sat a steady 6 to 13 cm off even at rest, sagged under the sword, and
  trailed 20 to 50 cm on fast swings, however good the pose was.
- The character's arm is about as long as Matthew's (62 to 63 cm against 59 to 60 cm measured), so arm
  length was never the real problem. Scaling reach treated a symptom.
- The view was anchored to the simulated head bone with 0.06 s of smoothing, so every sway of the
  ragdoll moved the world, and leaning moved the view out of a head that stayed put.

## What the rework does

Four parts, each with its own switch in `HalfSwordVR.ini`.

1. **The view is the player's head** (`body_follow`, `stereo_device.cpp`). View = character position +
   a slow offset + the headset's movement since recentering, one to one. The slow offset is where the
   view must be for the eyes to sit in the character's real head; it is followed over
   `view_follow_seconds` (2 s) and the view is never more than `view_leash_cm` (30 cm) from the real
   head, so a knock-down still takes the view along. Ragdoll sway no longer moves the world.
2. **The upper body and head follow the headset** (`body_follow`, `apply_head` in `arm_control.cpp`).
   Before the arms are solved, the driver skeleton's upper body is swung about `spine_01` so that its
   head moves by the headset's displacement from its recentered place (`lean_follow`, at most
   `lean_max_deg`), and the head bone is turned like the headset (`head_turn`, at most
   `head_turn_max_deg` from the game's own pose). The game's muscles move the real body after it, so
   balance and strength stay the game's.
3. **The character's own head is not drawn in first person** (`hide_head`): `HideBoneByName` on every
   skinned mesh of the pawn (`hide_head_bone`, default `head`) and the hair hidden; shown again in the
   third-person view.
4. **The hands are pulled to the controllers** (`hand_pull`, "pulling the hands" in `arm_control.cpp`).
   The two hand bodies get the positional part of the same engine spring, in the world
   (`hand_pull_strength` 1500, `hand_pull_damping` 60, `hand_pull_max_force` 0 = no limit), and the
   hand's own turning strength times `hand_turn_scale` (2). The driver's hand bone is written at the
   controller's true place, with the arm solved from the *physical* shoulder, and the reach scaling of
   phase 3 is skipped. Only the hand is pulled: the sword hangs from the hand by the game's own grip,
   so its mass still slows the hand and its inertia still makes the blade trail. Upper arm and forearm
   keep the game's strengths.

How the pull is done without engine source in the mod: the engine keeps the per-body settings in
`UPhysicalAnimationComponent::DriveData`, which is not reflected. It sits right after the reflected
`SkeletalMeshComponent` pointer; the mod finds it by shape (every entry starts with a bone name, one of
them `hand_r`), reads the struct layout from the reflected `ApplyPhysicalAnimationSettings` parameter,
logs every entry once ("Pull:" lines, the first hard facts on the game's muscle settings), and changes
entries only through that engine function, from inside the mesh tick (after the game's own script, before
the engine uses the settings). Whatever is in an entry that the mod did not write is taken as the game's
own value, kept, and put back when the pull is off (menu, Page Up, controller lost).

## Log lines to read after a test

- `Pull: the game's muscle settings found ...` and one line per body: whether the game drives each
  body in the world or relative to its parent, and its strengths.
- `Body: head bone axes learned ...`, `Body: the character's own head is hidden ...`.
- Once a second: `Body: follow on | headset N cm from its recentered place | view N cm from the
  character's real eyes | hand pull right on left on (spring anchored A / B cm from the controller;
  settings written K times)`. A and B should be near 0: they compare the engine's own spring anchor
  with where the mod meant to put it. K growing every second means the game rewrites its settings
  every frame (still works, noted for later).
- `Arms: ... right hand sent err X cm Y deg`: X is now the true distance between the simulated hand
  and the controller's place. Phase 3 had 6 to 13 cm at rest and 20 to 50 cm in swings.

## Tuning (all re-read while the game runs)

| If | Change |
| --- | --- |
| Hand still trails | `hand_pull_strength` up (3000, 6000), damping about 2 x sqrt(strength / 2) |
| Hand shakes or buzzes | strength down, or damping up |
| Heavy weapons feel weightless | `hand_pull_max_force` above 0 (start near 20000), or strength down |
| Blade angle lags the hand | `hand_turn_scale` up (3 to 5) |
| View drifts when standing off-centre | recenter (Home), or `view_follow_seconds` up / 0 |
| Character stumbles when leaning | `lean_follow` down (0.5), `lean_max_deg` down |
| Something feels wrong | switch parts off one by one: `hand_pull=0`, `body_follow=0`, `hide_head=0` |

## Known limits of this first cut

- Ducking: the upper body leans but the legs do not crouch with the headset; a deep duck is limited
  by the leash and the view then drifts back up. Needs the game's crouch driven from headset height.
- Walking around the room leans the body rather than walking the character; recenter after moving.
- The torso does not turn with the player's shoulders (only the head turns; nothing tracks the chest).
- Two-handed grip: both hands are pulled to their own controllers.
- The drive may be mass-independent or not (engine setting unknown from outside); the strengths above
  are a first guess for either case.
- Hand pull makes the player's arms stronger in position than the enemies'. Matthew asked for the
  hand truly at his hand; `hand_pull_max_force` is the balance knob.
